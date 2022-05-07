/**************************************************************************************************
 * phase4.c
 *
 * Kyle AuBuchon
 *************************************************************************************************/
#include <stdlib.h>

#include <stdio.h>

#include <strings.h>

#include <usloss.h>

#include <phase1.h>

#include <phase2.h>

#include <phase3.h>

#include <phase4.h>

#include <usyscall.h>

#include <string.h>

#include <provided_prototypes.h>

#include "driver.h"

#include "proc4structs.h"

#include <libuser.h>


int debug4 = 0;
static int running;

static int ClockDriver(char * );
static int DiskDriver(char * );
static int TermDriver(char * );
static int TermReader(char * );
static int TermWriter(char * );
extern int start4();

void sleep(sysargs * );
void diskRead(sysargs * );
void diskWrite(sysargs * );
void diskSize(sysargs * );
void termRead(sysargs * );
void termWrite(sysargs * );

int sleepReal(int seconds);
int diskSizeReal(int, int * , int * , int * );
int diskWriteReal(int, int, int, int, void * );
int diskReadReal(int, int, int, int, void * );
int diskReadOrWriteReal(int, int, int, int, void * , int);
int termReadReal(int, int, char * );
int termWriteReal(int, int, char * );
int getTime();

void check_kernel_mode(char * );
void emptyProc(int);
void initProc(int);
void set_user_mode();
void initDiskQueue(diskQueue * );
void addDiskQ(diskQueue * , procPtr);
procPtr peekDiskQ(diskQueue * );
procPtr removeDiskQ(diskQueue * );
void initHeap(heap * );
void heapAdd(heap * , procPtr);
procPtr heapPeek(heap * );
procPtr heapRemove(heap * );

/* Globals */
procStruct ProcTable[MAXPROC];
heap sleepHeap;
int diskIsZapped;
diskQueue diskQs[DISK_UNITS];
int diskPids[DISK_UNITS]; // pids for disk drivers

int charRecBox[TERM_UNITS]; // mailbox to receive char
int charSendBox[TERM_UNITS]; // mailbox to send char
int lineReadBox[TERM_UNITS]; // mailbox to read line
int lineWriteBox[TERM_UNITS]; // mailbox to write line
int pidBox[TERM_UNITS]; // mailbox to send pids to
int termInt[TERM_UNITS]; // interrupt for term (control writing)

int termProcTable[TERM_UNITS][3];

