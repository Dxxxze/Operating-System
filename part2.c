/* ***********************************************
 * FILE:       part2.c
 * AUTHOR:     SIWEN WANG, HUIQI HE
 * COURSE:     CSC4XX FALL 2022
 * ASSIGNMENT: OS PROJECT PART 2
 * PURPOSE:    MESSAGE AND INTERRUPT HANDLER
 * ***********************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <usloss.h>
#include "part1.h"
#include "part2.h"

/* -------------------------------------------------------- Global Variables */
#define EMPTY		0
#define OCCUPIED	1
#define DESTROYED	2
#define INTSIZE		4	// in bytes
#define NUMDEVICE	2
#define NUMTERMINAL	4
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------- Helper Functions */
int checkRelease(int MID);
void diskInterruptHandler(int type, void *payload);
void terminalInterruptHandler(int type, void *payload);
void syscallInterruptHandler(int type, void *payload);
static void nullsys(systemArgs *args);
void checkKernelMode();
void restoreInterrupt(int PSR);
void disableInterrupt();
void enableInterrupt();
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------------- Structures */
typedef struct queue {
	int ID;
	struct queue *next;
} queue;

typedef struct mailSlot {
	int SID;
	int status;
	int slotSize;
	void *message;
} mailSlot;

typedef struct mailbox {
	int MID;
	int status;
	int numSlots;
	int slotSize;
	int numMsgQueued;
	struct queue *slotsHead;
	struct queue *slotsTail;
	struct queue *consumersHead; 
	struct queue *consumersTail; 
	struct queue *producersHead;
	struct queue *producersTail;
} mailbox;

typedef struct shadowPTE{
	int PID;
	int status;
	int isBlocked;
	void *msg;
	int msgSize;
} shadowPTE;
/* ------------------------------------------------------------------------- */

/* --------------------------------------------------------------- Variables */
void (*systemCallVec[MAXSYSCALLS])(systemArgs *args);
// array of mailboxes
mailbox mailboxes[MAXMBOX]; 
// array of mail slots
mailSlot mailSlots[MAXSLOTS];
// shadow process table
shadowPTE shadowProcTable[MAXPROC];
int numMailboxes, numSlotUsed;
int curMID, curSID;
// count blocked process
int blockingIOCount;
int clockInterruptCount;
// interrupt mailboxes below
int clockMB;
int diskMB[NUMDEVICE];
int terminalMB[NUMTERMINAL];
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------ Required Functions */
/*
 * Starting point, other code will call it but it does nothing
 */
void part2_start_service_processes(void) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// restore interrupt
	restoreInterrupt(currPSR);
}

/*
 * Initialize all variables
 */
void part2_init(void) {
	// check if in kernel mode
	checkKernelMode();
	// initiate variables
	numMailboxes = 0;
	numSlotUsed = 0;
	curMID = 0;
	curSID = 0;
	blockingIOCount = 0;
	clockInterruptCount = 0;
	// initialize the arrays with all 0
	memset(mailboxes, 0, MAXMBOX * sizeof(mailbox)); 
	memset(mailSlots, 0, MAXSLOTS * sizeof(mailSlot));
	memset(shadowProcTable, 0, MAXPROC * sizeof(shadowPTE));
	// initialize interrupt mailboxes
	clockMB = CreateMbox(1, INTSIZE);
	for (int i = 0; i < NUMDEVICE; i++) 
		diskMB[i] = CreateMbox(1, INTSIZE);
	for (int i = 0; i < NUMTERMINAL; i++)
		terminalMB[i] = CreateMbox(1, INTSIZE);
	// install interrupt handler onto USLOSS_IntVet[]
	USLOSS_IntVec[USLOSS_DISK_INT] = diskInterruptHandler;
	USLOSS_IntVec[USLOSS_TERM_INT] = terminalInterruptHandler;
	USLOSS_IntVec[USLOSS_SYSCALL_INT] = syscallInterruptHandler;
	// initialize systemCallVec[]
	for (int i = 0; i < MAXSYSCALLS; i++)
		systemCallVec[i] = nullsys;
}	

