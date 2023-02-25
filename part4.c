/* ***********************************************
 * FILE:       part4.c
 * AUTHOR:     SIWEN WANG, HUIQI HE
 * COURSE:     CSC4XX FALL 2022
 * ASSIGNMENT: OS PROJECT PART 4
 * PURPOSE:	   DEVICE DRIVER
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
#include "phase4.h"
#include "phase4_usermode.h"

/* -------------------------------------------------------- Global Variables */
#define EMPTY	 	0
#define OCCUPIED 	1
#define READ		0
#define WRITE		1
#define MAXDISK		32	// this is bad
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------- Helper Functions */
int clockDriver(char *arg);
void sleep(systemArgs *args);
int sleepHelper(int seconds);
int terminalDriver(char *arg);
void termRead(systemArgs *args);
int termReadHelper(char *buffer, int bufSize, int unit, int *lenOut);
void termWrite(systemArgs * args);
int termWriteHelper(char *buffer, int bufSize, int unit, int *lenOut);
int diskDriver(char *arg);
void diskSize(systemArgs * args);
int diskSizeHelper(int unit, int *sector, int *track, int *disk);
void diskRead(systemArgs * args);
void diskWrite(systemArgs * args);
int diskRequestHelper(void *buffer, int unit, int track, int firstBlock, 
						int blocks, int *statusOut, int type);
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------------- Structures */
typedef struct wakeUpQueue {
	int wakeUpTime;
	int PID;
	struct wakeUpQueue *next;
} wakeUpQueue;

typedef struct diskRequestQueue {
	int PID;
	int type; // READ or WRITE
	int firstBlock;
	int numBlocks;
	void *buffer;
	int status;
	struct diskRequestQueue *last;
	struct diskRequestQueue *next;
} diskRequestQueue;
/* ------------------------------------------------------------------------- */

/* --------------------------------------------------------------- Variables */
// terminal related, initialized in init
int termWriteLock[USLOSS_TERM_UNITS];
int termWriteFinish[USLOSS_TERM_UNITS];
int termReadLock[USLOSS_TERM_UNITS];
int termReadFinish[USLOSS_TERM_UNITS];
char toWrite[MAXLINE];
char toRead[MAXLINE];
int readIndex;

// int canWrite;
// wakeUpQueue *writeHead[USLOSS_TERM_UNITS];
// wakeUpQueue *writeTail[USLOSS_TERM_UNITS];

// used in start processes
int clockDriverPID;
int diskDriverPID[USLOSS_DISK_UNITS];
int terminalDriverPID[USLOSS_TERM_UNITS];
// initialized in init, disk and sleep related
int firstTrack[USLOSS_DISK_UNITS][2];
wakeUpQueue *wakeUpHead;
int diskRequestLock;
int numDiskTracks[USLOSS_DISK_UNITS];
int diskSizeMailbox[USLOSS_DISK_UNITS][2];
diskRequestQueue *diskRequests[USLOSS_DISK_UNITS][MAXDISK][2];
int diskMailbox[USLOSS_DISK_UNITS]; 
// global disk request
USLOSS_DeviceRequest globalDiskRequest;
/* ------------------------------------------------------ Required Functions */
/*
 * Fork all the required device drivers
 */
void phase4_start_service_processes() {
	// fork all device driver processes
	clockDriverPID = fork1("clockDriver", clockDriver, "", USLOSS_MIN_STACK, 2);

	// sprintf(unit, "%d", 2);

	diskDriverPID[0] = fork1("diskDriver1", diskDriver, "0", 
									USLOSS_MIN_STACK, 2);
	diskDriverPID[1] = fork1("diskDriver2", diskDriver, "1", 
									USLOSS_MIN_STACK, 2);
	terminalDriverPID[0] = fork1("terminalDriver1", terminalDriver, "0", 
									USLOSS_MIN_STACK, 2);
	terminalDriverPID[1] = fork1("terminalDriver2", terminalDriver, "1", 
									USLOSS_MIN_STACK, 2);
	terminalDriverPID[2] = fork1("terminalDriver3", terminalDriver, "2", 
									USLOSS_MIN_STACK, 2);
	terminalDriverPID[3] = fork1("terminalDriver4", terminalDriver, "3", 
									USLOSS_MIN_STACK, 2);
}

