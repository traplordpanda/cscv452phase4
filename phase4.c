#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <usloss.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <usyscall.h>
#include <provided_prototypes.h>
#include "driver.h"

#include <proc4structs.h>
#include <libuser.h>


int debug4 = 0;
static int running; 

static int ClockDriver(char *);
static int DiskDriver(char *);
static int TermDriver(char *);
static int TermReader(char *);
static int TermWriter(char *);
extern int start4();


void sleep(sysargs *);
void diskRead(sysargs *);
void diskWrite(sysargs *);
void diskSize(sysargs *);
void termRead(sysargs *);
void termWrite(sysargs *);

int sleepReal(int);
int diskSizeReal(int, int*, int*, int*);
int diskWriteReal(int, int, int, int, void *);
int diskReadReal(int, int, int, int, void *);
int diskReadOrWriteReal(int, int, int, int, void *, int);
int termReadReal(int, int, char *);
int termWriteReal(int, int, char *);
int getTime();

void checkForKernelMode(char *);
void emptyProc(int);
void initProc(int);
void setUserMode();
void initDiskQueue(diskQueue*);
void addDiskQ(diskQueue*, procPtr);
procPtr peekDiskQ(diskQueue*);
procPtr removeDiskQ(diskQueue*);
void initHeap(heap *);
void heapAdd(heap *, procPtr);
procPtr heapPeek(heap *);
procPtr heapRemove(heap *);

/* Globals */
procStruct ProcTable[MAXPROC];
heap sleepHeap;
int diskIsZapped; // indicates if the disk drivers are 'zapped' or not
diskQueue diskQs[DISK_UNITS]; // queues for disk drivers
int diskPids[DISK_UNITS]; // pids of the disk drivers

// mailboxes for terminal device
int charRecvMbox[TERM_UNITS]; // receive char
int charSendMbox[TERM_UNITS]; // send char
int lineReadMbox[TERM_UNITS]; // read line
int lineWriteMbox[TERM_UNITS]; // write line
int pidMbox[TERM_UNITS]; // pid to block
int termInt[TERM_UNITS]; // interrupt for term (control writing)

int termProcTable[TERM_UNITS][3]; // keep track of term procs