/*
 * Create a new mailbox in the mailboxes array
 * @parameters:	numSlots, 	the maximum number of slots that may be used to queue up
 * 							messages from this mailbox
 * 				slotSize, 	the largest allowable message that can be sent through this mailbox
 * @return:		-1, 		if invalid parameters or no mailbox avaliavle
 * 				>0, 		MID of the new mailbox created
 */
int CreateMbox(int numSlots, int slotSize) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for errors
	if (numSlots < 0 || slotSize < 0 || slotSize > MAX_MESSAGE) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (numMailboxes >= MAXMBOX) {
		restoreInterrupt(currPSR);
		return -1;
	}
	// find open spot in the mailboxes array
	int openMailbox = curMID;
	for (int i = curMID; ;i++){
		openMailbox = i % MAXMBOX;
		if (mailboxes[openMailbox].status == EMPTY) 
			break;
	}
	// initialize value
	// use index in the array as ID
	mailboxes[openMailbox].MID = openMailbox;
	mailboxes[openMailbox].numSlots = numSlots;
	mailboxes[openMailbox].slotSize = slotSize;
	mailboxes[openMailbox].status = OCCUPIED;
	mailboxes[openMailbox].numMsgQueued = 0;
	mailboxes[openMailbox].slotsHead = NULL;
	mailboxes[openMailbox].slotsTail = NULL;
	mailboxes[openMailbox].consumersHead = NULL;
	mailboxes[openMailbox].consumersTail = NULL;
	mailboxes[openMailbox].producersHead = NULL;
	mailboxes[openMailbox].producersTail = NULL;
	numMailboxes++;	
	curMID++;
	// restore interrupt
	restoreInterrupt(currPSR);
	return openMailbox;
}

/*
 * Release a mailbox from the mailboxes array
 * @return: 	-1, 	if the ID is not a mailbox currently in use
 * 				 0, 	success
 */
int ReleaseMbox(int mbox_id) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for error
	if (mailboxes[mbox_id].status == EMPTY) {
		restoreInterrupt(currPSR);
		return -1;
	}
	// start releasing the mailbox
	mailboxes[mbox_id].status = DESTROYED;
	// release producers and consumers aka wake them all up
	queue *curr = mailboxes[mbox_id].consumersHead;
	for (; curr != NULL; curr = curr -> next) {
		if (shadowProcTable[curr -> ID % MAXPROC].isBlocked) {
			shadowProcTable[curr -> ID % MAXPROC].isBlocked = 0;
			unblockProc(curr -> ID);
		}
	}
	curr = mailboxes[mbox_id].producersHead;
	for (; curr != NULL; curr = curr -> next) {
		if (shadowProcTable[curr -> ID % MAXPROC].isBlocked) {
			shadowProcTable[curr -> ID % MAXPROC].isBlocked = 0;
			unblockProc(curr -> ID);
		}
	}
	curr = mailboxes[mbox_id].slotsHead;
	for (; curr != NULL; curr = curr -> next) {
		memset(&mailSlots[curr -> ID], 0, 1 * sizeof(mailSlot));
		numSlotUsed--;
	}
	// free this entry on the mailboxes array
	memset(&mailboxes[mbox_id], 0, 1 * sizeof(mailbox));
	numMailboxes--;
	// restore interrupt
	restoreInterrupt(currPSR);
	return 0;
}

/*
 * Send message with the given mailbox
 * Enforce producer queue as well
 * @return:		-3, 	if the mailbox was released
 * 				-1, 	if invalid arguments
 * 				 0, 	success
 */
