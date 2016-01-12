#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "console.h"
#include "speed.h"
#include "clock.h"
#include "cpu.h"
#include "bus.h"
#include "onsel.h"
#include "main.h"

/*
 * random() is a BSD function that is usually documented to return
 * values in the range 0 to 2^31-1, independent of RAND_MAX. It
 * appears, on some systems, that this is in fact the case and that
 * RAND_MAX must be ignored. So define RANDOM_MAX for our own purposes.
 * (Also see comment in dev_random.c.)
 *
 * Nowadays NetBSD defines RANDOM_MAX for us; maybe other systems will
 * sometime too.
 */

#ifndef RANDOM_MAX
#define RANDOM_MAX 0x7fffffffUL
#endif

////////////////////////////////////////////////////////////
// System clock state.

/*
 * We maintain two notions of time: physical time (time on the host
 * machine) and virtual time (time in the System/161 environment).
 * These are nominally synchronized, but in practice they diverge
 * because the CPU code doesn't run fast enough to keep up with its
 * nominal speed.
 *
 * Virtual time advances as follows:
 *    - when the main loop is stopped in the debugger: not at all
 *    - when the CPU is running: NSECS_PER_CLOCK per cpu clock
 *    - when the CPU is not running and a timed event is pending:
 *      instantly
 *    - when the CPU is not running and no timed events are pending:
 *      synchronously with physical time
 *
 * Physical time advances as follows:
 *    - when the main loop is stopped in the debugger: not at all (*)
 *    - otherwise: according to the host clock
 *
 * (*) not implemented yet
 *
 * We measure both kinds of time as a 64-bit nanoseconds counter where
 * startup is zero. The displayed time returned by the timer/clock
 * hardware includes a saved startup time used as an offset.
 */

#define NSECS_PER_SEC 1000000000ULL

static uint64_t virtual_now;

static uint32_t start_secs, start_nsecs;

unsigned progress;
static int check_progress;
static int progress_warned;
static uint64_t progress_timeout; /* nsecs */
static uint64_t progress_deadline; /* vtime */

////////////////////////////////////////////////////////////
// core clock logic

/*
 * Initialize.
 *
 * XXX: progress_deadline is set from command-line processing (if it's
 * going to be used) BEFORE we get here, so don't initialize it as
 * that clobbers it.
 */
static
void
clock_coreinit(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	start_secs = tv.tv_sec;
	/*
	 * Pretend we started at the beginning of the current second,
	 * rather than the actual start time. This means the time
	 * syncup code will make things run a little fast while it
	 * catches up (which doesn't matter) but has the important
	 * effect that the disk rotation model is anchored to a
	 * deterministic angular position at startup. At least, until
	 * the time handling for bus devices gets fixed to not expose
	 * the external physical time. (XXX)
	 */
	/* start_nsecs = 1000*tv.tv_usec; */
	start_nsecs = 0;

	virtual_now = 0;
}

/*
 * Return the current virtual time. Accounts for being partway through
 * a cpu_cycles() run.
 */
static
inline
uint64_t
clock_vnow(void)
{
	return virtual_now
		+ NSECS_PER_CLOCK * cpu_cycles_count
		+ extra_selecttime;
}

/*
 * Advance virtual time.
 */
static
inline
void
clock_vadvance(uint64_t nsecs)
{
	virtual_now += nsecs;
}

/*
 * Establish a new progress deadline.
 */
static
inline
void
clock_newprogressdeadline(void)
{
	progress_deadline = clock_vnow() + progress_timeout;
}

/*
 * Figure out how far a given virtual time is ahead of physical time.
 * Returns zero if it isn't ahead of physical time.
 *
 * If we're already ahead of physical time, limit the amount we report
 * to the amount the given virtual time is in the virtual future.
 */
static
uint64_t
clock_vahead(uint64_t vnow, uint64_t vnsecs)
{
	struct timeval tv;
	uint64_t pnsecs;

	/* get the physical time */
	gettimeofday(&tv, NULL);

	/* convert to time since startup */
	if (tv.tv_sec < start_secs) {
		/* just in case */
		return 0;
	}
	else {
		tv.tv_sec -= start_secs;
	}
	pnsecs = tv.tv_sec * NSECS_PER_SEC + tv.tv_usec * 1000;
	pnsecs -= start_nsecs;

	if (vnsecs <= pnsecs) {
		return 0;
	}
	if (vnow <= pnsecs) {
		return vnsecs - pnsecs;
	}
	return vnsecs - vnow;
}

////////////////////////////////////////////////////////////
// timer actions

struct timed_action {
	struct timed_action *ta_next;
	uint64_t ta_vtime;
	void *ta_data;
	uint32_t ta_code;
	void (*ta_func)(void *, uint32_t);
	const char *ta_desc;
	int ta_runningto;
};

