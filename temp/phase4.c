
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

static int running; /*semaphore to synchronize drivers and start3*/

static struct driver_proc Driver_Table[MAXPROC];

static int diskpids[DISK_UNITS];

static int	ClockDriver(char *);
static int	DiskDriver(char *);

void diskRead(sysargs *);
void diskWrite(sysargs *);
void diskSize(sysargs *);
void termRead(sysargs *);
void termWrite(sysargs *);

void check_kernel_mode(char * proc);
void sleep(sysargs * args);

int debugflag4 = 0;

int
start3(char *arg)
{
    char	name[128];
    char        termbuf[10];
    int		i;
    int		clockPID;
    int		pid;
    int		status;
    /*
     * Check kernel mode here.
     */
    check_kernel_mode("start3()\n");
    /* Assignment system call handlers */
    sys_vec[SYS_SLEEP]     = sleep;
    sys_vec[SYS_DISKREAD] = diskRead;
    sys_vec[SYS_DISKWRITE] = diskWrite;
    sys_vec[SYS_DISKSIZE] = diskSize;
    sys_vec[SYS_TERMREAD] = termRead;
    sys_vec[SYS_TERMWRITE] = termWrite;
    //more for this phase's system call handlings


    /* Initialize the phase 4 process table */


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

    for (i = 0; i < DISK_UNITS; i++) {
        sprintf(termbuf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        diskpids[i] = fork1(name, DiskDriver, termbuf, USLOSS_MIN_STACK, 2);
        if (diskpids[i] < 0) {
           console("start3(): Can't create disk driver %d\n", i);
           halt(1);
        }
    }
    semp_real(running);
    semp_real(running);


    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * I'm assuming kernel-mode versions of the system calls
     * with lower-case names.
     */
    pid = spawn_real("start4", start4, NULL,  8 * USLOSS_MIN_STACK, 3);
    pid = wait_real(&status);

    /*
     * Zap the device drivers
     */
    zap(clockPID);  // clock driver
    join(&status); /* for the Clock Driver */
}

static int
ClockDriver(char *arg)
{
    int result;
    int status;

    /*
     * Let the parent know we are running and enable interrupts.
     */
    semv_real(running);
    psr_set(psr_get() | PSR_CURRENT_INT);
    while(! is_zapped()) {
	result = waitdevice(CLOCK_DEV, 0, &status);
	if (result != 0) {
	    return 0;
	}
	/*
	 * Compute the current time and wake up any processes
	 * whose time has come.
	 */
    }
}

static int
DiskDriver(char *arg)
{
   int unit = atoi(arg);
   device_request my_request;
   int result;
   int status;
   

   driver_proc_ptr current_req;

   if (debugflag4)
      console("DiskDriver(%d): started\n", unit);


   /* Get the number of tracks for this disk */
   my_request.opr  = DISK_TRACKS;
   my_request.reg1 = &current_req[unit];

   result = device_output(DISK_DEV, unit, &my_request);

   if (result != DEV_OK) {
      console("DiskDriver %d: did not get DEV_OK on DISK_TRACKS call\n", unit);
      console("DiskDriver %d: is the file disk%d present???\n", unit, unit);
      halt(1);
   }

   waitdevice(DISK_DEV, unit, &status);
   if (debugflag4)
      console("DiskDriver(%d): tracks = %d\n", unit, current_req[unit]);


   //more code 
    return 0;
}


void check_kernel_mode(char * proc) {
    if ((PSR_CURRENT_MODE & psr_get()) == 0) {
        console("check_kernel_mode(): called while in user mode by process %s. Halting...\n", proc);
        halt(1);
    }
}

/* sleep function value extraction */
void sleep(sysargs * args) {
    checkForKernelMode("sleep()");
    int seconds = (long) args->arg1;
	
    int retval = sleepReal(seconds);
	
    args->arg4 = (void *) ((long) retval);
    setUserMode();
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