void
start3(void) {
    char name[128];
    char termbuf[10];
    char diskbuf[10];
    int i;
    int clockPID;
    int pid;
    int status;

    /*
     * Check kernel mode here.
     */
    check_kernel_mode("start3()");

    // initialize proc table
    for (i = 0; i < MAXPROC; i++) {
        initProc(i);
    }

    // sleep queue
    initHeap( & sleepHeap);

    /* Assignment system call handlers */
    sys_vec[SYS_SLEEP] = sleep;
    sys_vec[SYS_DISKREAD] = diskRead;
    sys_vec[SYS_DISKWRITE] = diskWrite;
    sys_vec[SYS_DISKSIZE] = diskSize;
    sys_vec[SYS_TERMREAD] = termRead;
    sys_vec[SYS_TERMWRITE] = termWrite;

    // mboxes for terminal
    for (i = 0; i < TERM_UNITS; i++) {
        charRecBox[i] = MboxCreate(1, MAXLINE);
        charSendBox[i] = MboxCreate(1, MAXLINE);
        lineReadBox[i] = MboxCreate(10, MAXLINE);
        lineWriteBox[i] = MboxCreate(10, MAXLINE);
        pidBox[i] = MboxCreate(1, sizeof(int));
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
        diskSizeReal(i, & temp, & temp, & ProcTable[pid % MAXPROC].diskTrack);
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
    pid = wait_real( & status);

    /*
     * Zap the device drivers
     */

    status = 0;

    // clock
    zap(clockPID);
    join( & status);

    // disk
    for (i = 0; i < DISK_UNITS; i++) {
        semv_real(ProcTable[diskPids[i]].blockSem);
        zap(diskPids[i]);
        join( & status);
    }

    // termreader
    for (i = 0; i < TERM_UNITS; i++) {
        MboxSend(charRecBox[i], NULL, 0);
        zap(termProcTable[i][1]);
        join( & status);
    }

    // termwriter
    for (i = 0; i < TERM_UNITS; i++) {
        MboxSend(lineWriteBox[i], NULL, 0);
        zap(termProcTable[i][2]);
        join( & status);
    }

    // termdriver
    char filename[50];
    for (i = 0; i < TERM_UNITS; i++) {
        int ctrl = 0;
        ctrl = TERM_CTRL_RECV_INT(ctrl);
        int result = device_output(TERM_DEV, i, (void * )((long) ctrl));

        if (result) {}

        // close out the file file stuff
        sprintf(filename, "term%d.in", i);
        FILE * f = fopen(filename, "a+");
        fprintf(f, "last line\n");
        fflush(f);
        fclose(f);

        // termdriver
        zap(termProcTable[i][0]);
        join( & status);
    }

    // eventually, at the end:
    quit(0);

}
/*-----------------------start3-----------------------*/

/*-----------------------ClockDriver-----------------------*/
static int
ClockDriver(char * arg) {
    int result;
    int status;

    // Let the parent know we are running and enable interrupts.
    semv_real(running);
    psr_set(psr_get() | PSR_CURRENT_INT);

    // Infinite loop until we are zap'd
    while (!is_zapped()) {
        result = waitdevice(CLOCK_DEV, 0, & status);
        if (result != 0) {
            return 0;
        }
        /*
         * Compute the current time and wake up any processes
         * whose time has come.
         */
        procPtr proc;
        while (sleepHeap.size > 0 && getTime() >= heapPeek( & sleepHeap) -> wakeTime) {
            proc = heapRemove( & sleepHeap);
            if (debug4)
                console("ClockDriver: Waking up process %d", proc -> pid);
            semv_real(proc -> blockSem);
        }
    }
    return 0;
}
/*-----------------------ClockDriver-----------------------*/

/*-----------------------sleep-----------------------*/ // todo fix sleep :/
void sleep(sysargs * args) {
    check_kernel_mode("sleep()");
    int seconds = (long) args -> arg1;

    int retval = sleepReal(seconds);

    args -> arg4 = (void * )((long) retval);
    set_user_mode();
}
/*-----------------------sleep-----------------------*/

/*-----------------------sleepReal-----------------------*/
int sleepReal(int seconds) {
    check_kernel_mode("sleepReal()");

    if (debug4)
        console("sleepReal: called for process %d with %d seconds\n", getpid(), seconds);

    if (seconds < 0) {
        return -1;
    }

    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr proc = & ProcTable[getpid() % MAXPROC];

    // set wake time
    proc -> wakeTime = getTime() + seconds * 1000000;
    if (debug4)
        console("sleepReal: set wake time for process %d to %d, adding to heap...\n", proc -> pid, proc -> wakeTime);

    heapAdd( & sleepHeap, proc);
    // put to sleep by blocking 
    semp_real(proc -> blockSem); // block the process

    return 0;
}
/*-----------------------sleepReal-----------------------*/

/*-----------------------TermDriver-----------------------*/
static int
TermDriver(char * arg) {
    int result;
    int status;
    int unit = atoi((char * ) arg); // Unit is passed as arg.

    if (debug4)
        console("TermDriver (unit %d): running\n", unit);

    // Let the parent know we are running
    semv_real(running);

    while (!is_zapped()) {
        result = waitdevice(TERM_INT, unit, & status);
        if (result != 0) {
            return 0;
        }

        // Try to receive character
        int recv = TERM_STAT_RECV(status);
        if (recv == DEV_BUSY) {
            MboxCondSend(charRecBox[unit], & status, sizeof(int));
        } else if (recv == DEV_ERROR) {
            if (debug4)
                console("TermDriver RECV ERROR\n");
        }

        // Try to send character
        int xmit = TERM_STAT_XMIT(status);
        if (xmit == DEV_READY) {
            MboxCondSend(charSendBox[unit], & status, sizeof(int));
        } else if (xmit == DEV_ERROR) {
            if (debug4)
                console("TermDriver XMIT ERROR\n");
        }
    }

    return 0;
}
/*-----------------------TermDriver-----------------------*/

/*-----------------------TermReader-----------------------*/
static int
TermReader(char * arg) {
    int unit = atoi((char * ) arg); // Unit is passed as arg.
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
        MboxReceive(charRecBox[unit], & receive, sizeof(int));
        char ch = TERM_STAT_CHAR(receive);
        line[next] = ch;
        next++;

        // receive line
        if (ch == '\n' || next == MAXLINE) {
            if (debug4)
                console("TermReader (unit %d): line send\n", unit);

            line[next] = '\0'; // end with null
            MboxSend(lineReadBox[unit], line, next);

            // reset line
            for (i = 0; i < MAXLINE; i++) {
                line[i] = '\0';
            }
            next = 0;
        }
    }
    return 0;
}
/*-----------------------TermReader-----------------------*/