/*
 * Initialize all variables, except the drivers' PID
 */
void phase4_init(void) {
	// register all the syscall handler
	systemCallVec[SYS_SLEEP] = sleep;
	systemCallVec[SYS_TERMREAD] = termRead;
	systemCallVec[SYS_TERMWRITE] = termWrite;
	systemCallVec[SYS_DISKSIZE] = diskSize;
	systemCallVec[SYS_DISKREAD] = diskRead;
	systemCallVec[SYS_DISKWRITE] = diskWrite;
	// terminal related
	for (int i = 0; i < USLOSS_TERM_UNITS; i++) {
		termWriteLock[i] = MboxCreate(1, 0);
		termWriteFinish[i] = MboxCreate(0, 0);
		termReadLock[i] = MboxCreate(1, 0);
		termReadFinish[i] = MboxCreate(0, 0);
	}
	readIndex = 0;

	// canWrite = 1;

	// clock and disk related
	wakeUpHead = NULL;
	diskRequestLock = MboxCreate(1, 0);
	for(int i = 0; i < USLOSS_DISK_UNITS; i++) {
		numDiskTracks[i] = -1;
		diskSizeMailbox[i][0] = MboxCreate(0, 0);
		diskSizeMailbox[i][1] = 0;
		diskMailbox[i] = MboxCreate(1, 0);
		firstTrack[i][0] = -1;
		firstTrack[i][1] = 0;
	}
}
/* ------------------------------------------------------------------------- */

/*
 * for debug
 */
void test() {
	int currPSR = USLOSS_PsrGet();
	int reval = USLOSS_PsrSet(currPSR | 1); // 0000 0001
	dumpProcesses();
	if (reval) ;
	reval = USLOSS_PsrSet(currPSR);
}


/* -------------------------------------------------------- Helper Functions */
/* 				  						     Some of these are also required */
/* 	     but not specified in phase3.h and won't be called by any test cases */

/*
 * Clock driver, wake up processes
 * Infinitely loop here
 */
int clockDriver(char *arg) {
	int status;
	while (1) {
		// waiting on a clock interrupt to happen
		waitDevice(USLOSS_CLOCK_DEV, 0, &status); 
		// wake up all sleeping process
		for (; wakeUpHead != NULL; ) {
			if (wakeUpHead -> wakeUpTime <= currentTime()) {
				wakeUpQueue *curr = wakeUpHead;
				if (wakeUpHead -> next == NULL) wakeUpHead = NULL;
				else wakeUpHead = wakeUpHead -> next;
				unblockProc(curr -> PID);
				free(curr);
			} else break;
		}
	}
	return status;
}

/*
 * The SYS_SLEEP handler
 * Pauses the current process for the specified amount of time
 * System Call Outputs: 
 * 			arg4:	   -1, if illegal values were given as input
 * 						0, otherwise
 */
void sleep(systemArgs *args) {
	int re = sleepHelper((long) args -> arg1);
	args -> arg4 = (void*)(long) re;
}

/*
 * The actual SYS_SLEEP handler
 * @return: 	   -1, if illegal values were given as input
 * 					0, otherwise
 */
int sleepHelper(int seconds) {
	if (seconds < 0) return -1;
	// add this process to wake up queue
	int wakeUpTime = currentTime() + (seconds * 1000000);
	wakeUpQueue *temp = malloc(sizeof(wakeUpQueue));
	temp -> wakeUpTime = wakeUpTime;
	temp -> PID = getpid();
	temp -> next = NULL;
	// keep the wake up queue sorted
	if (wakeUpHead == NULL) wakeUpHead = temp;
	else {
		wakeUpQueue *curr = wakeUpHead;
		for (; curr != NULL; curr = curr -> next) {

			if (curr -> next == NULL) {
				curr -> next = temp;
				break;
			} else if (curr -> wakeUpTime <= wakeUpTime
						&& curr -> next -> wakeUpTime >= wakeUpTime) {
				temp -> next = curr -> next;
				curr -> next = temp;
				break;
			}
		}
	}
	blockMe(30); // arbitrary number 30	
	return 0;
}

/*
 * Terminal driver
 */
