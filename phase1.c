/* ********************************
 * FILE:       phase1.c
 * AUTHOR:     Huiqi He, Siwen Wang
 * COURSE:     CSC452 FALL 2022
 * ASSIGNMENT: OS PROJECT PHASE 1
 * ********************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <usloss.h>
#include "phase1.h"

/* -------------------------------------------------------- Global Variables */
#define MINPRIORITY		7
#define EMPTY			0
#define READY			1
#define BLOCKED			2
#define DYING			3
#define DEAD			4
#define CODEJOIN		20
#define CODEZAP			21
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------- Helper Functions */
static void dispatcher();
void enqueue(int slot);
int dequeue(int index);
int peek(int index);
int init(char *str);
int sentinel(char *str);
void checkKernel();
void checkKernelMode(char *fun);
void restoreInterrupt(int PSR);
void disableInterrupt();
void enableInterrupt();
void newProcess(int slot, char *name, int PID, int priority,
					int (*func)(char *), char *arg,
					int stacksize, int parentSlot);
void deleteProcess(int slot);
void launcher();

static void clockHandler(int dev,void *arg)
{
   // call the dispatcher if the time slice has expired
    timeSlice();

    phase2_clockHandler();
}
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------------- Structures */
// Process Table Entry

// Queue implemented as linked list
typedef struct queue{
	int PID;
	struct queue *next;
} queue;

typedef struct PTE{
	char name[MAXNAME];
	int PID;
	int priority;
	char arg[MAXARG];
	int(*func)(char *);
	int(*testCaseMain)(void);	/*	for testcase_main only	*/
	int stacksize;
	USLOSS_Context context;
	struct PTE *parent;
	struct PTE *firstChild;
	struct PTE *lastChild; // TO MATCH TEST CASES
	struct PTE *olderSibling;
	struct PTE *youngerSibling;
	int numChildren;
	int state;
	int runnableStatus;
	int quitStatus;
	int isZapped; /* 0 if not, 1 otherwise */
	queue *zappers;
	int numZapped;
	int CPUTime;
	int currTimeSliceStart;
	int read; // whether this process is dead and read by another process or not
} PTE;
/* ------------------------------------------------------------------------- */

/* --------------------------------------------------------------- Variables */
// The PID of the currently running process
int currProcess;
// All processes in this array
struct PTE procTable[MAXPROC];
// Help assign new PID to new process
int currPID;
// Keep track of the total alive process in the table
int processTableCount;
// Each entry is a linked list, index 0 is priority 1, etc
// Two dimensional because we want front and back of the linked list
queue *priorityQueue[MINPRIORITY][2];
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------ Required Functions */
/*
 * Initialize all variables and set up init process
 */
void phase1_init(void){
	checkKernelMode("phase1_init");
	currProcess = -1;
	// initialize process table with all 0 entries
	memset(procTable, 0, MAXPROC * sizeof(PTE)); 
	// initialize priority queues
	for (int i = 0; i < MINPRIORITY; i++) {
		priorityQueue[i][0] = NULL;
		priorityQueue[i][1] = NULL;
	}
	// set up init
	currPID = 1;
	newProcess(currPID, "init", currPID, 6, init, "", USLOSS_MIN_STACK, 0);
	currPID++;
	processTableCount = 1;
	
	USLOSS_IntVec[USLOSS_CLOCK_INT] = clockHandler;
}

/*
 * Create new process, then call dispatcher
 * @return:		-2, if stacksize is less than USLOSS_MIN_STACK
 * 				-1, if no empty slots in the process table,
 * 					priority out of range, startFunc or name are NULL,
 * 					or name too long
 * 				>0, PID of the new process
 */