int SendMbox(int mbox_id, void *msg_ptr, int msg_size) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for errors
	if (mbox_id >= MAXMBOX || mbox_id < 0) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (msg_size > mailboxes[mbox_id].slotSize || msg_size < 0) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == EMPTY) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == DESTROYED) {
		if (checkRelease(mbox_id)) {
			restoreInterrupt(currPSR);
			return -1;
		} else {
			restoreInterrupt(currPSR);
			return -3;
		}
	} else if (msg_size != 0 && msg_ptr == NULL) {
		restoreInterrupt(currPSR);
		return -1;
	}
	// start to send message
	mailbox *MB = &mailboxes[mbox_id];
	// add this process to producer queue
	queue *newProd = malloc(sizeof(queue));
	newProd -> ID = getpid();
	newProd -> next = NULL;
	if (MB -> producersHead == NULL) {
		MB -> producersHead = newProd;
		MB -> producersTail = newProd;
	} else {
		MB -> producersTail -> next = newProd;
		MB -> producersTail = MB -> producersTail -> next;
	}
	// block
	while ((MB -> numSlots > 0 && MB -> numMsgQueued >= MB -> numSlots) 
			|| (MB -> numSlots == 0 && MB -> consumersHead == NULL)
			|| MB -> producersHead -> ID != getpid()) {
		shadowProcTable[getpid() % MAXPROC].isBlocked = 1;
		blockMe(15); // an arbitrary int 15
		shadowProcTable[getpid() % MAXPROC].isBlocked = 0;
		// if the mailbox was destroyed while blocked
		if (mailboxes[mbox_id].status != OCCUPIED) {
			restoreInterrupt(currPSR);
			return -3;
		}
	}
	// if has consumers and 0 message in slot, feed the message directly
	if (MB -> consumersHead != NULL && MB -> numMsgQueued == 0) {
		queue *curCons = MB -> consumersHead;
		// feed the message into shadowPTE
		if (msg_ptr == NULL) shadowProcTable[curCons -> ID % MAXPROC].msg = NULL;
		else strcpy(shadowProcTable[curCons -> ID % MAXPROC].msg, msg_ptr);
		shadowProcTable[curCons -> ID % MAXPROC].msgSize = msg_size;
		// remove itself from the producer queue
		MB -> producersHead = MB -> producersHead -> next;
		if (MB -> producersHead == NULL) MB -> producersTail = NULL;
		// remove the consumer queue
		MB -> consumersHead = MB -> consumersHead -> next;
		if (MB -> consumersHead == NULL) MB -> consumersTail = NULL;
		// wake up the consumer
		if (shadowProcTable[curCons -> ID].isBlocked) {
			shadowProcTable[curCons -> ID].isBlocked = 0;
			unblockProc(curCons -> ID);
		}
	// queue if has slots but not full
	// or no consumers with available slots
	} else if ((MB -> consumersHead == NULL && MB -> numMsgQueued == 0 
					&& MB -> numSlots != 0)
					|| MB -> numMsgQueued < MB -> numSlots) {
		// halt simulation if all system mail slots are in use
		if (numSlotUsed >= MAXSLOTS) {
			USLOSS_Console("Error: all available system mail slots ");
			USLOSS_Console("are in use, halt simulation\n");
			USLOSS_Halt(1);
		}
		// create new slot queue object
		queue *newSlot = malloc(sizeof(queue));
		int openSlot = 0;
		for (int i = curSID; ; i++) {
			openSlot = i % MAXSLOTS;
			if (mailSlots[openSlot].status == EMPTY) break;
		}
		newSlot -> ID = openSlot;
		newSlot -> next = NULL;
		mailSlots[openSlot].SID = openSlot;
		mailSlots[openSlot].status = OCCUPIED;
		mailSlots[openSlot].slotSize = msg_size;
		if (msg_ptr == NULL) mailSlots[openSlot].message = NULL;
		else {
			mailSlots[openSlot].message = malloc(msg_size);
			strcpy(mailSlots[openSlot].message, msg_ptr);
		}
		numSlotUsed++;
		if (MB -> slotsHead == NULL) {
			MB -> slotsHead = newSlot;
			MB -> slotsTail = newSlot;
		} else {
			MB -> slotsTail -> next = newSlot;
			MB -> slotsTail = MB -> slotsTail -> next;
		}
		MB -> numMsgQueued++;
		// remove itself from the producer queue
		MB -> producersHead = MB -> producersHead -> next;
		if (MB -> producersHead == NULL) MB -> producersTail = NULL;
	} else {
		USLOSS_Console("DEBUG ERROR IN SendMbox()\n");
		USLOSS_Halt(1);
	}
	// restore interrupt
	restoreInterrupt(currPSR);
	return 0;
}

/*
 * Receive Message
 * @return:		-3, 	if the mailbox was released
 * 				-1, 	if invalid arguments
 * 			   >=0, 	the size of the message received
 */