int terminalDriver(char *arg) {
	int unit = atoi(arg);
	int status, re, control;
	// enable read and write interrupt
	control = 6;
	re = USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, &control);
	if (re == USLOSS_DEV_INVALID) 
		USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter in terminal driver\n");
	while (1) {
		re = waitDevice(USLOSS_TERM_INT, unit, &status);
		if (re == USLOSS_DEV_INVALID) 
			USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter in terminal driver\n");
		// if write available
		if (USLOSS_TERM_STAT_XMIT(status) == USLOSS_DEV_READY) {
			for (int i = 0; i < strlen(toWrite); i++) {
				control = 0;
				control = USLOSS_TERM_CTRL_CHAR(control, toWrite[i]);
				control = USLOSS_TERM_CTRL_XMIT_INT(control);
				control = USLOSS_TERM_CTRL_RECV_INT(control);
				control = USLOSS_TERM_CTRL_XMIT_CHAR(control);
				re = USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit,((void *)(long) control));
				if (re == USLOSS_DEV_INVALID) 
					USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter in terminal driver\n");
				// wait till the current write finished
				while (1) {
					re = waitDevice(USLOSS_TERM_INT, unit, &status);
					if (re == USLOSS_DEV_INVALID) 
						USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter in terminal driver\n");
					if (USLOSS_TERM_STAT_XMIT(status) == USLOSS_DEV_READY) break;
					else if (USLOSS_TERM_STAT_XMIT(status == USLOSS_DEV_ERROR))
						USLOSS_Trace("Error in terminal write\n");
				}
			}
			MboxSend(termWriteFinish[unit], NULL, 0);
		}
		// if read available
		if (USLOSS_TERM_STAT_RECV(status) == USLOSS_DEV_READY) {
			char c = USLOSS_TERM_STAT_CHAR(status);
			toRead[readIndex] = c;
			readIndex++;
			if (readIndex == MAXLINE || c == '\n') {
				readIndex = 0;
				MboxSend(termReadFinish[unit], NULL, 0);
			}
		}
	}
	return 0;
}

/*
 * The SYS_TERMREAD handler
 * System Call Outputs: 
 * 			arg2: 		number of characters read
 * 			arg4:	   -2, if no children
 * 						0, otherwise
 */
void termRead(systemArgs *args) {
	int lenOut;
	int re = termReadHelper((char*)args -> arg1, (long) args -> arg2, 
									(long) args -> arg3, &lenOut);
	args->arg2 = (void*)((long)lenOut);
	args->arg4 = (void*)(long) re;
}

/*
 * The actual SYS_TERMREAD handler   
 */
int termReadHelper(char *buffer, int bufSize, int unit, int *lenOut) {
	if (unit < 0 || unit >= USLOSS_TERM_UNITS) return -1;
	if (bufSize < 0 || bufSize > MAXLINE) return -1;
	// lock read
	MboxSend(termReadLock[unit], NULL, 0);
	// waiting for driver to finish
	MboxReceive(termReadFinish[unit], NULL, 0);
	*lenOut = strlen(toRead);
	memset(toRead, '\0', MAXLINE);
	// unlock read
	MboxReceive(termReadLock[unit], NULL, 0);
	return 0;
}

/*
 * The SYS_TERMWRITE handler
 * System Call Outputs: 
 * 			arg2: 		number of characters read ????
 * 			arg4:	   -1, if illegal values were given as input
 * 						0, otherwise
 */
void termWrite(systemArgs * args) {
	int lenOut;
	int re = termWriteHelper((char*)args -> arg1, (long) args -> arg2, 
									(long) args -> arg3, &lenOut);
	args->arg2 = (void*)(long) lenOut;
	args->arg4 = (void*)(long) re;
}

/*
 * The actual SYS_TERMWRITE handler	   
 */