int fork1(char *name, int(*func)(char *), char *arg, int stacksize, int priority) {
	// check kernel mode and disable interrupt
	checkKernelMode("fork1");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// make sure all inputs are valid
	if (stacksize < USLOSS_MIN_STACK) return -2;
	if (strlen(name) > MAXNAME || name == NULL
			|| (arg != NULL && strlen(arg) > MAXARG) 
			|| priority < 1 || priority > 5
			|| processTableCount >= MAXPROC) {
		return -1;
	}
	// find empty spot on the process table
	while (1) {
		if (procTable[currPID % MAXPROC].state != EMPTY)
			currPID++;
		else break;
	}
	int slot = currPID % MAXPROC;
	// set up the process
	newProcess(slot, name, currPID, priority, func, arg, stacksize, currProcess);
	currPID++;
	// call dispatcher, parent run first
	if (priority < procTable[currProcess % MAXPROC].priority)
		dispatcher();
	// restore interrupt
	restoreInterrupt(currPSR);
	return procTable[slot].PID;
}

/*
 * Block the current process and wait on one child to die
 * May block and context switch
 * @return:		-2, if the process does not have any children
 * 				>0, PID of the child joined-to
 */
int join(int *status) {
	// check kernel mode and disable interrupt
	checkKernelMode("join");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check the children
	int slot = currProcess % MAXPROC;
	PTE *currChild = procTable[slot].lastChild;
	if (procTable[slot].numChildren == 0) {
		restoreInterrupt(currPSR);
		return -2;
	}
	int deadPID;
	for (; currChild != NULL;) {
		// found a dead child, return immediately
		if (currChild -> state == DYING || currChild -> state == DEAD) {
			*status = currChild -> quitStatus;
			deadPID = currChild -> PID;
			// clear out this entry on the process table
			procTable[currChild -> PID % MAXPROC].read = 1;
			deleteProcess(currChild -> PID % MAXPROC);
			// return quit status
			restoreInterrupt(currPSR);
			return deadPID;
		}
		currChild = currChild -> olderSibling;
	}

	// block the current process
	blockMe(CODEJOIN);
	// check if a child has died
	currChild = procTable[slot].lastChild;
	for (; currChild != NULL;) {
		if (currChild -> state == DYING || currChild -> state == DEAD) {
			*status = currChild -> quitStatus;
			deadPID = currChild -> PID;
			// clear out this entry on the process table
			procTable[currChild -> PID % MAXPROC].read = 1;
			deleteProcess(currChild->PID % MAXPROC);
			break;
		}
		currChild = currChild -> olderSibling;
	}
	// return quit status
	restoreInterrupt(currPSR);
	return deadPID;
}

/*
 * Marked the current process as dead and call dispatcher
 * It also wakes up everyone joined or zapped on this process
 */
void quit(int status) {
	// check kernel mode and disable interrupt
	checkKernelMode("quit");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// mark current process as dead
	int slot = currProcess % MAXPROC;
	procTable[slot].state = DYING;
	procTable[slot].quitStatus = status;
	if (procTable[slot].numChildren != 0) {
		USLOSS_Console("ERROR: Process pid %d called quit() while it still had children.\n", currProcess);
		USLOSS_Halt(1);
	}
	// check join and zap and wake up everyone
	if (procTable[slot].parent -> state == BLOCKED) 
		unblockProc(procTable[slot].parent -> PID);
	if (procTable[slot].isZapped) {
		for ( ; procTable[slot].zappers != NULL;) {
			queue *temp = procTable[slot].zappers;
			procTable[slot].zappers = procTable[slot].zappers -> next;
			if (procTable[(temp->PID) % MAXPROC].state == BLOCKED)
				unblockProc(procTable[(temp->PID) % MAXPROC].PID);
		}
	}
	procTable[slot].state = DEAD;
	mmu_quit(currProcess);
	// call dispatcher
	dispatcher();
	restoreInterrupt(currPSR);
}

/*
 * Zap the given process
 * @return: 	0, the zapped process has called quit()
 */