void
start3(void)
{
    char	name[128];
    char        termbuf[10];
    char        diskbuf[10];
    int		i;
    int		clockPID;
    int		pid;
    int		status;

    /*
     * Check kernel mode here.
     */
    checkForKernelMode("start3()");

    // initialize proc table
    for (i = 0; i < MAXPROC; i++) {
        initProc(i);
    }

    // sleep queue
    initHeap(&sleepHeap);

    // initialize sys_vec
    sys_vec[SYS_SLEEP] = sleep;
    sys_vec[SYS_DISKREAD] = diskRead;
    sys_vec[SYS_DISKWRITE] = diskWrite;
    sys_vec[SYS_DISKSIZE] = diskSize;
    sys_vec[SYS_TERMREAD] = termRead;
    sys_vec[SYS_TERMWRITE] = termWrite;

    // mboxes for terminal
    for (i = 0; i < TERM_UNITS; i++) {
        charRecvMbox[i] = MboxCreate(1, MAXLINE);
        charSendMbox[i] = MboxCreate(1, MAXLINE);
        lineReadMbox[i] = MboxCreate(10, MAXLINE);
        lineWriteMbox[i] = MboxCreate(10, MAXLINE); 
        pidMbox[i] = MboxCreate(1, sizeof(int));
    }

    /*
     * Create clock device driver 
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    running = semcreate_real(0);
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    if (clockPID < 0) {
    	console("start3(): Can't create clock driver\n");
    	halt(1);
    }
    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "running" once it is running.
     */

    semp_real(running);

    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */
    int temp;
    for (i = 0; i < DISK_UNITS; i++) {
        sprintf(diskbuf, "%d", i);
        pid = fork1("Disk driver", DiskDriver, diskbuf, USLOSS_MIN_STACK, 2);
        if (pid < 0) {
            console("start3(): Can't create disk driver %d\n", i);
            halt(1);
        }

        diskPids[i] = pid;
        semp_real(running); // wait for driver to start running

        // get number of tracks
        diskSizeReal(i, &temp, &temp, &ProcTable[pid % MAXPROC].diskTrack);
    }


    /*
     * Create terminal device drivers.
     */

     for (i = 0; i < TERM_UNITS; i++) {
        sprintf(termbuf, "%d", i); 
        termProcTable[i][0] = fork1(name, TermDriver, termbuf, USLOSS_MIN_STACK, 2);
        termProcTable[i][1] = fork1(name, TermReader, termbuf, USLOSS_MIN_STACK, 2);
        termProcTable[i][2] = fork1(name, TermWriter, termbuf, USLOSS_MIN_STACK, 2);
        semp_real(running);
        semp_real(running);
        semp_real(running);
     }


    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * I'm assuming kernel-mode versions of the system calls
     * with lower-case first letters, as shown in provided_prototypes.h
     */
    pid = spawn_real("start4", start4, NULL, 4 * USLOSS_MIN_STACK, 3);
    pid = wait_real(&status);

    /*
     * Zap the device drivers
     */

    status = 0;

     // zap clock driver
    zap(clockPID); 
    join(&status);

    // zap disk drivers
    for (i = 0; i < DISK_UNITS; i++) {
        semv_real(ProcTable[diskPids[i]].blockSem); 
        zap(diskPids[i]);
        join(&status);
    }

    // zap termreader
    for (i = 0; i < TERM_UNITS; i++) {
        MboxSend(charRecvMbox[i], NULL, 0);
        zap(termProcTable[i][1]);
        join(&status);
    }

    // zap termwriter
    for (i = 0; i < TERM_UNITS; i++) {
        MboxSend(lineWriteMbox[i], NULL, 0);
        zap(termProcTable[i][2]);
        join(&status);
    }

    // zap termdriver, etc
    char filename[50];
    for(i = 0; i < TERM_UNITS; i++)
    {
        int ctrl = 0;
        ctrl = TERM_CTRL_RECV_INT(ctrl);
        int result = device_output(TERM_DEV, i, (void *)((long) ctrl));

        if(result) {}

        // file stuff
        sprintf(filename, "term%d.in", i);
        FILE *f = fopen(filename, "a+");
        fprintf(f, "last line\n");
        fflush(f);
        fclose(f);

        // actual termdriver zap
        zap(termProcTable[i][0]);
        join(&status);
    }

    // eventually, at the end:
    quit(0);
    
}

/* Clock Driver */
static int
ClockDriver(char *arg) {
    int result;
    int status;

    // Let the parent know we are running and enable interrupts.
    semv_real(running);
    psr_set(psr_get() | PSR_CURRENT_INT);



    // Infinite loop until we are zap'd
    while(! is_zapped()) {
		result = waitdevice(CLOCK_DEV, 0, &status);
		if (result != 0) {
			return 0;
		}
		/*
		* Compute the current time and wake up any processes
		* whose time has come.
		*/
        procPtr proc;
        while(sleepHeap.size > 0 && getTime() >= heapPeek(&sleepHeap)->wakeTime)
        {
            proc = heapRemove(&sleepHeap);
            if(debug4)
                console("ClockDriver: Waking up process %d", proc->pid);
            semv_real(proc->blockSem);
        }
    }
    return 0;
}

/* sleep function value extraction */
void sleep(sysargs * args) {
    checkForKernelMode("sleep()");
    int seconds = (long) args->arg1;
	
    int retval = sleepReal(seconds);
	
    args->arg4 = (void *) ((long) retval);
    setUserMode();
}

