#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "config.h"

#include "speed.h"
#include "console.h"
#include "clock.h"
#include "main.h"
#include "util.h"

#include "busids.h"
#include "lamebus.h"


#define SERREG_CHAR   0x0
#define SERREG_WIRQ   0x4
#define SERREG_RIRQ   0x8

#define SCNREG_POSN   0x0
#define SCNREG_SIZE   0x4
#define SCNREG_CHAR   0x8
#define SCNREG_RIRQ   0xc

#define IRQF_ON    0x1
#define IRQF_READY 0x2
#define IRQF_FORCE 0x4

#define INBUF_SIZE 512

//////////////////////////////////////////////////

struct serirq {
	int si_on;
	int si_ready;
	int si_force;
};

struct ser_data {
	int sd_slot;
	int sd_wbusy;
	int sd_rbusy;
	struct serirq sd_rirq;
	struct serirq sd_wirq;

	uint32_t sd_readch;
       int sd_didread;
	char sd_inbuf[INBUF_SIZE];
	int sd_inbufhead;	/* characters are read from inbufhead */
	int sd_inbuftail;	/* characters are written to inbuftail */
};

static
uint32_t
fetchirq(struct serirq *si)
{
	uint32_t val = 0;
	if (si->si_on) val |= IRQF_ON;
	if (si->si_ready) val |= IRQF_READY;
	if (si->si_force) val |= IRQF_FORCE;
	return val;
}

static
void
storeirq(struct serirq *si, uint32_t val)
{
	si->si_on = (val & IRQF_ON)!=0;
	si->si_ready = (val & IRQF_READY)!=0;
	si->si_force = (val & IRQF_FORCE)!=0;
}

static
void
setirq(struct ser_data *sd)
{
	int rirq = sd->sd_rirq.si_on &&
		(sd->sd_rirq.si_ready || sd->sd_rirq.si_force);
	int wirq = sd->sd_wirq.si_on &&
		(sd->sd_wirq.si_ready || sd->sd_wirq.si_force);
	if (rirq || wirq) {
		raise_irq(sd->sd_slot);
	}
	else {
		lower_irq(sd->sd_slot);
	}
}

static
void
serial_writedone(void *d, uint32_t gen)
{
	struct ser_data *sd = d;
	(void)gen;

	sd->sd_wbusy = 0;
	sd->sd_wirq.si_ready = 1;
	setirq(sd);
}

static
void
serial_pushinput(void *d, uint32_t junk)
{
	struct ser_data *sd = d;
	uint32_t ch;

	(void)junk;

	if (sd->sd_inbufhead==sd->sd_inbuftail) {
		sd->sd_rbusy = 0;
	}
       else if (!sd->sd_didread) {
               msg("Input character dropped");
               schedule_event(SERIAL_NSECS, sd, 0,
                              serial_pushinput, "serial read");
       }
	else {
		ch = (uint32_t)(unsigned char)sd->sd_inbuf[sd->sd_inbufhead];
		sd->sd_inbufhead = (sd->sd_inbufhead+1)%INBUF_SIZE;

		sd->sd_readch = ch;
               sd->sd_didread = 0;
		sd->sd_rirq.si_ready = 1;
		setirq(sd);

		sd->sd_rbusy = 1;
		schedule_event(SERIAL_NSECS, sd, 0,
			       serial_pushinput, "serial read");
	}
}

static
void
serial_input(void *d, int ch)
{
	struct ser_data *sd = d;
	int nexttail;
	static int overrun_in_progress=0;

	nexttail = (sd->sd_inbuftail + 1)%INBUF_SIZE;
	if (nexttail==sd->sd_inbufhead) {
		if (!overrun_in_progress) {
			msg("Input buffer overrun");
			overrun_in_progress=1;
		}
		return;
	}
	overrun_in_progress = 0;

	sd->sd_inbuf[sd->sd_inbuftail] = ch;
	sd->sd_inbuftail = nexttail;

	if (!sd->sd_rbusy) {
		serial_pushinput(sd, 0);
	}
}

static
int
serial_fetch(unsigned cpunum, void *d, uint32_t offset, uint32_t *val)
{
	struct ser_data *sd = d;

	(void)cpunum;

	switch (offset) {
	    case SERREG_CHAR: 
		*val = sd->sd_readch;
               sd->sd_didread = 1;
		g_stats.s_rchars++; 
		return 0;
	    case SERREG_RIRQ:
		*val = fetchirq(&sd->sd_rirq);
		return 0;
	    case SERREG_WIRQ:
		*val = fetchirq(&sd->sd_wirq);
		return 0;
	}
	return -1;
}

static
int
serial_store(unsigned cpunum, void *d, uint32_t offset, uint32_t val)
{
	struct ser_data *sd = d;

	(void)cpunum;

	switch (offset) {
	    case SERREG_CHAR: 
		    if (!sd->sd_wbusy) {
			    sd->sd_wbusy = 1;
			    g_stats.s_wchars++;
			    console_putc(val);
			    schedule_event(SERIAL_NSECS, sd, 0, 
					   serial_writedone, "serial write");
		    }
		    return 0;
	    case SERREG_RIRQ: 
		    storeirq(&sd->sd_rirq, val);
		    setirq(sd);
		    return 0;
	    case SERREG_WIRQ:
		    storeirq(&sd->sd_wirq, val);
		    setirq(sd);
		    return 0;
	}
	return -1;
}

static
void *
serial_init(int slot, int argc, char *argv[])
{
	struct ser_data *sd = domalloc(sizeof(struct ser_data));
	sd->sd_slot = slot;
	sd->sd_wbusy = 0;
	sd->sd_rbusy = 0;
	sd->sd_rirq.si_on = 0;
	sd->sd_rirq.si_ready = 0;
	sd->sd_rirq.si_force = 0;
	sd->sd_wirq.si_on = 0;
	sd->sd_wirq.si_ready = 0;
	sd->sd_wirq.si_force = 0;

	sd->sd_readch = 0;
       sd->sd_didread = 1;
	sd->sd_inbufhead = 0;	/* empty if head==tail */
	sd->sd_inbuftail = 0;

	(void)argc;
	(void)argv;

	console_onkey(sd, serial_input);

	return sd;
}

static
void
serial_dumpstate(void *data)
{
	struct ser_data *sd = data;
	char c[2];

	msg("System/161 serial port rev %d", SERIAL_REVISION);
	c[0] = sd->sd_readch;
	c[1] = 0;
       msg("    Last character typed: %s (%ld), which was %sread",
	    isprint((int)c[0]) ? c : "(?)",
           (unsigned long) sd->sd_readch,
           sd->sd_didread ? "" : "not ");
	msg("    Read interrupts %s%s%s", 
	    sd->sd_rirq.si_on ? "active" : "inactive",
	    sd->sd_rirq.si_ready ? " (asserted)" : "",
	    sd->sd_rirq.si_force ? " (forced)" : "");
	if (sd->sd_wbusy) {
		msg("    Write in progress");
	}
	else {
		msg("    Ready for writing");
	}
	msg("    Write interrupts %s%s%s", 
	    sd->sd_wirq.si_on ? "active" : "inactive",
	    sd->sd_wirq.si_ready ? " (asserted)" : "",
	    sd->sd_wirq.si_force ? " (forced)" : "");
}

const struct lamebus_device_info serial_device_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_SERIAL,
	SERIAL_REVISION,
	serial_init,
	serial_fetch,
	serial_store,
	serial_dumpstate,
	NULL
};