int zap(int pid) {
	// check kernel mode and disable interrupt
	checkKernelMode("zap");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for error
	if (pid <= 0) {
		USLOSS_Console("ERROR: Attempt to zap() a PID which is <=0.  other_pid = %d\n", pid);
		USLOSS_Halt(1);
	} else if (procTable[pid % MAXPROC].state == EMPTY) {
		USLOSS_Console("ERROR: Attempt to zap() a non-existent process.\n");
		USLOSS_Halt(1);
	} else if (procTable[pid % MAXPROC].state == DYING) {
		USLOSS_Console("ERROR: Attempt to zap() a process that is already in the process of dying.\n");
		USLOSS_Halt(1);
	} else if (procTable[pid % MAXPROC].state == DEAD) {
		USLOSS_Console("ERROR: Attempt to zap() a process that is already in the process of dying.\n");
		USLOSS_Halt(1);
	} else if (pid == 1) {
		USLOSS_Console("ERROR: Attempt to zap() init.\n");
		USLOSS_Halt(1);
	} else if (pid == currProcess) {
		USLOSS_Console("ERROR: Attempt to zap() itself.\n");
		USLOSS_Halt(1);
	} else if (procTable[pid % MAXPROC].PID != pid) {
		USLOSS_Console("ERROR: Attempt to zap() a non-existent process.\n");
		USLOSS_Halt(1);
	}
	// return 0 if already quit
	if (procTable[pid % MAXPROC].state == DEAD) {
		restoreInterrupt(currPSR);
		return 0;
	}
	// Zap
	procTable[pid % MAXPROC].isZapped = 1;
	procTable[pid % MAXPROC].numZapped++;
	queue *newZapper = malloc(sizeof(queue));
	if (newZapper == NULL) {
		USLOSS_Trace("Error: Out of memory! \n");
		USLOSS_Halt(1);
	}
	newZapper -> next = NULL;
	newZapper -> PID = currProcess;
	if (procTable[pid % MAXPROC].zappers == NULL) 
		procTable[pid % MAXPROC].zappers = newZapper;
	else {
		int newPriority = procTable[newZapper -> PID % MAXPROC].priority;
		queue *curr = procTable[pid % MAXPROC].zappers;
		if (newPriority < procTable[curr -> PID % MAXPROC].priority) {
			newZapper -> next = curr;
			procTable[pid % MAXPROC].zappers = newZapper;
		} else {
			for (; curr != NULL; curr = curr -> next) {
				if (curr -> next == NULL) {
					curr -> next = newZapper;
					break;
				} else if (newPriority < procTable[curr -> next -> PID % MAXPROC].priority) {
					newZapper -> next = curr -> next;
					curr -> next = newZapper;
					break;
				} 
			}
		}
	}
	blockMe(CODEZAP);
	restoreInterrupt(currPSR);
	if (procTable[pid % MAXPROC].state >= DYING || procTable[pid % MAXPROC].state == EMPTY) 
		return 0;
	else return 1;
}

/*
 * Check if the process itself was zapped
 * @return:		0, the calling process is not zapped
 * 				1, the calling process is zapped already
 */
int isZapped(void) {
	checkKernelMode("isZapped");
	return procTable[currProcess % MAXPROC].isZapped;
}

/*
 * Return the PID of the currently running process
 */
int getpid() {
	checkKernelMode("getpid");
	return currProcess;
}

/*
 * Print out the process information
 */
void dumpProcesses() {
	// check kernel mode and disable interrupt
	checkKernelMode("dumpProcesses");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	USLOSS_Console(" PID  PPID  NAME              PRIORITY  STATE\n");
	for (int i = 0; i < MAXPROC; i++) {
		if (procTable[i].state == EMPTY) continue;
		int pid = procTable[i].PID;
		int ppid;
		if (pid == 1) ppid = 0;
		else ppid = procTable[i].parent -> PID;
		char *name = procTable[i].name;
		int priority = procTable[i].priority;
		USLOSS_Console("%4d  %4d  %-17s %-10d", pid, ppid, name, priority);
		if (currProcess == pid) USLOSS_Console("Running\n", procTable[i].quitStatus);
		else if (procTable[i].state == READY) USLOSS_Console("Runnable\n");
		else if (procTable[i].state == DYING || procTable[i].state == DEAD) USLOSS_Console("Terminated(%d)\n", procTable[i].quitStatus);
		else if (procTable[i].state == BLOCKED) {
			USLOSS_Console("Blocked");
			if (procTable[i].runnableStatus == CODEJOIN) USLOSS_Console("(waiting for child to quit)\n");
			else if (procTable[i].runnableStatus == CODEZAP) USLOSS_Console("(waiting for zap target to quit)\n");
			else USLOSS_Console("(%d)\n", procTable[i].runnableStatus);
		}
	}
	restoreInterrupt(currPSR);
}