int ReceiveMbox(int mbox_id, void *msg_ptr, int msg_max_size) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for errors
	if (mbox_id >= MAXMBOX || mbox_id < 0 || msg_max_size < 0) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == EMPTY) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == DESTROYED) {
		if (checkRelease(mbox_id)) {
			restoreInterrupt(currPSR);
			return -1;
		} else {
			restoreInterrupt(currPSR);
			return -3;
		}
	} 

	// start receiving
	mailbox *MB = &mailboxes[mbox_id];
	// add itself to the consumer queue
	queue *newCons = malloc(sizeof(queue));
	newCons -> ID = getpid();
	newCons -> next = NULL;
	if (MB -> consumersHead == NULL) {
		MB -> consumersHead = newCons;
		MB -> consumersTail = newCons;
	} else {
		MB -> consumersTail -> next = newCons;
		MB -> consumersTail = MB -> consumersTail -> next;
	}
	int index = getpid() % MAXPROC;
	shadowProcTable[index].status = OCCUPIED;
	shadowProcTable[index].PID = getpid();
	shadowProcTable[index].msg = malloc(msg_max_size);
	shadowProcTable[index].msgSize = -1;
	// block if no queued message or this process is not the first in consumer 
	// queue, or no message has been feed yet
	while (MB -> slotsHead == NULL || MB -> consumersHead -> ID != getpid()) {
		// if zero slot mailbox and exist producers
		if (MB -> numSlots == 0 && MB -> producersHead != NULL) {
			shadowProcTable[MB -> producersHead -> ID % MAXPROC].isBlocked = 0;
			unblockProc(MB -> producersHead -> ID);
			break;
		}
		// otherwise block
		shadowProcTable[getpid() % MAXPROC].isBlocked = 1;
		blockMe(16); // an arbitrary int 16
		shadowProcTable[getpid() % MAXPROC].isBlocked = 0;
		// if message was feed directly
		if (shadowProcTable[index].msgSize != -1) break;
		// if the mailbox was destroyed while blocked
		if (mailboxes[mbox_id].status != OCCUPIED) {
			restoreInterrupt(currPSR);
			return -3;
		}
	}
	// If the message is not feed yet
	if (shadowProcTable[index].msgSize == -1) {
		int SID = MB -> slotsHead -> ID;
		// remove the message from the mailbox
		MB -> slotsHead = MB -> slotsHead -> next;
		if (MB -> slotsHead == NULL) MB -> slotsTail = NULL;
		MB -> numMsgQueued--;
		// put the message in the shadowPTE
		shadowProcTable[index].msg = mailSlots[SID].message;
		shadowProcTable[index].msgSize = mailSlots[SID].slotSize;
		// remove the slot from the mailSlots array
		memset(&mailSlots[SID], 0, 1 * sizeof(mailSlot));
		numSlotUsed--;
		curSID = SID;
		// remove itself from the consumer queue
		MB -> consumersHead = MB -> consumersHead -> next;
		if (MB -> consumersHead == NULL) MB -> consumersTail = NULL;
	} 
	// write the message to the out pointer
	if (shadowProcTable[index].msgSize > msg_max_size) {
		restoreInterrupt(currPSR);
		return -1;
	}
	int msgSize = shadowProcTable[index].msgSize;
	if (msgSize != 0 && msg_ptr == NULL) {
		USLOSS_Console("Error: receive with NULL out pointer but non-zero message\n");
		return -1;
	}
	if (shadowProcTable[index].msg == NULL || msg_ptr == NULL) 
		msg_ptr = NULL;
	else strcpy(msg_ptr, shadowProcTable[index].msg);
	// free the spot on the shadow process table
	memset(&shadowProcTable[index], 0, 1 * sizeof(shadowPTE));
	// wake up the next producer if available slots
	if (MB -> producersHead != NULL && MB -> numMsgQueued < MB -> numSlots) {
		if (shadowProcTable[MB -> producersHead -> ID % MAXPROC].isBlocked) {
			shadowProcTable[MB -> producersHead -> ID % MAXPROC].isBlocked = 0;
			unblockProc(MB -> producersHead -> ID);
		}
	}
	// wake up the next consumer if more messages in slots
	if (MB -> consumersHead != NULL && MB -> slotsHead != NULL) {
		if (shadowProcTable[MB -> consumersHead -> ID % MAXPROC].isBlocked) {
			shadowProcTable[MB -> consumersHead -> ID % MAXPROC].isBlocked = 0;
			unblockProc(MB -> consumersHead -> ID);
		}
	}
	// restore interrupt
	restoreInterrupt(currPSR);
	return msgSize;
}