/*-----------------------termRead-----------------------*/
void termRead(sysargs * args) {
    if (debug4)
        console("termRead\n");
    check_kernel_mode("termRead()");

    char * buffer = (char * ) args -> arg1;
    int size = (long) args -> arg2;
    int unit = (long) args -> arg3;

    long retval = termReadReal(unit, size, buffer);

    if (retval == -1) {
        args -> arg2 = (void * )((long) retval);
        args -> arg4 = (void * )((long) - 1);
    } else {
        args -> arg2 = (void * )((long) retval);
        args -> arg4 = (void * )((long) 0);
    }
    set_user_mode();

    if (debug4)
        console("termRead (unit %d): retval %d \n", unit, retval);

}
/*-----------------------termRead-----------------------*/

/*-----------------------termReadReal-----------------------*/
int termReadReal(int unit, int size, char * buffer) {
    if (debug4)
        console("termReadReal\n");
    check_kernel_mode("termReadReal");

    if (unit < 0 || unit > TERM_UNITS - 1 || size <= 0) {
        return -1;
    }
    char line[MAXLINE];
    int ctrl = 0;

    //enable term receive interrupts
    if (termInt[unit] == 0) {
        ctrl = TERM_CTRL_RECV_INT(ctrl);
        int result = device_output(TERM_DEV, unit, (void * )((long) ctrl));
        if (result) {}
        termInt[unit] = 1;

        if (debug4)
            console("termReadReal: enable term receive interrupts\n");
    }

    int retval = MboxReceive(lineReadBox[unit], & line, MAXLINE);
    if (debug4)
        console("termReadReal: after mailbox\n");

    if (debug4)
        console("termReadReal: unit %d, size %d, retval %d\n", unit, size, retval);

    if (retval > size) {
        retval = size;
    }

    if (debug4)
        console("termReadReal: right before buffer\n");
    memcpy(buffer, line, retval);

    if (debug4)
        console("termReadReal: size %d, retval %d\n", size, retval);

    return retval;
}
/*-----------------------termReadReal-----------------------*/

/*-----------------------TermWriter-----------------------*/
static int TermWriter(char * arg) {
    int unit = atoi((char * ) arg); // Unit is passed as arg.
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
        size = MboxReceive(lineWriteBox[unit], line, MAXLINE);

        if (is_zapped()) {
            break;
        }

        next = 0;
        while (next < size) {
            MboxReceive(charSendBox[unit], & status, sizeof(int));

            // xmit the character
            int x = TERM_STAT_XMIT(status);
            if (x == DEV_READY) {

                ctrl = 0;
                //ctrl = TERM_CTRL_RECV_INT(ctrl);
                ctrl = TERM_CTRL_CHAR(ctrl, line[next]);
                ctrl = TERM_CTRL_XMIT_CHAR(ctrl);
                ctrl = TERM_CTRL_XMIT_INT(ctrl);

                int result2 = device_output(TERM_DEV, unit, (void * )((long) ctrl));
                if (result2) {}
            }

            next++;
        }

        // enable receive interrupt
        ctrl = 0;
        if (termInt[unit] == 1) {
            ctrl = TERM_CTRL_RECV_INT(ctrl);
            int result3 = device_output(TERM_DEV, unit, (void * )((long) ctrl));
            if (result3) {}
            termInt[unit] = 0;

            if (debug4)
                console("TermWriter: enable term receive interrupts\n");
        }

        int pid;
        MboxReceive(pidBox[unit], & pid, sizeof(int));
        semv_real(ProcTable[pid % MAXPROC].blockSem);
    }

    return 0;
}
/*-----------------------TermWriter-----------------------*/

/*-----------------------termWrite-----------------------*/
void termWrite(sysargs * args) {
    if (debug4)
        console("termWrite\n");
    check_kernel_mode("termWrite()");

    char * text = (char * ) args -> arg1;
    int size = (long) args -> arg2;
    int unit = (long) args -> arg3;

    long retval = termWriteReal(unit, size, text);

    if (retval == -1) {
        args -> arg2 = (void * )((long) retval);
        args -> arg4 = (void * )((long) - 1);
    } else {
        args -> arg2 = (void * )((long) retval);
        args -> arg4 = (void * )((long) 0);
    }
    set_user_mode();
}
/*-----------------------termWrite-----------------------*/