/*
 * Change runnableStatus for a process
 */
int blockMe(int block_status) {
	// check kernel mode and disable interrupt
	checkKernelMode("blockMe");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check error
    if (block_status <= 10) {
        USLOSS_Console("Error: status less than 10 \n");
        USLOSS_Halt(1);
    }
	// block the current process
	procTable[currProcess % MAXPROC].state = BLOCKED;
	procTable[currProcess % MAXPROC].runnableStatus = block_status;
	dispatcher();
	restoreInterrupt(currPSR);
    return 0;
}

/*
 * Unblock the given process
 * @return:		-2, the indicated process was not blocked, do not exist,
 * 					or is blocked on a status <= 10
 * 				 0, otherwise
 */
int unblockProc(int pid) {
	// check kernel mode and disable interrupt
	checkKernelMode("unblockProc");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check error
	if (procTable[pid % MAXPROC].state == EMPTY
			|| procTable[pid % MAXPROC].runnableStatus <= 10
			|| procTable[pid % MAXPROC].state != BLOCKED) {
		USLOSS_Console("Error when unblock\n");
		USLOSS_Halt(1);
	}
	// unblock
	procTable[pid % MAXPROC].state = READY;
	procTable[pid % MAXPROC].runnableStatus = 0;
	enqueue(pid);
	// call dispatcher
	dispatcher();
	// restore interrupt
	restoreInterrupt(currPSR);
	return 0;
}

/*
 * Return the wall-clock time in microseconds when
 * the current process started its time slice
 */
int readCurStartTime() {
	checkKernel(); // why this cannot be changed to checkKernelMode("readCurStartTime")?
	return procTable[currProcess % MAXPROC].currTimeSliceStart;
}

/*
 * Call dispatcher() if total running time is over 80us
 */
void timeSlice(void){
	// check kernel mode and disable interrupt
	checkKernelMode("timeSlice");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check time slice
	if (currentTime() - readCurStartTime() >= 80)
		dispatcher();
	// restore interrupt
	restoreInterrupt(currPSR);
}

/*
 * Return the total running time of the current process
 */
int readtime(void){
	checkKernelMode("readtime");
	return currentTime() - readCurStartTime();
}

/*
 * Return the current time wall-clock time in microseconds
 */
int currentTime() {
	// check kernel mode and disable interrupt
	checkKernelMode("currentTime");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// read time
	int now;
	int status = USLOSS_DeviceInput(USLOSS_CLOCK_DEV, 0, &now);
	if (status != USLOSS_DEV_OK) {
		USLOSS_Trace("Error with USLOSS_DeviceInput()\n");
	    USLOSS_Halt(status);
	}
	restoreInterrupt(currPSR);
	return now;
}

/*
 * Kick off the simulation and start init
 */
void startProcesses(void){
	// check kernel mode and disable interrupt
	checkKernelMode("startProcesses");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// call dispatcher, this will start init
	dispatcher();
	// restore interrupt
	restoreInterrupt(currPSR);
}

/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------- Helper Functions */

/*
 * Dispatcher
 */