/*
 * Send message but refuse to block
 * @return:		-3, 	if the mailbox was released
 * 				-1, 	if invalid arguments
 * 				-2, 	if normally it would attempt to block
 * 			   >=0, 	the size of the message received		 
 */
int CondSendMbox(int mbox_id, void *msg_ptr, int msg_size) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for errors
	if (mbox_id >= MAXMBOX || mbox_id < 0) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (msg_size > mailboxes[mbox_id].slotSize || msg_size < 0) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == EMPTY) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == DESTROYED) { 
		restoreInterrupt(currPSR);
		return -3;
	} else if (msg_size != 0 && msg_ptr == NULL) {
		restoreInterrupt(currPSR);
		return -1;
	}
	// start to send message
	mailbox *MB = &mailboxes[mbox_id];
	// return -2 if the producer queue is not empty
	if (MB -> producersHead != NULL) {
		restoreInterrupt(currPSR);
		return -2;
	}
	// if has consumers and 0 message in slot, feed the message directly
	if (MB -> consumersHead != NULL && MB -> numMsgQueued == 0) {
		queue *curCons = MB -> consumersHead;
		// feed the message into shadowPTE
		if (msg_ptr == NULL) shadowProcTable[curCons -> ID].msg = NULL;
		else {
			shadowProcTable[curCons -> ID].msg = malloc(msg_size);
			strcpy(shadowProcTable[curCons -> ID % MAXPROC].msg, msg_ptr);
		}
		shadowProcTable[curCons -> ID % MAXPROC].msgSize = msg_size;
		unblockProc(curCons -> ID);
	// queue if has slots but not full
	// or no consumers with available slots
	} else if ((MB -> consumersHead == NULL && MB -> numMsgQueued == 0 
					&& MB -> numSlots != 0)
					|| MB -> numMsgQueued < MB -> numSlots) {
		// halt simulation if all system mail slots are in use
		if (numSlotUsed >= MAXSLOTS) {
			// USLOSS_Console("Error: all available system mail slots ");
			// USLOSS_Console("are in use, halt simulation\n");
			// USLOSS_Halt(1);
			// change to match testcase 16 exactly
			restoreInterrupt(currPSR);
			return -2;
		}
		queue *newSlot = malloc(sizeof(queue));
		int openSlot = 0;
		for (int i = curSID; ; i++) {
			openSlot = i % MAXSLOTS;
			if (mailSlots[openSlot].status == EMPTY) break;
		}
		newSlot -> ID = openSlot;
		newSlot -> next = NULL;
		
		mailSlots[openSlot].SID = openSlot;
		mailSlots[openSlot].status = OCCUPIED;
		mailSlots[openSlot].slotSize = msg_size;
		if (msg_ptr == NULL) mailSlots[openSlot].message = NULL;
		else {
			mailSlots[openSlot].message = malloc(msg_size);
			strcpy(mailSlots[openSlot].message, msg_ptr);
		}
		numSlotUsed++;
		if (MB -> slotsHead == NULL) {
			MB -> slotsHead = newSlot;
			MB -> slotsTail = newSlot;
		} else {
			MB -> slotsTail -> next = newSlot;
			MB -> slotsTail = MB -> slotsTail -> next;
		}
		MB -> numMsgQueued++;
	// else it'll attempt to block so return -2
	} else {
		restoreInterrupt(currPSR);
		return -2;
	}
	// restore interrupt
	restoreInterrupt(currPSR);
	return 0;
}

/*
 * Receive message but refuse to block
 * @return:		-3, 	if the mailbox was released
 * 				-1, 	if invalid arguments
 * 				-2, 	if normally it would attempt to block
 * 			   >=0, 	the size of the message received		 
 */