/*-----------------------termWriteReal-----------------------*/
int termWriteReal(int unit, int size, char * text) {
    if (debug4)
        console("termWriteReal\n");
    check_kernel_mode("termWriteReal()");

    if (unit < 0 || unit > TERM_UNITS - 1 || size < 0) {
        return -1;
    }

    int pid = getpid();
    MboxSend(pidBox[unit], & pid, sizeof(int));
    MboxSend(lineWriteBox[unit], text, size);
    semp_real(ProcTable[pid % MAXPROC].blockSem);
    return size;
}
/*-----------------------termWriteReal-----------------------*/

/*-----------------------DiskDriver-----------------------*/
static int DiskDriver(char * arg) {
    int unit = atoi((char * ) arg); // grab args for unit
    int result;
    int status;

    // get set up in proc table
    initProc(getpid());
    procPtr me = & ProcTable[getpid() % MAXPROC];
    initDiskQueue( & diskQs[unit]);

    if (debug4) {
        console("DiskDriver: unit %d started, pid = %d\n", unit, me -> pid);
    }

    // indicate we are running and enable itnerruptus
    semv_real(running);
    psr_set(psr_get() | PSR_CURRENT_INT);

    // Infinite loop until we are zap'd
    while (!is_zapped()) {
        // block on sem 
        semp_real(me -> blockSem);
        if (debug4) {
            console("DiskDriver: unit %d unblocked, zapped = %d, queue size = %d\n", unit, is_zapped(), diskQs[unit].size);
        }
        if (is_zapped()) // check  if we were zapped
            return 0;

        // get request off queue
        if (diskQs[unit].size > 0) {
            procPtr proc = peekDiskQ( & diskQs[unit]);
            int track = proc -> diskTrack;

            if (debug4) {
                console("DiskDriver: taking request from pid %d, track %d\n", proc -> pid, proc -> diskTrack);
            }

            // handle tracks request
            if (proc -> diskRequest.opr == DISK_TRACKS) {
                int result3 = device_output(DISK_DEV, unit, & proc -> diskRequest);

                if (result3) {}

                result = waitdevice(DISK_DEV, unit, & status);
                if (result != 0) {
                    if (debug4) {
                        console("DiskDriver: results exiting\n");
                    }

                    return 0;
                }
            } else { // handle read/write requests
                while (proc -> diskSectors > 0) {
                    // seek to needed track
                    device_request request;
                    request.opr = DISK_SEEK;
                    request.reg1 = & track;
                    int result4 = device_output(DISK_DEV, unit, & request);

                    if (result4) {}

                    // wait for result
                    result = waitdevice(DISK_DEV, unit, & status);
                    if (result != 0) {
                        return 0;
                    }

                    if (debug4) {
                        console("DiskDriver: seeked to track %d, status = %d, result = %d\n", track, status, result);
                    }

                    // read/write the sectors
                    int s;
                    for (s = proc -> diskFirstSec; proc -> diskSectors > 0 && s < DISK_TRACK_SIZE; s++) {
                        proc -> diskRequest.reg1 = (void * )((long) s);
                        int result5 = device_output(DISK_DEV, unit, & proc -> diskRequest);

                        if (result5) {}

                        result = waitdevice(DISK_DEV, unit, & status);
                        if (result != 0) {

                            return 0;
                        }

                        if (debug4) {
                            console("DiskDriver: read/wrote sector %d, status = %d, result = %d, buffer = %s\n", s, status, result, proc -> diskRequest.reg2);
                        }

                        proc -> diskSectors--;
                        proc -> diskRequest.reg2 += DISK_SECTOR_SIZE;
                    }

                    // request first sector of next track
                    track++;
                    proc -> diskFirstSec = 0;
                }
            }

            if (debug4)
                console("DiskDriver: finished request from pid %d\n", proc -> pid, result, status);

            removeDiskQ( & diskQs[unit]); // remove proc from queue
            semv_real(proc -> blockSem); // unblock caller
        }

    }

    return 0;
}
/*-----------------------DiskDriver-----------------------*/

