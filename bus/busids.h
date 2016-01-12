/*
 * Vendor IDs of lamebus devices.
 */
#define LBVEND_SYS161         1

/*
 * System/161-vendor devices.
 */
#define LBVEND_SYS161_OLDMAINBOARD 1 /* Uniprocessor bus controller */
#define LBVEND_SYS161_TIMER   2     /* Timer/clock card */
#define LBVEND_SYS161_DISK    3     /* Fixed disk */
#define LBVEND_SYS161_SERIAL  4     /* Serial port */
#define LBVEND_SYS161_SCREEN  5     /* Memory-mapped screen */
#define LBVEND_SYS161_NET     6     /* Network interface */
#define LBVEND_SYS161_EMUFS   7     /* Emulator passthrough filesystem */
#define LBVEND_SYS161_TRACE   8     /* Hardware trace controller */
#define LBVEND_SYS161_RANDOM  9     /* Random number generator */
#define LBVEND_SYS161_MAINBOARD 10  /* Multiprocessor bus controller */

/*
 * Versions for System/161-vendor devices.
 */
#define OLDMAINBOARD_REVISION    2
#define MAINBOARD_REVISION       1

#define TIMER_REVISION     1
#define DISK_REVISION      2
#define SERIAL_REVISION    1
#define SCREEN_REVISION    1
#define NET_REVISION       1
#define EMUFS_REVISION     1
#define TRACE_REVISION     3
#define RANDOM_REVISION    1