/*
 * Pool of timer actions so we don't have to malloc.
 * Allow up to 16 simultaneous timed actions per device.
 */
#define MAXACTIONS 1024
static struct timed_action action_storage[MAXACTIONS];
static struct timed_action *ta_freelist = NULL;

static
struct timed_action *
acalloc(void)
{
	struct timed_action *ta;
	if (ta_freelist == NULL) {
		smoke("Too many pending hardware interrupts");
	}
	ta = ta_freelist;
	ta_freelist = ta->ta_next;
	return ta;
}

static
void
acfree(struct timed_action *ta)
{
	ta->ta_next = ta_freelist;
	ta_freelist = ta;
}

static
void
acalloc_init(void)
{
	int i;
	for (i=0; i<MAXACTIONS; i++) {
		acfree(&action_storage[i]);
	}
}

/*
 * The event queue.
 */
static struct timed_action *queuehead = NULL;

static
void
check_queue(void)
{
	struct timed_action *ta;
	uint64_t vnow;

	vnow = clock_vnow();
	while (queuehead != NULL) {
		ta = queuehead;
		if (ta->ta_vtime > vnow) {
			return;
		}

		queuehead = ta->ta_next;
		
		ta->ta_func(ta->ta_data, ta->ta_code);

		acfree(ta);
	}
}

/*
 * Figure out how many ticks we can run before the next scheduled
 * event.
 *
 * The return value needs to be rounded up; otherwise an event due
 * after the current moment but less than one cpu cycle in the future
 * never gets dispatched.
 */
uint32_t
clock_getrunticks(void)
{
	struct timed_action *ta;
	uint64_t vnow;
	uint32_t retnsecs;

/* Go for up to 5 ms at a time (in virtual time) */
#define MAXRUN 125000

	ta = queuehead;
	if (ta != NULL) {
		vnow = clock_vnow();
		if (ta->ta_vtime <= vnow) {
			return 0;
		}
		if (ta->ta_vtime < vnow + MAXRUN * NSECS_PER_CLOCK) {
			ta->ta_runningto = 1;
			/* round up */
			retnsecs = ta->ta_vtime - vnow;
			retnsecs += NSECS_PER_CLOCK - 1;
			return retnsecs / NSECS_PER_CLOCK;
		}
	}
	return MAXRUN;
}

void
schedule_event(uint64_t nsecs, void *data, uint32_t code,
	       void (*func)(void *, uint32_t),
	       const char *desc)
{
	struct timed_action *n, **p;

	nsecs += (uint64_t)((random()*(nsecs*0.01))/RANDOM_MAX);

	n = acalloc();
	n->ta_vtime = clock_vnow() + nsecs;
	n->ta_data = data;
	n->ta_code = code;
	n->ta_func = func;
	n->ta_desc = desc;
	n->ta_runningto = 0;

	/*
	 * Sorted linked-list insert.
	 */

	for (p = &queuehead; (*p) != NULL; p = &(*p)->ta_next) {
		if (n->ta_vtime < (*p)->ta_vtime) {
			break;
		}
	}

	n->ta_next = (*p);
	(*p) = n;

	/*
	 * If the event we're scheduling before is the next event and
	 * we told the cpu it can run until that time, stop the cpu.
	 * Then the main loop logic will recalculate things.
	 *
	 * XXX: it would be more efficient to tell the cpu when to stop,
	 * but currently that'd be difficult.
	 */
	if (n->ta_next != NULL && n->ta_next->ta_runningto) {
		cpu_stopcycling();
		n->ta_next->ta_runningto = 0;
	}
}

////////////////////////////////////////////////////////////
// elapsed time interface

/*
 * The cpu has been running, and ran for some number of cycles; update
 * the clock accordingly.
 */
void
clock_ticks(uint64_t nticks)
{
	uint64_t vnow;
	uint32_t secs;

	g_stats.s_tot_rcycles += nticks;
	clock_vadvance(nticks * NSECS_PER_CLOCK);
	check_queue();

	if (!check_progress) {
		return;
	}

	if (progress) {
		progress = 0;
		clock_newprogressdeadline();
		if (progress_warned) {
			progress_warned = 0;
		}
		return;
	}

	vnow = clock_vnow();
	if (vnow < progress_deadline) {
		return;
	}

	secs = progress_timeout / NSECS_PER_SEC;
	if (progress_warned) {
		msg("No progress in %lu seconds; dropping to debugger",
		    (unsigned long)secs * 2);
		main_enter_debugger(1 /* lethal */);

		/* avoid repeating */
		clock_newprogressdeadline();
		progress_warned = 0;
	}
	else {
		msg("Caution: no progress in %lu seconds",
		    (unsigned long)secs);
		clock_newprogressdeadline();
		progress_warned = 1;
	}
}