/*-----------------------diskRead-----------------------*/
void diskRead(sysargs * args) {
    check_kernel_mode("diskRead()");

    int sectors = (long) args -> arg2;
    int track = (long) args -> arg3;
    int first = (long) args -> arg4;
    int unit = (long) args -> arg5;

    int retval = diskReadReal(unit, track, first, sectors, args -> arg1);

    if (retval == -1) {
        args -> arg1 = (void * )((long) retval);
        args -> arg4 = (void * )((long) - 1);
    } else {
        args -> arg1 = (void * )((long) retval);
        args -> arg4 = (void * )((long) 0);
    }
    set_user_mode();
}
/*-----------------------diskRead-----------------------*/

/*-----------------------diskWrite-----------------------*/
void diskWrite(sysargs * args) {
    check_kernel_mode("diskWrite()");

    int sectors = (long) args -> arg2;
    int track = (long) args -> arg3;
    int first = (long) args -> arg4;
    int unit = (long) args -> arg5;

    int retval = diskWriteReal(unit, track, first, sectors, args -> arg1);

    if (retval == -1) {
        args -> arg1 = (void * )((long) retval);
        args -> arg4 = (void * )((long) - 1);
    } else {
        args -> arg1 = (void * )((long) retval);
        args -> arg4 = (void * )((long) 0);
    }
    set_user_mode();
}
/*-----------------------diskWrite-----------------------*/

/*-----------------------diskWriteReal-----------------------*/
int diskWriteReal(int unit, int track, int first, int sectors, void * buffer) {
    check_kernel_mode("diskWriteReal()");
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 1);
}
/*-----------------------diskWriteReal-----------------------*/

/*-----------------------diskReadReal-----------------------*/
int diskReadReal(int unit, int track, int first, int sectors, void * buffer) {
    check_kernel_mode("diskWriteReal()");
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 0);
}
/*-----------------------diskReadReal-----------------------*/

/*-----------------------diskReadOrWriteReal-----------------------*/
int diskReadOrWriteReal(int unit, int track, int first, int sectors, void * buffer, int write) {
    if (debug4)
        console("diskReadOrWriteReal: called with unit: %d, track: %d, first: %d, sectors: %d, write: %d\n", unit, track, first, sectors, write);

    // validity check on arguments
    if (unit < 0 || unit > 1 || track < 0 || track > ProcTable[diskPids[unit]].diskTrack ||
        first < 0 || first > DISK_TRACK_SIZE || buffer == NULL ||
        (first + sectors) / DISK_TRACK_SIZE + track > ProcTable[diskPids[unit]].diskTrack) {
        if (debug4) {
            console("diskReadorWriteReal: illegal args return -1");
        }
        return -1;
    }

    procPtr driver = & ProcTable[diskPids[unit]];

    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr proc = & ProcTable[getpid() % MAXPROC];

    if (write)
        proc -> diskRequest.opr = DISK_WRITE;
    else
        proc -> diskRequest.opr = DISK_READ;
    proc -> diskRequest.reg2 = buffer;
    proc -> diskTrack = track;
    proc -> diskFirstSec = first;
    proc -> diskSectors = sectors;
    proc -> diskBuffer = buffer;

    addDiskQ( & diskQs[unit], proc); // add 
    semv_real(driver -> blockSem); // wake 
    semp_real(proc -> blockSem); // block

    int status;
    int result = device_input(DISK_DEV, unit, & status);

    if (debug4)
        console("diskReadOrWriteReal: finished, status = %d, result = %d\n", status, result);

    return result;
}
/*-----------------------diskReadOrWriteReal-----------------------*/

/*-----------------------diskSize-----------------------*/
void diskSize(sysargs * args) {
    check_kernel_mode("diskSize()");
    int unit = (long) args -> arg1;
    int sector, track, disk;
    int retval = diskSizeReal(unit, & sector, & track, & disk);
    args -> arg1 = (void * )((long) sector);
    args -> arg2 = (void * )((long) track);
    args -> arg3 = (void * )((long) disk);
    args -> arg4 = (void * )((long) retval);
    set_user_mode();
}
/*-----------------------diskSize-----------------------*/

