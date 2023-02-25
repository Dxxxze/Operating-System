/* ***********************************************
 * FILE:       part3.c
 * AUTHOR:     SIWEN WANG, HUIQI HE
 * COURSE:     CSC4XX FALL 2022
 * ASSIGNMENT: OS PROJECT PART 3
 * PURPOSE:	   USER PROCESS AND SYSCALL
 * ***********************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <usloss.h>
#include <usyscall.h>
#include "phase1.h"
#include "phase2.h"
#include "phase3.h"
#include "phase3_usermode.h"

/* -------------------------------------------------------- Global Variables */
#define EMPTY	 	0
#define OCCUPIED 	1
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------- Helper Functions */
int launcher(char *arg);
void spawn(systemArgs *args);
int spawnHelper(char *name, int (*func)(char *), char *arg, int stack_size, int priority, int *pid);
void wait(systemArgs *args);
int waitHelper(int *pid, int *status);
void terminate(systemArgs *args);
void terminateHelper(int status);
void getTimeofDay(systemArgs *args);
void getTimeofDayHelper(int *tod);
void cpuTime(systemArgs *args);
void cpuTimeHelper(int *cpu);
void getPID(systemArgs *args);
void getPIDHelper(int *pid);
void semCreate(systemArgs *args);
int semCreateHelper(int value, int *semaphore);
void semP(systemArgs *args);
int semPHelper(int semaphore);
void semV(systemArgs *args);
int semVHelper(int semaphore);
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------------- Structures */
typedef struct queue{
	int ID;
	struct queue *next;
} queue;

typedef struct shadowPTE{
	int PID;
	char *arg;
	int(*func)(char *);
	int status;
	int mailbox;
} shadowPTE;

typedef struct semaphore{
	int semaID;
	int value;
	int status;
	int mutex;
	queue *blockedHead;
	queue *blockedTail;
} semaphore;
/* ------------------------------------------------------------------------- */

/* --------------------------------------------------------------- Variables */
// shadow process table
shadowPTE shadowProcTable[MAXPROC];
semaphore semaphores[MAXSEMS];
// the mailbox for mutex, this mutex is over the whole shadowProcTable[]
int mutex;
// help assign semaphore ID
int currSema;
// total number of semaphore in the system
int numSema;
/* ------------------------------------------------------ Required Functions */
/*
 * Starting point, other code will call it but it does nothing
 */
void phase3_start_service_processes() {}

/*
 * Initialize all variables
 */
void phase3_init(void) {
	// initialize shadowProcTable and semaphores
	memset(shadowProcTable, 0, MAXPROC * sizeof(shadowPTE));
	memset(semaphores, 0, MAXPROC * sizeof(semaphore));
	// create the mailbox for mutex
	mutex = MboxCreate(1, 0);
	// initialize all other value
	currSema = 0;
	numSema = 0;
	// Register all the syscall handler
	systemCallVec[SYS_SPAWN] = spawn;
	systemCallVec[SYS_WAIT] = wait;
	systemCallVec[SYS_TERMINATE] = terminate;
	systemCallVec[SYS_GETTIMEOFDAY] = getTimeofDay;
	systemCallVec[SYS_GETPROCINFO] = cpuTime; // couldn't find SYS_CPUTIME anywhere
	systemCallVec[SYS_GETPID] = getPID;
	systemCallVec[SYS_SEMCREATE] = semCreate;
	systemCallVec[SYS_SEMP] = semP;
	systemCallVec[SYS_SEMV] = semV;
}
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------ Helper Functions */
/* 										   Some of these are also required */
/* 	   but not specified in phase3.h and won't be called by any test cases */
/*
 * The SYS_SPAWN handler
 * System Call Outputs: 
 * 		arg1: PID of the newly created process, or -1 if it could not be created
 * 		arg4: -1 if illegal values were given as input; 0 otherwise
 */
void spawn(systemArgs *args) {
	int pid;
	int re = spawnHelper(args -> arg5, args -> arg1, args -> arg2, 
						 (long) args -> arg3, (long) args -> arg4, &pid);
	if (re == -1) args -> arg1 = (void*)(long) -1;
	else args -> arg1 = (void*)(long) pid;
	args -> arg4 = (void*)(long) re;
} 

/*
 * The SYS_SPAWN handler helper or the actual SYS_SPAWN handler
 * Create a new process that will run entirely in user mode. If the process
 * returns, then the trampoline will automatically call Terminate() (still from
 * user mode) on behalf of this process, passing as the argument the value returned
 * from the user-main function
 * The PID of the child is returned using an out parameter pid
 * @return: 		0, if the child was successfully created
 * 				   -1, if not
 */