/*
 * The cpu is not running; wait until something happens.
 */
void
clock_waitirq(void)
{
	static uint32_t idleslop;

	uint64_t vnow, wnsecs, sleptnsecs, tmp;

	while (cpu_running_mask == 0) {
		if (queuehead != NULL) {
			/*
			 * We have an event due; wait for it. Figure
			 * out how far ahead of real wall time we will
			 * be when it's due to fire. If we aren't,
			 * don't sleep. If we are, sleep to sync up,
			 * as long as it's more than 10 ms. (Trying to
			 * sleep less than that is generally not
			 * useful.)
			 */
			vnow = clock_vnow();
			wnsecs = clock_vahead(vnow, queuehead->ta_vtime);

			if (wnsecs > 10000000) {
				/* Sleep. */
				sleptnsecs = tryselect(1, wnsecs);

				/* Clamp to wnsecs to avoid confusion. */
				if (sleptnsecs > wnsecs) {
					sleptnsecs = wnsecs;
				}
			}
			else {
				sleptnsecs = queuehead->ta_vtime - vnow;

				(void)tryselect(1, 0);
			}
		}
		else {
			/*
			 * No event due; wait for something to happen
			 * (network packet, keypress, etc.)
			 */
			sleptnsecs = tryselect(0, 0);
		}

		tmp = sleptnsecs + idleslop;
		sleptnsecs += idleslop;
		g_stats.s_tot_icycles += tmp / NSECS_PER_CLOCK;
		idleslop = tmp % NSECS_PER_CLOCK;

		clock_vadvance(sleptnsecs);
		check_queue();
	}
}

////////////////////////////////////////////////////////////
// auxiliary external clock interfaces

void
clock_time(uint32_t *secs_ret, uint32_t *nsecs_ret)
{
	uint64_t now;
	uint32_t secs, nsecs;

	now = clock_vnow();
	secs = start_secs + now / NSECS_PER_SEC;
	nsecs = start_nsecs + now % NSECS_PER_SEC;
	if (nsecs > NSECS_PER_SEC) {
		nsecs -= NSECS_PER_SEC;
		secs++;
	}
	if (secs_ret) {
		*secs_ret = secs;
	}
	if (nsecs_ret) {
		*nsecs_ret = nsecs;
	}
}

void
clock_setsecs(uint32_t newsecs)
{
	uint64_t now;
	uint32_t offset;
	uint32_t oldsecs;

	now = clock_vnow();
	oldsecs = start_secs + now / NSECS_PER_SEC;
	
	offset = newsecs - oldsecs;
	start_secs += offset;
}

void
clock_setnsecs(uint32_t newnsecs)
{
	uint64_t now;
	uint32_t offset;
	uint32_t oldnsecs;

	now = clock_vnow();
	oldnsecs = start_nsecs + now % NSECS_PER_SEC;
	
	offset = newnsecs - oldnsecs;
	start_nsecs += offset;
}

void
clock_dumpstate(void)
{
	uint64_t vnow;
	uint32_t cur_secs, cur_nsecs;
	struct timed_action *ta;

	vnow = clock_vnow();
	cur_secs = vnow / NSECS_PER_SEC;
	cur_nsecs = vnow % NSECS_PER_SEC;
	msg("clock: %lu.%09lu secs elapsed (start at %lu.%09lu)", 
	    (unsigned long) cur_secs,
	    (unsigned long) cur_nsecs,
	    (unsigned long) start_secs,
	    (unsigned long) start_nsecs);

	if (queuehead==NULL) {
		msg("clock: No events pending");
		return;
	}

	for (ta = queuehead; ta; ta = ta->ta_next) {
		msg("clock: at %12llu: %s",
		    (unsigned long long) ta->ta_vtime, ta->ta_desc);
	}
}

void
clock_setprogresstimeout(uint32_t secs)
{
	check_progress = 1;
	progress_timeout = secs * NSECS_PER_SEC;
	clock_newprogressdeadline();
}

void
clock_init(void)
{
	uint32_t offset;

	clock_coreinit();
	acalloc_init();

	/* Shift the clock ahead a random fraction of 10 ms. */
	offset = random() % 10000000;
	clock_vadvance(offset);
	check_queue();
}

void
clock_cleanup(void)
{
	uint64_t vnow;
	uint32_t secs, nsecs;

	vnow = clock_vnow();
	secs = vnow / NSECS_PER_SEC;
	nsecs = vnow % NSECS_PER_SEC;

	msg("Elapsed virtual time: %lu.%09lu seconds (%d mhz)", 
	    (unsigned long)secs, 
	    (unsigned long)nsecs,
	    1000/NSECS_PER_CLOCK);
}