/*-----------------------diskSizeReal-----------------------*/
int diskSizeReal(int unit, int * sector, int * track, int * disk) {
    check_kernel_mode("diskSizeReal()");

    // validity check
    if (unit < 0 || unit > 1 || sector == NULL || track == NULL || disk == NULL) {

        return -1;
    }

    procPtr driver = & ProcTable[diskPids[unit]];

    // if first time get track number
    if (driver -> diskTrack == -1) {
        if (ProcTable[getpid() % MAXPROC].pid == -1) {
            initProc(getpid());
        }
        procPtr proc = & ProcTable[getpid() % MAXPROC];
        proc -> diskTrack = 0;
        device_request request;
        request.opr = DISK_TRACKS;
        request.reg1 = & driver -> diskTrack;
        proc -> diskRequest = request;

        addDiskQ( & diskQs[unit], proc); // add 
        semv_real(driver -> blockSem); // wake 
        semp_real(proc -> blockSem); // block

        if (debug4)
            console("diskSizeReal: number of units: tracks %d: %d\n", unit, driver -> diskTrack);
    }

    * sector = DISK_SECTOR_SIZE;
    * track = DISK_TRACK_SIZE;
    * disk = driver -> diskTrack;
    return 0;
}
/*-----------------------diskSizeReal-----------------------*/

/*-----------------------check_kernel_mode-----------------------*/
void check_kernel_mode(char * name) {
    if ((PSR_CURRENT_MODE & psr_get()) == 0) {
        console("%s: called while in user mode, by process %d. Halting...\n",
            name, getpid());
        halt(1);
    }
}
/*-----------------------check_kernel_mode-----------------------*/

/*-----------------------set user mode-----------------------*/
void set_user_mode() {
    psr_set(psr_get() & 14);

}
/*-----------------------set user mode-----------------------*/

/*-----------------------initProc-----------------------*/
void initProc(int pid) {
    check_kernel_mode("initProc()");

    int i = pid % MAXPROC;

    ProcTable[i].pid = pid;
    ProcTable[i].mboxID = MboxCreate(0, 0);
    ProcTable[i].blockSem = semcreate_real(0);
    ProcTable[i].wakeTime = -1;
    ProcTable[i].diskTrack = -1;
    ProcTable[i].nextDiskPtr = NULL;
    ProcTable[i].prevDiskPtr = NULL;
}
/*-----------------------initProc-----------------------*/

/*-----------------------emptyProc-----------------------*/
void emptyProc(int pid) {
    check_kernel_mode("emptyProc()");

    int i = pid % MAXPROC;

    ProcTable[i].pid = -1;
    ProcTable[i].mboxID = -1;
    ProcTable[i].blockSem = -1;
    ProcTable[i].wakeTime = -1;
    ProcTable[i].nextDiskPtr = NULL;
    ProcTable[i].prevDiskPtr = NULL;
}
/*-----------------------emptyProc-----------------------*/

/* ------------------------------------------------------------------------
  Functions for the dskQueue and heap.
   ----------------------------------------------------------------------- */

/*-----------------------initDiskQueue-----------------------*/
void initDiskQueue(diskQueue * q) {
    q -> head = NULL;
    q -> tail = NULL;
    q -> curr = NULL;
    q -> size = 0;
}
/*-----------------------initDiskQueue-----------------------*/

/* Adds the proc pointer to the disk queue in sorted order */
/*-----------------------addDiskQ-----------------------*/
void addDiskQ(diskQueue * q, procPtr p) {
    if (debug4)
        console("addDiskQ: adding pid %d, track %d to queue\n", p -> pid, p -> diskTrack);

    // first add
    if (q -> head == NULL) {
        q -> head = q -> tail = p;
        q -> head -> nextDiskPtr = q -> tail -> nextDiskPtr = NULL;
        q -> head -> prevDiskPtr = q -> tail -> prevDiskPtr = NULL;
    } else {
        // find the right location to add
        procPtr prev = q -> tail;
        procPtr next = q -> head;
        while (next != NULL && next -> diskTrack <= p -> diskTrack) {
            prev = next;
            next = next -> nextDiskPtr;
            if (next == q -> head)
                break;
        }
        if (debug4)
            console("addDiskQ: found place, prev = %d\n", prev -> diskTrack);
        prev -> nextDiskPtr = p;
        p -> prevDiskPtr = prev;
        if (next == NULL)
            next = q -> head;
        p -> nextDiskPtr = next;
        next -> prevDiskPtr = p;
        if (p -> diskTrack < q -> head -> diskTrack)
            q -> head = p; // update head
        if (p -> diskTrack >= q -> tail -> diskTrack)
            q -> tail = p; // update tail
    }
    q -> size++;
    if (debug4)
        console("addDiskQ: add complete, size = %d\n", q -> size);
}