int spawnHelper(char *name, int (*func)(char *), char *arg, int stack_size, int priority, int *pid) {
	// lock
	MboxSend(mutex, NULL, 0);
	// fork the new process
	*pid = fork1(name, launcher, arg, stack_size, priority);
	if (*pid < 0) {
		*pid = -1;
		return -1;
	}
	// store info into shadowProcTable
	// if the child process is not filled yet
	if (shadowProcTable[*pid % MAXPROC].status == EMPTY) {
		shadowProcTable[*pid % MAXPROC].PID = *pid;
		shadowProcTable[*pid % MAXPROC].status = OCCUPIED;
		shadowProcTable[*pid % MAXPROC].mailbox = MboxCreate(0, 0);
	}
	shadowProcTable[*pid % MAXPROC].arg = malloc(MAXARG);
	if (arg == NULL) shadowProcTable[*pid % MAXPROC].arg = "";
	else strcpy(shadowProcTable[*pid % MAXPROC].arg, arg);
	shadowProcTable[*pid % MAXPROC].func = func;
	MboxCondSend(shadowProcTable[*pid % MAXPROC].mailbox, NULL, 0);
	// unlock
	MboxReceive(mutex, NULL, 0);
	return 0;
}

/*
 * Trampoline for user mode process
 */
int launcher(char *arg) {
	int pid = getpid();
	// if shadow proc table is not set up yet
	if (shadowProcTable[pid % MAXPROC].status == EMPTY) {
		shadowProcTable[pid % MAXPROC].status = OCCUPIED;
		shadowProcTable[pid % MAXPROC].PID = pid;
		shadowProcTable[pid % MAXPROC].mailbox = MboxCreate(0, 0);
		MboxReceive(shadowProcTable[pid % MAXPROC].mailbox, NULL, 0);
	}
	// disable kernel mode. This is the only exception we can call USLOSS_PsrSet()
	int re = USLOSS_PsrSet(USLOSS_PsrGet() & 254); // 1111 1110
	if (re == USLOSS_ERR_INVALID_PSR) {
		USLOSS_Trace("Error: fail USLOSS_PsrSet()\n");
		USLOSS_Halt(1);
	}
	// call the user main function in user mode
	int result = shadowProcTable[pid % MAXPROC].func(shadowProcTable[pid % MAXPROC].arg);
	// call Terminate() if it returns
	Terminate(result);
	return 0;
}

/*
 * The SYS_WAIT handler
 * System Call Outputs:
 * 		arg1: PID of the cleaned up process
 * 		arg2: status of the cleaned up process
 * 		arg4: -2 if no children; 0 otherwise
 */
void wait(systemArgs *args) {
	int pid, status;
	int re = waitHelper(&pid, &status);
	args -> arg1 = (void*)(long) pid;
	args -> arg2 = (void*)(long) status;
	args -> arg4 = (void*)(long) re;
}

/*
 * The SYS_WAIT handler helper or the actual SYS_WAIT handler
 * Calls join(), and returns the PID and status that join() provided
 * @return:	   -2, if join() returns -2
 * 				0, otherwise
 */
int waitHelper(int *pid, int *status) {
	*pid = join(status);
	if (*pid == -2) return -2;
	else return 0;
}

/*
 * The SYS_TERMINATE handler
 * System Call Outputs:		n/a
 */
void terminate(systemArgs *args) { terminateHelper((long) args -> arg1); }

/*
 * The SYS_TERMINATE handler helper or the actual SYS_TERMINATE handler
 * Terminates the current process, with the status specified
 * Unlike quit(), it is perfectly valid to call Terminate() while the process
 * still has children; Terminate() will call join(), over and over, 
 * until it returns -2 - and then call quit().
 */
void terminateHelper(int status) {
	while (1) {
		int childPID;
		int re = join(&childPID);
		if (re == -2) break;
	}
	memset(&shadowProcTable[getpid() % MAXPROC], 0, 1 * sizeof(shadowPTE));
	quit(status);
}

/*
 * The SYS_GETTIMEOFDAY handler
 * System Call Outputs:	
 * 		arg1: the value returned
 */
void getTimeofDay(systemArgs *args) {
	int tod;
	getTimeofDayHelper(&tod);
	args -> arg1 = (void*)(long) tod;
}

/*
 * SYS_GETTIMEOFDAY handler helper
 */
void getTimeofDayHelper(int *tod) { *tod = currentTime(); }

/*
 * The SYS_GETPROCINFO handler
 * System Call Outputs:	
 * 		arg1: the value returned
 */
void cpuTime(systemArgs *args) {
	int cpu;
	cpuTimeHelper(&cpu);
	args -> arg1 = (void*)(long) cpu;
}

/*
 * SYS_GETPROCINFO handler helper
 */
void cpuTimeHelper(int *cpu) { *cpu = readtime(); }

/*
 * The SYS_GETPID handler
 * System Call Outputs:	
 * 		arg1: the value returned
 */
void getPID(systemArgs *args) {
	int pid;
	getPIDHelper(&pid);
	args -> arg1 = (void*)(long) pid;
}

/*
 * SYS_GETPID handler helper
 */
void getPIDHelper(int *pid) { *pid = getpid(); }

