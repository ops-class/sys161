#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "bus.h"
#include "console.h"
#include "speed.h"
#include "cpu.h"
#include "clock.h"
#include "util.h"

#include "busids.h"
#include "lamebus.h"


/***************************************************************/

/*
 * Timer has the following registers, mapped starting at offset 4096:
 *
 *    4 bytes:  Current time (seconds) 
 *    4 bytes:  Current time (nsec)
 *    4 bytes:  Restart-on-expiry flag
 *    4 bytes:  True if interrupt (reading clears flag)
 *    4 bytes:  Countdown time (usec) (writing starts timer)
 *    4 bytes:  Speaker (write any value to beep)
 *    4 bytes:  Reserved
 *    4 bytes:  Reserved
 */

#define TREG_TSEC  0x00
#define TREG_TNSEC 0x04
#define TREG_REST  0x08
#define TREG_IRQ   0x0c
#define TREG_TIME  0x10
#define TREG_BEEP  0x14
#define TREG_RESV1 0x18
#define TREG_RESV2 0x1c

struct timer_data {
	int td_slot;
	int td_restartflag;
	uint32_t td_count_usecs; /* for restarting */
	uint32_t td_generation;  /* for discarding old events */
};

static
void *
timer_init(int slot, int argc, char *argv[])
{
	struct timer_data *td = domalloc(sizeof(struct timer_data));
	td->td_slot = slot;
	td->td_restartflag = 0;
	td->td_count_usecs = 0;
	td->td_generation = 0;

	(void)argc;
	(void)argv;

	return td;
}

static void timer_start(struct timer_data *td);

static
void
timer_interrupt(void *d, uint32_t gen)
{
	struct timer_data *td = d;
	if (gen!=td->td_generation) {
		return;
	}

	raise_irq(td->td_slot);

	if (td->td_restartflag) {
		timer_start(td);
	}
}

static
void
timer_start(struct timer_data *td)
{
	uint64_t nsecs = td->td_count_usecs;
	nsecs *= 1000;
	td->td_generation++;
	schedule_event(nsecs, td, td->td_generation, timer_interrupt, "timer");
}

static
int
timer_fetch(unsigned cpunum, void *d, uint32_t offset, uint32_t *val)
{
	struct timer_data *td = d;

	(void)cpunum;

	switch (offset) {
	    case TREG_TSEC:
		clock_time(val, NULL);
		return 0;
	    case TREG_TNSEC:
		clock_time(NULL, val);
		return 0;
	    case TREG_REST:
		*val = td->td_restartflag;
		return 0;
	    case TREG_IRQ: 
		*val = check_irq(td->td_slot);
		lower_irq(td->td_slot);
		return 0;
	    case TREG_TIME:
		*val = td->td_count_usecs;
		return 0;
	    case TREG_BEEP:
	    case TREG_RESV1:
	    case TREG_RESV2:
		/*
		 * Mimic typical annoying property of real hardware
		 * when looked at the wrong way: do something useless
		 * and broken, like wedging the system. If we wanted
		 * to be evil, we could just have the timer break and,
		 * say, start throwing lots of wild interrupts.
		 */
		hang("Illegal timer register read");
		return 0;
	}
	return -1;
}

static
int
timer_store(unsigned cpunum, void *d, uint32_t offset, uint32_t val)
{
	struct timer_data *td = d;

	(void)cpunum;

	switch (offset) {
	    case TREG_TSEC:
		clock_setsecs(val);
		return 0;
	    case TREG_TNSEC:
		clock_setnsecs(val);
		return 0;
	    case TREG_REST:
		td->td_restartflag = val;
		return 0;
	    case TREG_TIME:
		td->td_count_usecs = val;
		timer_start(td);
		return 0;
	    case TREG_BEEP:
		console_beep();
		return 0;
	    case TREG_IRQ:
	    case TREG_RESV1:
	    case TREG_RESV2:
		/*
		 * Again, mimic real hardware.
		 */
		hang("Illegal timer register write");
		return 0;
	}
	return -1;
}

static
void
timer_dumpstate(void *data)
{
	struct timer_data *td = data;
	msg("System/161 timer device rev %d", TIMER_REVISION);
	msg("    %lu microseconds, %s",
	    (unsigned long) td->td_count_usecs,
	    td->td_restartflag ? "restarting" : "one-shot");
	msg("    Generation number: %lu",
	    (unsigned long) td->td_generation);
}

const struct lamebus_device_info timer_device_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_TIMER,
	TIMER_REVISION,
	timer_init,
	timer_fetch,
	timer_store,
	timer_dumpstate,
	NULL
};