int CondReceiveMbox(int mbox_id, void *msg_ptr, int msg_max_size) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check for errors
	if (mbox_id >= MAXMBOX || mbox_id < 0 || msg_max_size < 0) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == EMPTY) {
		restoreInterrupt(currPSR);
		return -1;
	} else if (mailboxes[mbox_id].status == DESTROYED) { 
		restoreInterrupt(currPSR);
		return -3;
	}
	// start receiving
	mailbox *MB = &mailboxes[mbox_id];
	int index = getpid() % MAXPROC;
	// read if no consumer but has slots
	if (MB -> consumersHead == NULL && MB -> slotsHead != NULL) {
		int SID = MB -> slotsHead -> ID;
		// remove the message from the mailbox
		MB -> slotsHead = MB -> slotsHead -> next;
		if (MB -> slotsHead == NULL) MB -> slotsTail = NULL;
		MB -> numMsgQueued--;
		// put the message in the shadowPTE
		shadowProcTable[index].PID = getpid();
		if (mailSlots[SID].message == NULL) shadowProcTable[index].msg = NULL;
		else {
			shadowProcTable[index].msg = malloc(mailSlots[SID].slotSize);
			strcpy(shadowProcTable[index].msg, mailSlots[SID].message);
		}
		shadowProcTable[index].msgSize = mailSlots[SID].slotSize;
		// remove the slot from the mailSlots array
		memset(&mailSlots[SID], 0, 1 * sizeof(mailSlot));
		numSlotUsed--;
		curSID = SID;
	} else {
		restoreInterrupt(currPSR);
		return -2;
	}
	// write the message to the out pointer
	if (shadowProcTable[index].msgSize > msg_max_size) {
		restoreInterrupt(currPSR);
		return -1;
	}
	int msgSize = shadowProcTable[index].msgSize;
	if (shadowProcTable[index].msg == NULL) msg_ptr = NULL;
	else strcpy(msg_ptr, shadowProcTable[index].msg);
	// free the spot on the shadow process table
	memset(&shadowProcTable[index], 0, 1 * sizeof(shadowPTE));
	// wake up the next producer if avaliable slots
	if (MB -> producersHead != NULL && MB -> numMsgQueued < MB -> numSlots)
		unblockProc(MB -> producersHead -> ID);
	// wake up the next consumer if more messages in slots
	if (MB -> consumersHead != NULL && MB -> slotsHead != NULL) 
		unblockProc(MB -> consumersHead -> ID);
	// restore interrupt
	restoreInterrupt(currPSR);
	return msgSize;
}

/*
 * Waits for an interrupt to fire on a given device
 * @return:		0, 		always
 */
int deviceWait(int type, int unit, int *status) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check whether unit is correct based on the type
	blockingIOCount++;
	if (type == USLOSS_CLOCK_INT) {
		if (unit != 0) {
			USLOSS_Console("Error: type is clock but unit is %d, halt simulation\n", unit);
			USLOSS_Halt(1);
		}
		ReceiveMbox(clockMB, status, INTSIZE);
	} else if (type == USLOSS_DISK_INT) {
		if (unit < 0 || unit > 1) {
			USLOSS_Console("Error: type is disk but unit is %d, halt simulation\n", unit);
			USLOSS_Halt(1);
		}
		ReceiveMbox(diskMB[unit], status, INTSIZE);
	} else if (type == USLOSS_TERM_INT) {
		if (unit < 0 || unit > 3) {
			USLOSS_Console("Error: type is terminal but unit is %d, halt simulation\n", unit);
			USLOSS_Halt(1);
		}
		ReceiveMbox(terminalMB[unit], status, INTSIZE);
	} else {
		USLOSS_Console("Error: invalid device type, halt simulation\n");
		USLOSS_Halt(1);
	}
	blockingIOCount--;
	// restore interrupt
	restoreInterrupt(currPSR);
	return 0;
}

/*
 * Check the number of blocking process in the deviceWait
 * Return 0 is has noting, otherwise return the number blocked inside deviceWait()
 */
