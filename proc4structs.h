/* Queue struct for processes */
typedef struct procStruct procStruct;
typedef struct procStruct * procPtr;
typedef struct diskQueue diskQueue;

// #define BLOCKED 0
// #define CHILDREN 1
// #define SLEEP 2

struct diskQueue {
	procPtr  head;
	procPtr  tail;
	procPtr  curr;
	int 	 size;
	int 	 type; /* which procPtr to use for next */
};

/* Heap */
typedef struct heap heap;
struct heap {
  int size;
  procPtr procs[MAXPROC];
};

/* 
* Process struct for phase 4
*/
struct procStruct {
  int         pid;
  int 		  mboxID; 
  int         blockSem;
  int		  wakeTime;
  int 		  diskTrack;
  int 		  diskFirstSec;
  int 		  diskSectors;
  void 		  *diskBuffer;
  procPtr 	  prevDiskPtr;
  procPtr 	  nextDiskPtr;
  device_request diskRequest;
};