int termWriteHelper(char *buffer, int bufSize, int unit, int *lenOut) {
	if (unit < 0 || unit >= USLOSS_TERM_UNITS) return -1;
	if (bufSize < 0 || bufSize > MAXLINE) return -1;
	int re, status;
	// grab the write lock
	MboxSend(termWriteLock[unit], NULL, 0);

	// somehow the mutex is not working so doing it manually 
	// if (canWrite) 
	// 	canWrite = 0;
	// else {
	// 	wakeUpQueue *curr = malloc(sizeof(wakeUpQueue));
	// 	curr -> PID = getpid();
	// 	curr -> next = NULL;
	// 	if (writeHead[unit] == NULL) {
	// 		writeHead[unit] = curr;
	// 		writeTail[unit] = curr;
	// 	} else {
	// 		writeTail[unit] -> next = curr;
	// 		writeTail[unit] = curr;
	// 	}
	// 	blockMe(40);
	// }

	// send the message
	int control = 0;
	control = USLOSS_TERM_CTRL_CHAR(control, buffer[0]);
	control = USLOSS_TERM_CTRL_XMIT_INT(control);
	control = USLOSS_TERM_CTRL_RECV_INT(control);
	control = USLOSS_TERM_CTRL_XMIT_CHAR(control);
	while (1) {
		re = USLOSS_DeviceInput(USLOSS_DISK_DEV, unit, &status);
		if (re == USLOSS_DEV_INVALID) printf("invalid\n");
		if (USLOSS_TERM_STAT_XMIT(status) == USLOSS_DEV_READY) break;
	}
	re = USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit,((void *)(long) control));
	if (re == USLOSS_DEV_INVALID) 
		USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter in terminal driver\n");
	strcpy(toWrite, buffer + 1);
	// after finish
	//MboxReceive(termWriteNum[unit], lenOut, sizeof(int));
	*lenOut = bufSize;
	// waiting for driver to finish
	MboxReceive(termWriteFinish[unit], NULL, 0);
	// clear out buffer
	memset(toWrite, '\0', MAXLINE);

	// release write lock
	MboxReceive(termWriteLock[unit], NULL, 0);
	// wakeUpQueue *temp = writeHead[unit];
	// for (; temp != NULL; ) {
	// 	int pid = temp -> PID;
	// 	if (temp -> next == NULL) {
	// 		writeHead[unit] = NULL;
	// 		writeTail[unit] = NULL;
	// 	} else {
	// 		writeHead[unit] = temp -> next;
	// 		writeTail[unit] = writeTail[unit] -> next;
	// 	}
	// 	temp = writeHead[unit];
	// 	unblockProc(pid);
	// }
	return 0;
}

/*
 * The Disk Driver
 * Perform all read and write disk request using C-SCAN algorithm
 * Infinite loop here
 */