static void dispatcher() {
	// check kernel mode and disable interrupt
	checkKernelMode("dispatcher");
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();

	if (procTable[currProcess % MAXPROC].state == DYING) return;

	// set up stuff for switch
	int toSwitch = 1;		// flag
	int oldPID = currProcess;
	int newPID = -1;
	int isBlocked;		// flag
	if (procTable[oldPID % MAXPROC].state == BLOCKED) isBlocked = 1;
	int timeSliceUp = 0;	// flag
	int currPriority = procTable[oldPID % MAXPROC].priority;
	// check to switch or not and how to switch
	if (oldPID == -1) newPID = 1;
	else {
		// check if there's a process with higher priority
		int i = 0;
		for (; i < currPriority - 1; i++) {
			if (peek(i) != -1) {
				newPID = dequeue(i);
				break;
			}
		}
		// if none found, check block and time slice
		if (newPID == -1) {
			// if blocked or dead, must switch
			if (isBlocked || procTable[oldPID % MAXPROC].state == DEAD) {
				for (; i < MINPRIORITY; i++) {
					if (peek(i) != -1) {
						newPID = dequeue(i);
						break;
					}
				}
				// if no process is found
				if (newPID == -1) {
					USLOSS_Trace("Error: no process to run\n");
					USLOSS_Halt(1);
				}
			// else if time slice is up
			} else if (currentTime() - readCurStartTime() >= 80) {
				timeSliceUp = 1;
				// if no other process to run on this priority too
				if (peek(currPriority - 1) == -1) 
					toSwitch = 0;
				// else must switch
				else newPID = dequeue(currPriority - 1);
			}
		}
	}
	if (newPID == -1) toSwitch = 0;
	if (toSwitch) {
		currProcess = newPID;
		mmu_switch(newPID);
		//dequeue(newPID);
		if ((isBlocked != 1) && (oldPID != -1) && (procTable[oldPID].state != DEAD)) enqueue(oldPID);	
		procTable[oldPID % MAXPROC].CPUTime += readtime();
		procTable[newPID % MAXPROC].currTimeSliceStart = currentTime();
		if (procTable[oldPID % MAXPROC].state == DEAD && procTable[oldPID % MAXPROC].read) {
			memset(&procTable[oldPID % MAXPROC], 0, 1 * sizeof(PTE));
			processTableCount--;
		}
		if (oldPID == -1)
			USLOSS_ContextSwitch(NULL, &procTable[newPID % MAXPROC].context);
		else {
			USLOSS_ContextSwitch(&procTable[oldPID % MAXPROC].context,
									&procTable[newPID % MAXPROC].context);
		}
	} else {
		if (isBlocked) {
			USLOSS_Trace("Error: current process is blocked ");
			USLOSS_Trace("but no other runnable process found\n");
			USLOSS_Halt(1);
		}
		if (timeSliceUp)
			procTable[currProcess % MAXPROC].currTimeSliceStart = currentTime();
	}
	// restore interrupt
	restoreInterrupt(currPSR);
}

/*
 * Add the ready process back on the queue based on its priority
 */
void enqueue(int pid) {
	if (pid == -1) {
		printf("error enqueue\n");
		return;
	}
	int slot = pid % MAXPROC;
	int priority = procTable[slot].priority;
	
	queue *newQueue = malloc(sizeof(queue));
	if (newQueue == NULL) {
		USLOSS_Trace("Error: Out of memory! \n");
		USLOSS_Halt(1);
	}
	newQueue -> PID = pid;
	newQueue -> next = NULL;

	if (priorityQueue[priority - 1][0] == NULL) {
		priorityQueue[priority - 1][0] = newQueue;
		priorityQueue[priority - 1][1] = newQueue;
	} else {
		priorityQueue[priority - 1][1] -> next = newQueue;
		priorityQueue[priority - 1][1] = newQueue;
	}
}

/*
 * Remove a process from a priority queue
 */
int dequeue(int index) {
	queue *temp = priorityQueue[index][0];
	int pid = temp -> PID;
	if (priorityQueue[index][1] -> PID == pid) {
		priorityQueue[index][0] = NULL;
		priorityQueue[index][1] = NULL;
	} else
		priorityQueue[index][0] = temp -> next;
	free(temp);
	return pid;
}

/*
 * Look at the first element in the queue and return its PID
 * Return -1 if the queue is empty
 */
int peek(int index) {
	if (priorityQueue[index][0] == NULL) return -1;
	else return priorityQueue[index][0] -> PID;
}

/*
 * Function for init process
 */