/* Returns the next proc on the disk queue */
procPtr peekDiskQ(diskQueue * q) {
    if (q -> curr == NULL) {
        q -> curr = q -> head;
    }

    return q -> curr;
}
/*-----------------------addDiskQ-----------------------*/

/*-----------------------removeDiskQ-----------------------*/
procPtr removeDiskQ(diskQueue * q) {
    if (q -> size == 0)
        return NULL;

    if (q -> curr == NULL) {
        q -> curr = q -> head;
    }

    if (debug4)
        console("removeDiskQ: called, size = %d, curr pid = %d, curr track = %d\n", q -> size, q -> curr -> pid, q -> curr -> diskTrack);

    procPtr temp = q -> curr;

    if (q -> size == 1) { // remove only node
        q -> head = q -> tail = q -> curr = NULL;
    } else if (q -> curr == q -> head) { // remove head
        q -> head = q -> head -> nextDiskPtr;
        q -> head -> prevDiskPtr = q -> tail;
        q -> tail -> nextDiskPtr = q -> head;
        q -> curr = q -> head;
    } else if (q -> curr == q -> tail) { // remove tail
        q -> tail = q -> tail -> prevDiskPtr;
        q -> tail -> nextDiskPtr = q -> head;
        q -> head -> prevDiskPtr = q -> tail;
        q -> curr = q -> head;
    } else { // remove other
        q -> curr -> prevDiskPtr -> nextDiskPtr = q -> curr -> nextDiskPtr;
        q -> curr -> nextDiskPtr -> prevDiskPtr = q -> curr -> prevDiskPtr;
        q -> curr = q -> curr -> nextDiskPtr;
    }

    q -> size--;

    if (debug4)
        console("removeDiskQ: done, size = %d, curr pid = %d, curr track = %d\n", q -> size, temp -> pid, temp -> diskTrack);

    return temp;
}
/*-----------------------removeDiskQ-----------------------*/

/*-----------------------https://gist.github.com/aatishnn/8265656-----------------------*/
/*-----------------------initHeap-----------------------*/
void initHeap(heap * h) {
    h -> size = 0;
}

/* Add to heap */
void heapAdd(heap * h, procPtr p) {
    // start from bottom and find correct place
    int i, parent;
    for (i = h -> size; i > 0; i = parent) {
        parent = (i - 1) / 2;
        if (h -> procs[parent] -> wakeTime <= p -> wakeTime)
            break;
        // move parent down
        h -> procs[i] = h -> procs[parent];
    }
    h -> procs[i] = p; // put at final location
    h -> size++;
    if (debug4)
        console("heapAdd: Added proc %d to heap at index %d, size = %d\n", p -> pid, i, h -> size);
}
/*-----------------------initHeap-----------------------*/

/*-----------------------heapPeek-----------------------*/
procPtr heapPeek(heap * h) {
    return h -> procs[0];
}
/*-----------------------heapPeek-----------------------*/

/*-----------------------heapRemove-----------------------*/
procPtr heapRemove(heap * h) {
    if (h -> size == 0)
        return NULL;

    procPtr removed = h -> procs[0]; // remove min
    h -> size--;
    h -> procs[0] = h -> procs[h -> size]; // put last in first spot

    // re-heapify
    int i = 0, left, right, min = 0;
    while (i * 2 <= h -> size) {
        // get locations of children
        left = i * 2 + 1;
        right = i * 2 + 2;

        // get min child
        if (left <= h -> size && h -> procs[left] -> wakeTime < h -> procs[min] -> wakeTime)
            min = left;
        if (right <= h -> size && h -> procs[right] -> wakeTime < h -> procs[min] -> wakeTime)
            min = right;

        // swap current with min child if needed
        if (min != i) {
            procPtr temp = h -> procs[i];
            h -> procs[i] = h -> procs[min];
            h -> procs[min] = temp;
            i = min;
        } else
            break; // otherwise we're done
    }
    if (debug4)
        console("heapRemove: Called, returning pid %d, size = %d\n", removed -> pid, h -> size);
    return removed;
}
/*-----------------------heapRemove-----------------------*/

/*-----------------------getTime-----------------------*/
int getTime() {
    int result, unit = 0, status;

    result = device_input(CLOCK_DEV, unit, & status);

    if (result == DEV_INVALID) {
        console("clock device invalid.\n");
        halt(1);
    }

    return status;
}
/*-----------------------getTime-----------------------*/