int diskDriver(char *arg) {
	int status, re;
	int unit = atoi(arg);
	// get the disk sizes
	// lock the global disk request
	MboxSend(diskRequestLock, NULL, 0);
	while (1) {
		re = USLOSS_DeviceInput(USLOSS_DISK_DEV, unit, &status);
		if (status == USLOSS_DEV_READY) break;
		else if (status == USLOSS_DEV_ERROR) printf("error\n");
	}
	// call USLOSS_DeviceOutput
	globalDiskRequest.opr = USLOSS_DISK_TRACKS;
	globalDiskRequest.reg1 = (void*)(long) &numDiskTracks[unit];
	globalDiskRequest.reg2 = NULL;
	re = USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &globalDiskRequest);
	if (re == USLOSS_DEV_INVALID) 
		USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter in disk driver\n");
	// waiting for it to finish
	re = waitDevice(USLOSS_DISK_DEV, unit, &status);
	// wake up anyone that are waiting on the this
	for (; diskSizeMailbox[unit][1] != 0; diskSizeMailbox[unit][1]--) 
		MboxSend(diskSizeMailbox[unit][0], NULL, 0);
	//	MboxCondSend(diskSizeMailbox[unit][0], NULL, 0);
	// unlock the global disk request
	MboxReceive(diskRequestLock, NULL, 0);
	// initialize the diskRequests table
	for (int i = 0; i < USLOSS_DISK_UNITS; i++) {
		for(int j = 0; j < numDiskTracks[i]; j++) {
			diskRequests[i][numDiskTracks[i]][0] = NULL;
			diskRequests[i][numDiskTracks[i]][1] = NULL;
		}
	}

	// start handling request
	int currTrack = 0;
	int newTrack = currTrack;
	while (1) {
		// waiting for a disk request
		MboxReceive(diskMailbox[unit], NULL, 0);
		// process all queued disk requests on this disk
		// first iterate through all tracks
		if (firstTrack[unit][1] && firstTrack[unit][0] != -1) {
			currTrack = firstTrack[unit][0];
			firstTrack[unit][1] = 0;
		}
		for (int i = 0; i < numDiskTracks[unit]; i++) {
			if (diskRequests[unit][(currTrack + i) % 15][0] == NULL) continue;
			else {
				newTrack = (currTrack + i) % 15;
				// seek if necessary
				if (newTrack != currTrack) {
					MboxSend(diskRequestLock, NULL, 0);
					globalDiskRequest.opr = USLOSS_DISK_SEEK;
					globalDiskRequest.reg1 = (void*)(long)newTrack;
					globalDiskRequest.reg2 = NULL;
					int re = USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &globalDiskRequest);
					if (re == USLOSS_DEV_INVALID) 
						USLOSS_Trace("Invalid USLOSS_DeviceOutput parameter\n");
					re = waitDevice(USLOSS_DISK_DEV, unit, &status);
					if (re == USLOSS_DEV_INVALID) 
						USLOSS_Trace("Invalid USLOSS_DeviceInput parameter\n");
					if (status == USLOSS_DEV_BUSY) 
						USLOSS_Trace("USLOSS_DEV_BUSY showed, serious error in code\n");
					else if (status == USLOSS_DEV_ERROR) 
						USLOSS_Trace("Fail to Seek\n");
					MboxReceive(diskRequestLock, NULL, 0);
				}
				// process all requests within the current track
				for (; diskRequests[unit][newTrack][0] != NULL; ) {
					// deque the current request
					diskRequestQueue *currReq = diskRequests[unit][newTrack][0];
					if (currReq -> next == NULL) {
						diskRequests[unit][newTrack][0] = NULL;
						diskRequests[unit][newTrack][1] = NULL;
					} else diskRequests[unit][newTrack][0] = currReq -> next;
					// perform read/write on all required blocks
					for (int i = 0; i < currReq -> numBlocks; i++) {
						MboxSend(diskRequestLock, NULL, 0);
						if (currReq -> type == READ) globalDiskRequest.opr = USLOSS_DISK_READ;
						else if (currReq -> type == WRITE) globalDiskRequest.opr = USLOSS_DISK_WRITE;
						// NOTE!! I'm wrapping around blocks here and this might not be desired
						globalDiskRequest.reg1 = (void*)(long) ((currReq -> firstBlock + i) % USLOSS_DISK_TRACK_SIZE);
						globalDiskRequest.reg2 = (currReq -> buffer) + (i * USLOSS_DISK_SECTOR_SIZE);
						int re = USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &globalDiskRequest);
						if (re == USLOSS_DEV_INVALID) 
							USLOSS_Trace("Invalid USLOSS_DeviceOutput parametern");
						// check status after disk request
						re = waitDevice(USLOSS_DISK_DEV, unit, &status);
						if (re == USLOSS_DEV_INVALID) 
							USLOSS_Trace("Invalid USLOSS_DeviceInput parameter\n");
						if (status == USLOSS_DEV_BUSY) 
							USLOSS_Trace("USLOSS_DEV_BUSY showed, serious error in code\n");
						currReq -> status = status;
						if (status == USLOSS_DEV_ERROR) {
							USLOSS_Trace("failed disk request\n");
							break;
						}
						MboxReceive(diskRequestLock, NULL, 0);
					}
					unblockProc(currReq -> PID);
				}
			}
		}
		currTrack = newTrack;
	}
	return status;

}

/*
 * The SYS_DISKSIZE handler
 * Queries the size of a given disk
 * System Call Outputs: 
 * 			arg1: 		size of a block, in bytes
 * 			arg2: 		number of blocks in track
 * 			arg3:		number of tracks in the disk
 * 			arg4:	   -1, if illegal values were given as input
 * 						0, otherwise
 */
void diskSize(systemArgs * args) {
	int sector, track, disk;
	int re = diskSizeHelper((long)args -> arg1, &sector, &track, &disk);
	args -> arg1 = (void*)(long) sector;
	args -> arg2 = (void*)(long) track;
	args -> arg3 = (void*)(long) disk;
	args -> arg4 = (void*)(long) re;
}