int init(char *str) {
	// for Bootstrap process?
	phase2_start_service_processes();
	phase3_start_service_processes();
	phase4_start_service_processes();
	phase5_start_service_processes();
	// first disable interrupt
	disableInterrupt();
	int currPSR = USLOSS_PsrGet();
	// set up sentinel
	if (currPID != 2) {
		USLOSS_Trace("Error: PID not 2 when sentinel is created\n");
		exit(1);
	}
	newProcess(2, "sentinel", currPID, 7, sentinel, "", USLOSS_MIN_STACK, 1);
	currPID++;
	// set up testcase_main
	char *name = "testcase_main";
	char *arg = "";
	strcpy(procTable[3].name, name);
	procTable[3].PID = currPID;
	procTable[3].priority = 5;
	strcpy(procTable[3].arg, arg);
	procTable[3].testCaseMain = testcase_main;
	procTable[3].stacksize = USLOSS_MIN_STACK;
	procTable[3].parent = &procTable[1];
	procTable[1].numChildren++;
	procTable[3].olderSibling = &procTable[2];
	procTable[3].youngerSibling = NULL;
	procTable[3].numChildren = 0;
	procTable[3].state = READY;
	procTable[3].runnableStatus = 0;
	procTable[3].isZapped = 0;
	procTable[3].zappers = NULL;
	procTable[3].numZapped = 0;
	procTable[3].CPUTime = 0;
	procTable[3].currTimeSliceStart = 0;
	mmu_init_proc(3);
	processTableCount++;
	USLOSS_ContextInit(&(procTable[3].context),
						malloc(USLOSS_MIN_STACK),
						procTable[3].stacksize,
						procTable[3].context.pageTable,
						launcher);
	currPID++;
	// add to the ready process queue
	enqueue(3);
	// restore interrupt
	restoreInterrupt(currPSR);
	dispatcher(); 
	// Infinite join(). Report error and halt simulation if no child left
	while (1) {
		int status;
		join(&status);
		if (status == -2) {
			USLOSS_Trace("Error: No child left for init, halt simulation\n");
			USLOSS_Halt(1);
		}
	}
	return 0;
}

/*
 * Function for sentinel process
 */
int sentinel(char *str) {
	while (1) {
		if (phase2_check_io() == 0) {
			USLOSS_Console("DEADLOCK DETECTED!  All of the processes have blocked, but I/O is not ongoing.\n");
			USLOSS_Halt(1);
		}
		USLOSS_WaitInt();
	}
	return 0;
}

/*
 * Check if the Current mode bit on the PSR is 1
 */
void checkKernel() {
	if (! (USLOSS_PsrGet() & 1)) {
		USLOSS_Console( "ERROR: Someone attempted to call kernel code while in user mode!\n");
		USLOSS_Halt(1);
	}
}

/*
 * Check if the Current mode bit on the PSR is 1
 */
void checkKernelMode(char *fun) {
	if (! (USLOSS_PsrGet() & 1)) {
		USLOSS_Console( "ERROR: Someone attempted to call %s while in user mode!\n", fun);
		USLOSS_Halt(1);
	}
}

/*
 * Restore to the old PSR
 */
void restoreInterrupt(int PSR) { 
	int re = USLOSS_PsrSet(PSR);
	if (re == USLOSS_ERR_INVALID_PSR) {
		USLOSS_Trace("Error: fail USLOSS_PsrSet()\n");
		USLOSS_Halt(1);
	}
}

/*
 * Read and Edit PSR to disable interrupt
 * Change interrupt bit to 0
 */
void disableInterrupt() { 
	int re = USLOSS_PsrSet(USLOSS_PsrGet() & 253); 
	if (re == USLOSS_ERR_INVALID_PSR) {
		USLOSS_Trace("Error: fail USLOSS_PsrSet()\n");
		USLOSS_Halt(1);
	}
}

/*
 * Turn on interrupt
 */
void enableInterrupt() { 
	int re = USLOSS_PsrSet(USLOSS_PsrGet() | 2); 
	if (re == USLOSS_ERR_INVALID_PSR) {
		USLOSS_Trace("Error: fail USLOSS_PsrSet()\n");
		USLOSS_Halt(1);
	}
}

/*
 * Create a new process on the process table
 * Then call USLOSS_ContextInit() to initialize the process
 */
