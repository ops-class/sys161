/*
 * Exit codes.
 */

#define SYS161_EXIT_NORMAL	0	/* Poweroff. */
#define SYS161_EXIT_CRASH	1	/* Timeout or software failure. */
#define SYS161_EXIT_ERROR	2	/* Config/user/runtime errors. */
#define SYS161_EXIT_REQUESTED	3	/* Requested (doom counter/debugger) */