int part2_check_io() {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// checking io
	int retVal;
	if (blockingIOCount == 0) 
		retVal = 0;
	else if (blockingIOCount < 0) {
		USLOSS_Console("Error: Invalid number of blocking IO \" %i \" \n ", blockingIOCount);
		USLOSS_Halt(1);
	} else
		retVal = blockingIOCount;
	// restore interrupt
	restoreInterrupt(currPSR);
	return retVal;
}

/*
 * Called by Phase1 clockHandler. Count how many interrupt happened until 5 then
 * use CondSendMbox to send message and clear counter to 0.
 */
void part2_clockHandler() {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// check 
	clockInterruptCount++;
	if (clockInterruptCount == 5){
		int status;
		int re = USLOSS_DeviceInput(USLOSS_CLOCK_DEV, 0, &status);
		if (re != USLOSS_DEV_OK) {
			USLOSS_Console("Error: fail USLOSS_DeviceInput, halt simulation\n");
			USLOSS_Halt(1);
		}
		CondSendMbox(clockMB, &status, INTSIZE);
		clockInterruptCount = 0;
	}
	// restore interrupt
	restoreInterrupt(currPSR);
}
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------- Helper Functions */
/*
 * Check whether the given mailbox has finished releasing
 * return 1 if still releasing, 0 if otherwise
 */
int checkRelease(int MID) {
	queue *curr = mailboxes[MID].producersHead; 
	for (; curr != NULL; curr = curr -> next) {
		if (shadowProcTable[curr -> ID % MAXPROC].isBlocked) 
			return 1;
	}
	curr = mailboxes[MID].consumersHead;
	for (; curr != NULL; curr = curr -> next) {
		if (shadowProcTable[curr -> ID % MAXPROC].isBlocked)
			return 1;
	}
	return 0;
}

/*
 * Disk Interrupt Handler, type can be ignored cause it must be disk
 */
void diskInterruptHandler(int type, void *payload) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// start handling
	int unitNo = (int)(long)payload;
	int status;
	int re = USLOSS_DeviceInput(type, unitNo, &status);
	if (re != USLOSS_DEV_OK) {
		USLOSS_Console("Error: fail USLOSS_DeviceInput, halt simulation\n");
		USLOSS_Halt(1);
	}
	CondSendMbox(diskMB[unitNo], &status, INTSIZE);
	// restore interrupt
	restoreInterrupt(currPSR);
}

/*
 * Terminal Interrupt Handler, type can be ignored cause it must be terminal
 */
void terminalInterruptHandler(int type, void *payload) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// start handling
	int unitNo = (int)(long)payload;
	int status;
	int re = USLOSS_DeviceInput(type, unitNo, &status);
	if (re != USLOSS_DEV_OK) {
		USLOSS_Console("Error: fail USLOSS_DeviceInput, halt simulation\n");
		USLOSS_Halt(1);
	}
	CondSendMbox(terminalMB[unitNo], &status, INTSIZE);
	// restore interrupt
	restoreInterrupt(currPSR);
}

/*
 * Syscall Interrupt Handler, type can be ignored cause it must be syscall
 */
void syscallInterruptHandler(int type, void *payload) {
	// check kernel mode and disable interrupt
	checkKernelMode();
	int currPSR = USLOSS_PsrGet();
	disableInterrupt();
	// start handling
	systemArgs *args = payload;
	if (args -> number >= MAXSYSCALLS) {
		USLOSS_Console("syscallHandler(): Invalid syscall number ");
		USLOSS_Console("%d\n", args -> number);
		USLOSS_Halt(1);
	}
	nullsys(args);
	// restore interrupt
	restoreInterrupt(currPSR);
}


/*
 * Syscall handler, will be called by syscallInterruptHandler
 */
static void nullsys(systemArgs *args) {
	USLOSS_Console("nullsys(): Program called an unimplemented syscall.  ");
	USLOSS_Console("syscall no: %d   ", args -> number);
	USLOSS_Console("PSR: %#04x\n", USLOSS_PsrGet());
	USLOSS_Halt(1);
}

/*
 * Check if the Current mode bit on the PSR is 1
 */
void checkKernelMode() {
	if (! (USLOSS_PsrGet() & 1)) {
		USLOSS_Trace( "ERROR: Attempted to kernel func but not on kernel mode\n");
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
/* ------------------------------------------------------------------------- */
		