/* real sleep function */
int sleepReal(int seconds) {
    checkForKernelMode("sleepReal()");

    if (debug4) 
        console("sleepReal: called for process %d with %d seconds\n", getpid(), seconds);

    if (seconds < 0) {
        return -1;
    }

    // init/get the process
    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr proc = &ProcTable[getpid() % MAXPROC];
    
    // set wake time
    proc->wakeTime = getTime() + seconds*1000000;
    if (debug4) 
        console("sleepReal: set wake time for process %d to %d, adding to heap...\n", proc->pid, proc->wakeTime);

    heapAdd(&sleepHeap, proc); // add to sleep heap
    //if (debug4) 
      //  console("sleepReal: Process %d going to sleep until %d\n", proc->pid, proc->wakeTime);
    semp_real(proc->blockSem); // block the process
    //if (debug4) 
      //  console("sleepReal: Process %d woke up, time is %d\n", proc->pid, USLOSS_Clock());
    return 0;
}

/* Terminal Driver */
static int
TermDriver(char *arg) {
    int result;
    int status;
    int unit = atoi( (char *) arg);     // Unit is passed as arg.

    if (debug4) 
        console("TermDriver (unit %d): running\n", unit);

    // Let the parent know we are running
	semv_real(running);
	
    while (!is_zapped()) {
        result = waitdevice(TERM_INT, unit, &status);
        if (result != 0) {
            return 0;
        }

        // Try to receive character
        int recv = TERM_STAT_RECV(status);
        if (recv == DEV_BUSY) {
            MboxCondSend(charRecvMbox[unit], &status, sizeof(int));
        }
        else if (recv == DEV_ERROR) {
            if (debug4) 
                console("TermDriver RECV ERROR\n");
        }

        // Try to send character
        int xmit = TERM_STAT_XMIT(status);
        if (xmit == DEV_READY) {
            MboxCondSend(charSendMbox[unit], &status, sizeof(int));
        }
        else if (xmit == DEV_ERROR) {
            if (debug4) 
                console("TermDriver XMIT ERROR\n");
        }
    }

    return 0;
}

/* Terminal Reader */
static int 
TermReader(char * arg) {
    int unit = atoi( (char *) arg);     // Unit is passed as arg.
    int i;
    int receive; // char to receive
    char line[MAXLINE]; // line being created/read
    int next = 0; // index in line to write char

    if (debug4) 
        console("TermReader (unit %d): running\n", unit);

    // Let the parent know we are running
	semv_real(running);

    for (i = 0; i < MAXLINE; i++) { 
        line[i] = '\0';
    }

    while (!is_zapped()) {
        // receieve characters
        MboxReceive(charRecvMbox[unit], &receive, sizeof(int));
        char ch = TERM_STAT_CHAR(receive);
        line[next] = ch;
        next++;

        // receive line
        if (ch == '\n' || next == MAXLINE) {
            if (debug4) 
                console("TermReader (unit %d): line send\n", unit);

            line[next] = '\0'; // end with null
            MboxSend(lineReadMbox[unit], line, next);

            // reset line
            for (i = 0; i < MAXLINE; i++) {
                line[i] = '\0';
            } 
            next = 0;
        }
    }
    return 0;
}