/*
 * The actual SYS_DISKSIZE handler
 * @return: 	   -1, if illegal values were given as input
 * 					0, otherwise
 */
int diskSizeHelper(int unit, int *sector, int *track, int *disk) {
	if (unit < 0 || unit > USLOSS_DISK_UNITS) return -1;

	*sector = USLOSS_DISK_SECTOR_SIZE; // size of disk sector in bytes
	*track = USLOSS_DISK_TRACK_SIZE; // number of sectors in a track
	// wait for init to finish if haven't already
	if (numDiskTracks[unit] == -1) {
		diskSizeMailbox[unit][1]++;
		MboxReceive(diskSizeMailbox[unit][0], NULL, 0);
	} 
	*disk = numDiskTracks[unit]; // number of disk tracks
	return 0;
}

/*
 * The SYS_DISKREAD handler
 * System Call Outputs: 
 * 			arg1: 		0, if transfer was successful
 * 						the disk status register otherwise
 * 			arg4:	   -1, if illegal values were given as input
 * 						0, otherwise
 */
void diskRead(systemArgs * args) {
	int statusOut;
	int re = diskRequestHelper(args -> arg1, (long) args -> arg5, (long) args -> arg3, 
								(long) args -> arg4, (long) args -> arg2, &statusOut, READ);
	args -> arg1 = (void*)(long)statusOut;
	args -> arg4 = (void*)(long)re;
}

/*
 * The SYS_DISKWRITE handler
 * System Call Outputs: 
 * 			arg1: 		0, if transfer was successful
 * 						the disk status register otherwise
 * 			arg4:	   -1, if illegal values were given as input
 * 						0, otherwise
 */
void diskWrite(systemArgs * args) {
	int statusOut;
	int re = diskRequestHelper(args -> arg1, (long) args -> arg5, (long) args -> arg3, 
								(long) args -> arg4, (long) args -> arg2, &statusOut, WRITE);
	args -> arg1 = (void*)(long)statusOut;
	args -> arg4 = (void*)(long)re;
}

/*
 * The actual SYS_DISKREAD and SYS_DISKWRITE handler
 * @return: 	   -1, if illegal values were given as input
 * 					0, otherwise
 */
int diskRequestHelper(void *buffer, int unit, int track, int firstBlock, 
						int blocks, int *statusOut, int type) {
	if (numDiskTracks[unit] == -1) {
		diskSizeMailbox[unit][1]++;
		MboxReceive(diskSizeMailbox[unit][0], NULL, 0);
		// NOTE I'm changing this because test 14 timeout in gradescope
		// but it runs fine in my terminal
		//numDiskTracks[unit] = 16 * (1 + unit);
	} 
	if (unit < 0 || unit >= USLOSS_DISK_UNITS) return -1;
	if (track < 0 || track > numDiskTracks[unit]) return -1;
	if (firstBlock < 0 || firstBlock > USLOSS_DISK_TRACK_SIZE) return -1;
	if (blocks < 0 || blocks > USLOSS_DISK_TRACK_SIZE) return -1;
	// add this request to the queue
	diskRequestQueue *currReq = malloc(sizeof(diskRequestQueue));
	currReq -> PID = getpid();
	currReq -> type = type;
	currReq -> firstBlock = firstBlock;
	currReq -> numBlocks = blocks;
	currReq -> buffer = buffer;
	currReq -> status = -1;
	currReq -> next = NULL;
	if (diskRequests[unit][track][0] == NULL) {
		diskRequests[unit][track][0] = currReq;
		diskRequests[unit][track][1] = currReq;
		if (firstTrack[unit][0] == -1) {
			firstTrack[unit][0] = track;
			firstTrack[unit][1] = 1;
		}
	} else {
		diskRequests[unit][track][1] -> next = currReq;
		diskRequests[unit][track][1] = currReq;
	}
	//MboxCondSend(diskMailbox[unit], NULL, 0);
	MboxSend(diskMailbox[unit], NULL, 0);
	blockMe(33); // arbitrary number 33
	if (currReq -> status == USLOSS_DEV_ERROR) 
		*statusOut = currReq -> status;
	else *statusOut = 0;
	return 0;
}

/* ------------------------------------------------------------------------- */