void newProcess(int slot, char *name, int PID, int priority,
					int (*func)(char *), char *arg, int stacksize,
					int parentSlot) {
	strcpy(procTable[slot].name, name);
	procTable[slot].PID = PID;
	procTable[slot].priority = priority;
	if (arg == NULL) strcpy(procTable[slot].arg, "");
	else strcpy(procTable[slot].arg, arg);
	procTable[slot].func = func;
	procTable[slot].stacksize = stacksize;
	procTable[slot].testCaseMain = NULL;
	if (parentSlot == 0) {
		procTable[slot].parent = NULL;
		procTable[slot].firstChild = NULL;
		procTable[slot].lastChild = NULL; // TO MATCH TEST CASES
		procTable[slot].olderSibling = NULL;
		procTable[slot].youngerSibling = NULL;
	} else {
		procTable[slot].parent = &procTable[parentSlot];
		procTable[slot].firstChild = NULL;
		procTable[slot].lastChild = NULL; // TO MATCH TEST CASES
		procTable[slot].youngerSibling = NULL;
		if (procTable[slot].parent -> firstChild == NULL) {
			procTable[slot].olderSibling = NULL;
			procTable[slot].parent -> firstChild = &procTable[slot];
			procTable[slot].parent -> lastChild = &procTable[slot];
		} else {
			procTable[slot].parent -> lastChild -> youngerSibling = &procTable[slot];
			procTable[slot].olderSibling = procTable[slot].parent -> lastChild;
			procTable[slot].parent -> lastChild = &procTable[slot];
		}
	}
	procTable[slot].numChildren = 0;
	if (PID != 1) procTable[slot].parent -> numChildren ++;
	procTable[slot].state = READY;
	procTable[slot].runnableStatus = 0;
	procTable[slot].isZapped = 0;
	procTable[slot].zappers = NULL;
	procTable[slot].numZapped = 0;
	processTableCount++;
	if (PID > 1) mmu_init_proc(procTable[slot].PID);
	USLOSS_ContextInit(&(procTable[slot].context),
						malloc(stacksize),
						procTable[slot].stacksize,
						procTable[slot].context.pageTable,
						launcher);
	// add to the ready process queue
	enqueue(PID);
}

/*
 * Delete a dead process from the process table
 */
void deleteProcess(int slot) {
	PTE p = procTable[slot];
	if (p.parent -> firstChild -> PID == p.PID) {
		if (p.parent -> lastChild -> PID == p.PID) {
			p.parent -> firstChild = NULL;
			p.parent -> lastChild = NULL;
		} else {
			p.parent -> firstChild = p.parent -> firstChild -> youngerSibling;
			p.parent -> firstChild -> olderSibling = NULL;
		}
	} else if (p.parent -> lastChild -> PID == p.PID) {
		p.parent -> lastChild = p.olderSibling;
		p.parent -> lastChild -> youngerSibling = NULL;
	} else {
		if (p.youngerSibling != NULL) {
			p.olderSibling -> youngerSibling = p.youngerSibling;
			p.youngerSibling -> olderSibling = p.olderSibling;
		} else 
			p.olderSibling -> youngerSibling = NULL;
	}
	procTable[slot].parent -> numChildren --;
	if (procTable[slot].state == DEAD && procTable[slot].read) {
		memset(&procTable[slot], 0, 1 * sizeof(PTE));
		processTableCount--;
	}
}

/*
 * Our process wrapper or trampoline function
 */
void launcher() {
	// make sure interrupt is on
	enableInterrupt();
	// run the function
	int re;
	int slot = currProcess % MAXPROC;
	if (strcmp(procTable[slot].name, "testcase_main") != 0)
		re = procTable[slot].func(procTable[slot].arg);
	else
		re = procTable[slot].testCaseMain();
	// if it's testcase_main
	if (strcmp(procTable[slot].name, "testcase_main") == 0) {
		if (re != 0) {
			USLOSS_Trace("Error: some error was detected by the testcase. ");
			USLOSS_Trace("Return code is %d\n", re);
		}
		USLOSS_Halt(re);
	}
	quit(re);
}
/* ------------------------------------------------------------------------- */