/* termRead */
void termRead(sysargs * args) {
    if (debug4)
        console("termRead\n");
    checkForKernelMode("termRead()");
    
    char *buffer = (char *) args->arg1;
    int size = (long) args->arg2;
    int unit = (long) args->arg3;

    long retval = termReadReal(unit, size, buffer);

    if (retval == -1) {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode();

    if (debug4) 
        console("termRead (unit %d): retval %d \n", unit, retval);
	
}

/* termReadReal */
int termReadReal(int unit, int size, char *buffer) {
    if (debug4)
        console("termReadReal\n");
    checkForKernelMode("termReadReal");

    if (unit < 0 || unit > TERM_UNITS - 1 || size <= 0) {
        return -1;
    }
    char line[MAXLINE];
    int ctrl = 0;

    //enable term receive interrupts
    if (termInt[unit] == 0) {
        ctrl = TERM_CTRL_RECV_INT(ctrl);
        int result = device_output(TERM_DEV, unit, (void *) ((long) ctrl));
        if(result) {}
        termInt[unit] = 1;
		
        if (debug4)
            console("termReadReal: enable term receive interrupts\n");
    }

    int retval = MboxReceive(lineReadMbox[unit], &line, MAXLINE);
	if(debug4)
			console("termReadReal: after mailbox\n");

    if (debug4) 
        console("termReadReal: unit %d, size %d, retval %d\n", unit, size, retval);

    if (retval > size) {
        retval = size;
    }
	
	if(debug4)
			console("termReadReal: right before buffer\n");
    memcpy(buffer, line, retval);
	
	if(debug4)
			console("termReadReal: size %d, retval %d\n", size, retval);

    return retval;
}

/* Terminal Writer */
static int 
TermWriter(char * arg) {
    int unit = atoi( (char *) arg);     // Unit is passed as arg.
    int size;
    int ctrl = 0;
    int next;
    int status;
    char line[MAXLINE];

    if (debug4) 
        console("TermWriter (unit %d): running\n", unit);

    // Let the parent know we are running
	semv_real(running);

    while (!is_zapped()) {
        size = MboxReceive(lineWriteMbox[unit], line, MAXLINE); // get line and size
		//console("TermWriter: unit %d, size %d\n", unit, size);	
		
        if (is_zapped()) {
			//console("TermWriter: break\n");
            break;
		}
		
        // enable xmit interrupt
        //ctrl = TERM_CTRL_XMIT_INT(ctrl);
        //int result = device_output(TERM_DEV, unit, (void *) ((long) ctrl));
        //if(result) {}

        // xmit the line
        next = 0;
        while (next < size) {
            MboxReceive(charSendMbox[unit], &status, sizeof(int));
			//console("TermWriter: unit %d, status %d\n", unit, status);
			
            // xmit the character
            int x = TERM_STAT_XMIT(status);
            if (x == DEV_READY) {
                //console("TermWriter: %c string %d unit\n", line[next], unit);

                ctrl = 0;
                //ctrl = TERM_CTRL_RECV_INT(ctrl);
                ctrl = TERM_CTRL_CHAR(ctrl, line[next]);
                ctrl = TERM_CTRL_XMIT_CHAR(ctrl);
                ctrl = TERM_CTRL_XMIT_INT(ctrl);

                int result2 = device_output(TERM_DEV, unit, (void *) ((long) ctrl));
                if(result2) {}
            }

            next++;
        }

        // enable receive interrupt
        ctrl = 0;
        if (termInt[unit] == 1) {
            ctrl = TERM_CTRL_RECV_INT(ctrl);
			int result3 = device_output(TERM_DEV, unit, (void *) ((long) ctrl));
			if(result3) {}
			termInt[unit] = 0;

			if (debug4)
				console("TermWriter: enable term receive interrupts\n");
		}
		
        int pid; 
        MboxReceive(pidMbox[unit], &pid, sizeof(int));
        semv_real(ProcTable[pid % MAXPROC].blockSem);
		//console("TermWriter: pid %d unblocked\n", pid);	           
    }

    return 0;
}

/* termWrite */
void termWrite(sysargs * args) {
    if (debug4)
        console("termWrite\n");
    checkForKernelMode("termWrite()");
    
    char *text = (char *) args->arg1;
    int size = (long) args->arg2;
    int unit = (long) args->arg3;

    long retval = termWriteReal(unit, size, text);

    if (retval == -1) {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode(); 
}

/* termWriteReal */
int termWriteReal(int unit, int size, char *text) {
    if (debug4)
        console("termWriteReal\n");
    checkForKernelMode("termWriteReal()");

    if (unit < 0 || unit > TERM_UNITS - 1 || size < 0) {
        return -1;
    }

    int pid = getpid();
    //console("termWriteReal: 574 pid %d\n", pid);	
    MboxSend(pidMbox[unit], &pid, sizeof(int));
    //console("termWriteReal: 576 size %d\n", size);
    MboxSend(lineWriteMbox[unit], text, size);
    //console("termWriteReal: pid %d blocked\n", pid);	
    semp_real(ProcTable[pid % MAXPROC].blockSem);
    //console("termWriteReal: 580 semp_real return\n ");    
	return size;
}


/* Disk Driver */
static int
DiskDriver(char *arg)
{
    int result;
    int status;
    int unit = atoi( (char *) arg);     // Unit is passed as arg.

    // get set up in proc table
    initProc(getpid());
    procPtr me = &ProcTable[getpid() % MAXPROC];
    initDiskQueue(&diskQs[unit]);

    if (debug4) {
        console("DiskDriver: unit %d started, pid = %d\n", unit, me->pid);
    }

    // Let the parent know we are running and enable interrupts.
    semv_real(running);
    psr_set(psr_get() | PSR_CURRENT_INT);



    // Infinite loop until we are zap'd
    while(!is_zapped()) {
        // block on sem until we get request
        semp_real(me->blockSem);
        if (debug4) {
            console("DiskDriver: unit %d unblocked, zapped = %d, queue size = %d\n", unit, is_zapped(), diskQs[unit].size);
        }
        if (is_zapped()) // check  if we were zapped
            return 0;
        
        // get request off queue
        if (diskQs[unit].size > 0) {
            procPtr proc = peekDiskQ(&diskQs[unit]);
            int track = proc->diskTrack;

            if (debug4) {
                console("DiskDriver: taking request from pid %d, track %d\n", proc->pid, proc->diskTrack);
            }

            // handle tracks request
            if (proc->diskRequest.opr == DISK_TRACKS) {
                int result3 = device_output(DISK_DEV, unit, &proc->diskRequest);
                 
                if(result3) {}

                result = waitdevice(DISK_DEV, unit, &status);
                if (result != 0) {
                    //console("exiting deskdriver 1\n");
                    return 0;
                }
            }

            else { // handle read/write requests
                while (proc->diskSectors > 0) {
                    // seek to needed track
                    device_request request;
                    request.opr = DISK_SEEK;
                    request.reg1 = &track;
                    int result4 = device_output(DISK_DEV, unit, &request);
                    
                    if(result4) {}

                    // wait for result
                    result = waitdevice(DISK_DEV, unit, &status);
                    if (result != 0) {
                        //console("exiting diskdriver 2\n");
                        return 0;
                    }

                    if (debug4) {
                        console("DiskDriver: seeked to track %d, status = %d, result = %d\n", track, status, result);
                    }

                    // read/write the sectors
                    int s;
                    for (s = proc->diskFirstSec; proc->diskSectors > 0 && s < DISK_TRACK_SIZE; s++) {
                        proc->diskRequest.reg1 = (void *) ((long) s);
                        int result5 = device_output(DISK_DEV, unit, &proc->diskRequest);
                        
                        if(result5) {}

                        result = waitdevice(DISK_DEV, unit, &status);
                        if (result != 0) {
                            //console("exiting diskdriver 3\n");
                            return 0;
                        }

                        if (debug4) {
                            console("DiskDriver: read/wrote sector %d, status = %d, result = %d, buffer = %s\n", s, status, result, proc->diskRequest.reg2);
                        }

                        proc->diskSectors--;
                        proc->diskRequest.reg2 += DISK_SECTOR_SIZE;
                    }

                    // request first sector of next track
                    track++;
                    proc->diskFirstSec = 0;
                }
            }

            if (debug4) 
                console("DiskDriver: finished request from pid %d\n", proc->pid, result, status);

            removeDiskQ(&diskQs[unit]); // remove proc from queue
            semv_real(proc->blockSem); // unblock caller
        }

    }

    //semv_real(running); // unblock parent
    //console("exiting diskDriver\n");
    return 0;
}


/* extract values from sysargs and call diskReadReal */
void diskRead(sysargs * args) {
    checkForKernelMode("diskRead()");

    int sectors = (long) args->arg2;
    int track = (long) args->arg3;
    int first = (long) args->arg4;
    int unit = (long) args->arg5;

    int retval = diskReadReal(unit, track, first, sectors, args->arg1);

    if (retval == -1) {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}

/* extract values from sysargs and call diskWriteReal */
void diskWrite(sysargs * args) {
    checkForKernelMode("diskWrite()");

    int sectors = (long) args->arg2;
    int track = (long) args->arg3;
    int first = (long) args->arg4;
    int unit = (long) args->arg5;

    int retval = diskWriteReal(unit, track, first, sectors, args->arg1);

    if (retval == -1) {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}

int diskWriteReal(int unit, int track, int first, int sectors, void *buffer) {
    checkForKernelMode("diskWriteReal()");
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 1);
}

int diskReadReal(int unit, int track, int first, int sectors, void *buffer) {
    checkForKernelMode("diskWriteReal()");
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 0);
}

/*------------------------------------------------------------------------
    diskReadOrWriteReal: Reads or writes to the desk depending on the 
                        value of write; write if write == 1, else read.
    Returns: -1 if given illegal input, 0 otherwise
 ------------------------------------------------------------------------*/
int diskReadOrWriteReal(int unit, int track, int first, int sectors, void *buffer, int write) {
    if (debug4)
        console("diskReadOrWriteReal: called with unit: %d, track: %d, first: %d, sectors: %d, write: %d\n", unit, track, first, sectors, write);

    // check for illegal args
    if (unit < 0 || unit > 1 || track < 0 || track > ProcTable[diskPids[unit]].diskTrack ||
        first < 0 || first > DISK_TRACK_SIZE || buffer == NULL  ||
        (first + sectors)/DISK_TRACK_SIZE + track > ProcTable[diskPids[unit]].diskTrack) {
        return -1;
    }

    procPtr driver = &ProcTable[diskPids[unit]];

    // init/get the process
    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr proc = &ProcTable[getpid() % MAXPROC];

    if (write)
        proc->diskRequest.opr = DISK_WRITE;
    else
        proc->diskRequest.opr = DISK_READ;
    proc->diskRequest.reg2 = buffer;
    proc->diskTrack = track;
    proc->diskFirstSec = first;
    proc->diskSectors = sectors;
    proc->diskBuffer = buffer;

    addDiskQ(&diskQs[unit], proc); // add to disk queue 
    semv_real(driver->blockSem);  // wake up disk driver
    semp_real(proc->blockSem); // block

    int status;
    int result = device_input(DISK_DEV, unit, &status);

    if (debug4)
        console("diskReadOrWriteReal: finished, status = %d, result = %d\n", status, result);

    return result;
}

/* extract values from sysargs and call diskSizeReal */
void diskSize(sysargs * args) {
    checkForKernelMode("diskSize()");
    int unit = (long) args->arg1;
    int sector, track, disk;
    int retval = diskSizeReal(unit, &sector, &track, &disk);
    args->arg1 = (void *) ((long) sector);
    args->arg2 = (void *) ((long) track);
    args->arg3 = (void *) ((long) disk);
    args->arg4 = (void *) ((long) retval);
    setUserMode();
}

/*------------------------------------------------------------------------
    diskSizeReal: Puts values into pointers for the size of a sector, 
    number of sectors per track, and number of tracks on the disk for the 
    given unit. 
    Returns: -1 if given illegal input, 0 otherwise
 ------------------------------------------------------------------------*/
int diskSizeReal(int unit, int *sector, int *track, int *disk) {
    checkForKernelMode("diskSizeReal()");

    // check for illegal args
    if (unit < 0 || unit > 1 || sector == NULL || track == NULL || disk == NULL) {
        if (debug4)
            console("diskSizeReal: given illegal argument(s), returning -1\n");
        return -1;
    }

    procPtr driver = &ProcTable[diskPids[unit]];

    // get the number of tracks for the first time
    if (driver->diskTrack == -1) {
        // init/get the process
        if (ProcTable[getpid() % MAXPROC].pid == -1) {
            initProc(getpid());
        }
        procPtr proc = &ProcTable[getpid() % MAXPROC];

        // set variables
        proc->diskTrack = 0;
        device_request request;
        request.opr = DISK_TRACKS;
        request.reg1 = &driver->diskTrack;
        proc->diskRequest = request;

        addDiskQ(&diskQs[unit], proc); // add to disk queue 
        semv_real(driver->blockSem);  // wake up disk driver
        semp_real(proc->blockSem); // block

        if (debug4)
            console("diskSizeReal: number of tracks on unit %d: %d\n", unit, driver->diskTrack);
    }

    *sector = DISK_SECTOR_SIZE;
    *track = DISK_TRACK_SIZE;
    *disk = driver->diskTrack;
    return 0;
}


/* ------------------------------------------------------------------------
   Name - requireKernelMode
   Purpose - Checks if we are in kernel mode and prints an error messages
              and halts USLOSS if not.
   Parameters - The name of the function calling it, for the error message.
   Side Effects - Prints and halts if we are not in kernel mode
   ------------------------------------------------------------------------ */
void checkForKernelMode(char *name)
{
    if( (PSR_CURRENT_MODE & psr_get()) == 0 ) {
        console("%s: called while in user mode, by process %d. Halting...\n", 
             name, getpid());
        halt(1); 
    }
} 

/* ------------------------------------------------------------------------
   Name - setUserMode
   Purpose - switches to user mode
   Parameters - none
   Side Effects - switches to user mode
   ------------------------------------------------------------------------ */
void setUserMode()
{
    psr_set( psr_get() & ~PSR_CURRENT_MODE );

 
}

/* initializes proc struct */
void initProc(int pid) {
    checkForKernelMode("initProc()"); 

    int i = pid % MAXPROC;

    ProcTable[i].pid = pid; 
    ProcTable[i].mboxID = MboxCreate(0, 0);
    ProcTable[i].blockSem = semcreate_real(0);
    ProcTable[i].wakeTime = -1;
    ProcTable[i].diskTrack = -1;
    ProcTable[i].nextDiskPtr = NULL;
    ProcTable[i].prevDiskPtr = NULL;
}

/* empties proc struct */
void emptyProc(int pid) {
    checkForKernelMode("emptyProc()"); 

    int i = pid % MAXPROC;

    ProcTable[i].pid = -1; 
    ProcTable[i].mboxID = -1;
    ProcTable[i].blockSem = -1;
    ProcTable[i].wakeTime = -1;
    ProcTable[i].nextDiskPtr = NULL;
    ProcTable[i].prevDiskPtr = NULL;
}

/* ------------------------------------------------------------------------
  Functions for the dskQueue and heap.
   ----------------------------------------------------------------------- */

/* Initialize the given diskQueue */
void initDiskQueue(diskQueue* q) {
    q->head = NULL;
    q->tail = NULL;
    q->curr = NULL;
    q->size = 0;
}

/* Adds the proc pointer to the disk queue in sorted order */
void addDiskQ(diskQueue* q, procPtr p) {
    if (debug4)
        console("addDiskQ: adding pid %d, track %d to queue\n", p->pid, p->diskTrack);

    // first add
    if (q->head == NULL) { 
        q->head = q->tail = p;
        q->head->nextDiskPtr = q->tail->nextDiskPtr = NULL;
        q->head->prevDiskPtr = q->tail->prevDiskPtr = NULL;
    }
    else {
        // find the right location to add
        procPtr prev = q->tail;
        procPtr next = q->head;
        while (next != NULL && next->diskTrack <= p->diskTrack) {
            prev = next;
            next = next->nextDiskPtr;
            if (next == q->head)
                break;
        }
        if (debug4)
            console("addDiskQ: found place, prev = %d\n", prev->diskTrack);
        prev->nextDiskPtr = p;
        p->prevDiskPtr = prev;
        if (next == NULL)
            next = q->head;
        p->nextDiskPtr = next;
        next->prevDiskPtr = p;
        if (p->diskTrack < q->head->diskTrack)
            q->head = p; // update head
        if (p->diskTrack >= q->tail->diskTrack)
            q->tail = p; // update tail
    }
    q->size++;
    if (debug4)
        console("addDiskQ: add complete, size = %d\n", q->size);
} 

/* Returns the next proc on the disk queue */
procPtr peekDiskQ(diskQueue* q) {
    if (q->curr == NULL) {
        q->curr = q->head;
    }

    return q->curr;
}

/* Returns and removes the next proc on the disk queue */
procPtr removeDiskQ(diskQueue* q) {
    if (q->size == 0)
        return NULL;

    if (q->curr == NULL) {
        q->curr = q->head;
    }

    if (debug4)
        console("removeDiskQ: called, size = %d, curr pid = %d, curr track = %d\n", q->size, q->curr->pid, q->curr->diskTrack);

    procPtr temp = q->curr;

    if (q->size == 1) { // remove only node
        q->head = q->tail = q->curr = NULL;
    }

    else if (q->curr == q->head) { // remove head
        q->head = q->head->nextDiskPtr;
        q->head->prevDiskPtr = q->tail;
        q->tail->nextDiskPtr = q->head;
        q->curr = q->head;
    }

    else if (q->curr == q->tail) { // remove tail
        q->tail = q->tail->prevDiskPtr;
        q->tail->nextDiskPtr = q->head;
        q->head->prevDiskPtr = q->tail;
        q->curr = q->head;
    }

    else { // remove other
        q->curr->prevDiskPtr->nextDiskPtr = q->curr->nextDiskPtr;
        q->curr->nextDiskPtr->prevDiskPtr = q->curr->prevDiskPtr;
        q->curr = q->curr->nextDiskPtr;
    }

    q->size--;

    if (debug4)
        console("removeDiskQ: done, size = %d, curr pid = %d, curr track = %d\n", q->size, temp->pid, temp->diskTrack);

    return temp;
} 


/* Setup heap, implementation based on https://gist.github.com/aatishnn/8265656 */
void initHeap(heap* h) {
    h->size = 0;
}

/* Add to heap */
void heapAdd(heap * h, procPtr p) {
    // start from bottom and find correct place
    int i, parent;
    for (i = h->size; i > 0; i = parent) {
        parent = (i-1)/2;
        if (h->procs[parent]->wakeTime <= p->wakeTime)
            break;
        // move parent down
        h->procs[i] = h->procs[parent];
    }
    h->procs[i] = p; // put at final location
    h->size++;
    if (debug4) 
        console("heapAdd: Added proc %d to heap at index %d, size = %d\n", p->pid, i, h->size);
} 

/* Return min process on heap */
procPtr heapPeek(heap * h) {
    return h->procs[0];
}

/* Remove earlist waking process form the heap */
procPtr heapRemove(heap * h) {
  if (h->size == 0)
    return NULL;

    procPtr removed = h->procs[0]; // remove min
    h->size--;
    h->procs[0] = h->procs[h->size]; // put last in first spot

    // re-heapify
    int i = 0, left, right, min = 0;
    while (i*2 <= h->size) {
        // get locations of children
        left = i*2 + 1;
        right = i*2 + 2;

        // get min child
        if (left <= h->size && h->procs[left]->wakeTime < h->procs[min]->wakeTime) 
            min = left;
        if (right <= h->size && h->procs[right]->wakeTime < h->procs[min]->wakeTime) 
            min = right;

        // swap current with min child if needed
        if (min != i) {
            procPtr temp = h->procs[i];
            h->procs[i] = h->procs[min];
            h->procs[min] = temp;
            i = min;
        }
        else
            break; // otherwise we're done
    }
    if (debug4) 
        console("heapRemove: Called, returning pid %d, size = %d\n", removed->pid, h->size);
    return removed;
}

int getTime() 
{
  int result, unit = 0, status;

  result = device_input(CLOCK_DEV, unit, &status);

  if(result == DEV_INVALID)
  {
    console("clock device invalid.\n");
    halt(1);
  }

  return status;
}