/*
 * The SYS_SEMCREATE handler
 * System Call Outputs:	
 * 		arg1: ID of the newly created semaphore
 * 		arg4: -1 if illegal values were given as input, 
 * 			  or if no semaphores were available; 0 otherwise
 */
void semCreate(systemArgs *args) {
	int semaphoreID;
	int re = semCreateHelper((long) args -> arg1, &semaphoreID);
	if (re == -1) args -> arg1 = (void*)(long) 0;
	else args -> arg1 = (void*)(long) semaphoreID;
	args -> arg4 = (void*)(long) re;
}

/*
 * SYS_SEMCREATE handler or the actual SYS_SEMCREATE handler
 * Creates a new semaphore object
 * @return:		   -1, if the initial value is negative or if no semaphores are available
 * 					0, otherwise
 */
int semCreateHelper(int value, int *semaphore) {
	// error checking
	if (numSema >= MAXSEMS || value < 0) return -1;
	// find open spot on the array
	int index = currSema % MAXSEMS;
	for (; ; index++) 
		if (semaphores[index].status == EMPTY) break;
	semaphores[index % MAXSEMS].semaID = index % MAXSEMS;
	semaphores[index % MAXSEMS].value = value;
	semaphores[index % MAXSEMS].status = OCCUPIED;
	semaphores[index % MAXSEMS].mutex = MboxCreate(1, 0);
	semaphores[index % MAXSEMS].blockedHead = NULL;
	semaphores[index % MAXSEMS].blockedTail = NULL;
	*semaphore = index;
	currSema = index;
	numSema++;
	return 0;
}

/*
 * The SYS_SEMP handler
 * System Call Outputs:	
 * 		arg4: -1 if the semaphore ID is invalid; 0 otherwise
 */
void semP(systemArgs *args) {
	int re = semPHelper((long) args -> arg1);
	args -> arg4 = (void*)(long) re;
}

/*
 * SYS_SEMP handler or the actual SYS_SEMP handler
 * If semaphore > 0, decrement it by 1; otherwise block until s > 0
 * This is achieve through mailbox
 * @return:		   -1, if the semaphore ID is invalid 
 * 					0, otherwise
 */
int semPHelper(int semaphore) {
	if (semaphores[semaphore].status != OCCUPIED) return -1;
	// lock the value critical section
	MboxSend(semaphores[semaphore].mutex, NULL, 0);
	// decrement the value by 1
	semaphores[semaphore].value--;
	// block self on semaphore if value < 0
	if (semaphores[semaphore].value < 0) {
		// unlock the value critical section
		MboxReceive(semaphores[semaphore].mutex, NULL, 0);
		// block itself
		queue *curr = malloc(sizeof(queue));
		curr -> ID = getpid();
		curr -> next = NULL;
		if (semaphores[semaphore].blockedHead == NULL) {
			semaphores[semaphore].blockedHead = curr;
			semaphores[semaphore].blockedTail = curr;
		} else {
			semaphores[semaphore].blockedTail -> next = curr;
			semaphores[semaphore].blockedTail = curr;
		}
		MboxReceive(shadowProcTable[getpid() % MAXPROC].mailbox, NULL, 0);
	} 
	// unlock the value critical section
	MboxReceive(semaphores[semaphore].mutex, NULL, 0);
	return 0;
}

/*
 * The SYS_SEMV handler
 * System Call Outputs:	
 * 		arg4: -1 if the semaphore ID is invalid; 0 otherwise
 */
void semV(systemArgs *args) {
	int re = semVHelper((long) args -> arg1);
	args -> arg4 = (void*)(long) re;
}

/*
 * SYS_SEMV handler or the actual SYS_SEMV handler
 * Increment semaphore by 1
 * @return:		   -1, if the semaphore ID is invalid 
 * 					0, otherwise
 */
int semVHelper(int semaphore) {
	if (semaphores[semaphore].status != OCCUPIED) return -1;
	// lock the value critical section
	MboxSend(semaphores[semaphore].mutex, NULL, 0);
	// increment the value by 1
	semaphores[semaphore].value++;
	// reactivate a process if possible
	// if (semaphores[semaphore].value <= 0 && semaphores[semaphore].blockedHead != NULL) {
	if (semaphores[semaphore].blockedHead != NULL) {
		queue *curr = semaphores[semaphore].blockedHead;
		semaphores[semaphore].blockedHead = curr -> next;
		if (semaphores[semaphore].blockedHead == NULL)
			semaphores[semaphore].blockedTail = NULL;
		else if (semaphores[semaphore].blockedHead -> next != NULL)
			semaphores[semaphore].blockedTail = semaphores[semaphore].blockedTail -> next;
		MboxSend(shadowProcTable[curr -> ID % MAXPROC].mailbox, NULL, 0);
	// else unlock the value critical section
	} else MboxReceive(semaphores[semaphore].mutex, NULL, 0);
	return 0;
}
	
