#include <sys/types.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

#include "util.h"
#include "bus.h"
#include "cpu.h"
#include "speed.h"
#include "console.h"
#include "gdb.h"
#include "onsel.h"
#include "clock.h"
#include "main.h"
#include "memdefs.h"

#include "lamebus.h"
#include "busids.h"
#include "busdefs.h"


/*
 * Maximum amount of physical memory we allow.
 * For now, 16M.
 */

#define MAXMEM (16*1024*1024)

/*
 * Memory.
 */

uint32_t bus_ramsize;					/* RAMSZ */
char *ram;

/*
 * Interrupts.
 */

static uint32_t bus_raised_interrupts;			/* IRQS */
static uint32_t bus_enabled_interrupts = 0xffffffff;	/* IRQE */

/*
 * CPUs.
 */

struct lamebus_cpu {
	int cpu_enabled;				/* one bit of CPUE */
	uint32_t cpu_enabled_interrupts;		/* CIRQE */
	int cpu_interrupting;
	int cpu_ipi;					/* CIPI, 0 or 1 */
	/* This used to be cpu_cram[LAMEBUS_CRAM_SIZE] see dev_disk.c */
	char *cpu_cram;					/* CRAM */
};

static struct lamebus_cpu cpus[LAMEBUS_NCPUS];
static unsigned ncpus;

/*
 * Slots.
 */

struct lamebus_slot {
   void                             *ls_devdata;
   const struct lamebus_device_info *ls_info;
};

static struct lamebus_slot devices[LAMEBUS_NSLOTS];

/***************************************************************/
/* Register offsets */

/* LAMEbus config registers (offsets into a config region) */
#define LBC_CONFIG_VENDORID	0x0
#define LBC_CONFIG_DEVICEID	0x4
#define LBC_CONFIG_REVISION	0x8

/* LAMEbus controller registers (offsets into bus controller config region) */
#define LBC_CTL_RAMSIZE		0x200
#define LBC_CTL_IRQS		0x204
#define LBC_CTL_POWER		0x208
#define LBC_CTL_IRQE		0x20c
#define LBC_CTL_CPUS		0x210
#define LBC_CTL_CPUE		0x214
#define LBC_CTL_SELF		0x218

/* LAMEbus per-cpu control registers (offsets into a cpu region) */
#define LBC_CPU_CIRQE		0x0
#define LBC_CPU_CIPI		0x4

/* LAMEbus per-cpu scratch area (offsets into a cpu region) */
#define LBC_CRAM_START		0x300
#define LBC_CRAM_END		0x400

/***************************************************************/
/* Bus dispatcher */

/*
 * Fetch device register.
 */
int
bus_io_fetch(unsigned cpunum, uint32_t offset, uint32_t *ret)
{
	uint32_t slot = offset / LAMEBUS_SLOT_MEM;
	uint32_t slotoffset = offset % LAMEBUS_SLOT_MEM;

	if (slot >= LAMEBUS_NSLOTS) {
		/* Out of range */
		return -1;
	}

	Assert((offset & 0x3)==0);

	if (devices[slot].ls_info==NULL) {
		return -1;
	}

	return devices[slot].ls_info->ldi_fetch(cpunum,
						devices[slot].ls_devdata,
						slotoffset, ret);
}

/*
 * Store to device registers.
 */
int
bus_io_store(unsigned cpunum, uint32_t offset, uint32_t val)
{
	uint32_t slot = offset / LAMEBUS_SLOT_MEM;
	uint32_t slotoffset = offset % LAMEBUS_SLOT_MEM;

	if (slot >= LAMEBUS_NSLOTS) {
		/* Out of range */
		return -1;
	}

	Assert((offset & 0x3)==0);

	if (devices[slot].ls_info==NULL) {
		return -1;
	}

	return devices[slot].ls_info->ldi_store(cpunum,
						devices[slot].ls_devdata, 
						slotoffset, val);
}

/***************************************************************/
/* IRQ dispatcher */

static
inline
void
irqupdate(void)
{
	struct lamebus_cpu *cpu;
	uint32_t mask;
	unsigned i;
	int irq;

	mask = bus_raised_interrupts & bus_enabled_interrupts;

	for (i=0; i<ncpus; i++) {
		cpu = &cpus[i];

		/* always 1/0, not a random bit, so the equality test works */
		if (mask & cpu->cpu_enabled_interrupts) {
			irq = 1;
		}
		else {
			irq = 0;
		}

		if (irq != cpu->cpu_interrupting) {
			cpu->cpu_interrupting = irq;
			cpu_set_irqs(i, cpu->cpu_interrupting, cpu->cpu_ipi);
		}
	}
}

void
raise_irq(int slot)
{
	bus_raised_interrupts |= ((uint32_t)1 << slot);
	irqupdate();
	HWTRACE(DOTRACE_IRQ, "Slot %2d: irq ON", (slot));
}

void
lower_irq(int slot)
{
	bus_raised_interrupts &= ~((uint32_t)1 << slot);
	irqupdate();
	HWTRACE(DOTRACE_IRQ, "Slot %2d: irq OFF", (slot));
}

int
check_irq(int slot)
{
	return (bus_raised_interrupts & ((uint32_t)1 << slot)) != 0;
}

/***************************************************************/
/* Operational functions */

static
void
dopoweroff(void *junk1, uint32_t junk2)
{
	(void)junk1;
	(void)junk2;

	/*
	 * This is never seen by the processor, but it breaks the clock
	 * module out of the idle loop.
	 *
	 * XXX this means if you mask slot 31 in IRQE or CIRQE, the
	 * system won't actually power off. This is ok as a hardware
	 * bug for the moment but it would be nice to not have such
	 * behavior.
	 */
	raise_irq(LAMEBUS_CONTROLLER_SLOT);

	main_poweroff();
}

static
uint32_t
get_cpue(void)
{
	unsigned i;
	uint32_t ret = 0;

	for (i=0; i<ncpus; i++) {
		if (cpus[i].cpu_enabled) {
			ret |= (uint32_t)1 << i;
		}
	}
	return ret;
}

static
void
set_cpue(uint32_t val)
{
	unsigned i, thisbit;
	struct lamebus_cpu *cpu;

	for (i=0; i<ncpus; i++) {
		thisbit = val & ((uint32_t)1 << i);
		cpu = &cpus[i];

		if (cpu->cpu_enabled && thisbit == 0) {
			/*
			 * Shutting off cpu.
			 *
			 * Just drop it in its tracks...
			 */
			cpu->cpu_enabled = 0;
			cpu_disable(i);
		}
		else if (!cpu->cpu_enabled && thisbit != 0) {
			/*
			 * Turning on cpu.
			 *
			 * According to spec, set stack to the top end
			 * of CRAM, and load the PC from the bottom
			 * end of CRAM.
			 */
			uint32_t cramoffset;
			uint32_t stackva, pcva, arg;
			uint32_t *cram;

			cramoffset = LAMEBUS_SLOT_MEM * LAMEBUS_CONTROLLER_SLOT
				+ 32768
				+ i * LAMEBUS_PERCPU_SIZE
				+ LBC_CRAM_END;

			stackva = cpu_get_secondary_start_stack(cramoffset);
			cram = (uint32_t *)cpu->cpu_cram;
			pcva = ntohl(cram[0]);
			arg = ntohl(cram[1]);

			cpu_set_entrypoint(i, pcva);
			cpu_set_stack(i, stackva, arg);

			cpu_enable(i);
		}
	}
}

/***************************************************************/
/* Register addressing */

/*
 * Translate an offset into a 32K region into an offset and number into
 * 32 1K regions. Works for both CPU regions and config regions.
 */
#if LAMEBUS_CONFIG_SIZE != LAMEBUS_PERCPU_SIZE
#error bah (1)
#endif
#if LAMEBUS_NSLOTS != LAMEBUS_NCPUS
#error bah (2)
#endif

static
inline
void
lamebus_controller_region(uint32_t offset,
			  uint32_t *region_ret, uint32_t *regionoffset_ret)
{
	uint32_t region, regionoffset;

	region = offset / LAMEBUS_CONFIG_SIZE;
	regionoffset = offset % LAMEBUS_CONFIG_SIZE;

	Assert(region < LAMEBUS_NSLOTS);

	*region_ret = region;
	*regionoffset_ret = regionoffset;
}

static
int
lamebus_controller_fetch_cpu(uint32_t offset, uint32_t *ret)
{
	uint32_t region;
	uint32_t *ptr;
	struct lamebus_cpu *cpu;

	lamebus_controller_region(offset, &region, &offset);
	if (region >= ncpus) {
		return -1;
	}
	cpu = &cpus[region];

	if (offset >= LBC_CRAM_START && offset < LBC_CRAM_END) {
		offset -= LBC_CRAM_START;
		ptr = (uint32_t *)(cpu->cpu_cram + offset);
		*ret = ntohl(*ptr);
		return 0;
	}

	switch (offset) {
	    case LBC_CPU_CIRQE:
		*ret = cpu->cpu_enabled_interrupts;
		return 0;
	    case LBC_CPU_CIPI:
		*ret = cpu->cpu_ipi ? 0xffffffff : 0;
		return 0;
	}

	return -1;
}

static
int
lamebus_controller_store_cpu(uint32_t offset, uint32_t val)
{
	uint32_t region;
	uint32_t *ptr;
	struct lamebus_cpu *cpu;

	lamebus_controller_region(offset, &region, &offset);
	if (region >= ncpus) {
		return -1;
	}
	cpu = &cpus[region];

	if (offset >= LBC_CRAM_START && offset < LBC_CRAM_END) {
		offset -= LBC_CRAM_START;
		ptr = (uint32_t *)(cpu->cpu_cram + offset);
		*ptr = htonl(val);
		return 0;
	}

	switch (offset) {
	    case LBC_CPU_CIRQE:
		cpu->cpu_enabled_interrupts = val;
		irqupdate();
		return 0;
	    case LBC_CPU_CIPI:
		cpu->cpu_ipi = val ? 1 : 0;
		cpu_set_irqs(region, cpu->cpu_interrupting, cpu->cpu_ipi);
		return 0;
	}

	return -1;
}

static
int
lamebus_controller_fetch_config(unsigned cpunum,
				int isold, uint32_t offset, uint32_t *ret)
{
	const struct lamebus_device_info *inf;
	uint32_t region;

	lamebus_controller_region(offset, &region, &offset);
	inf = devices[region].ls_info;

	switch (offset) {
	    case LBC_CONFIG_VENDORID:
		*ret = inf ? inf->ldi_vendorid : 0;
		return 0;
	    case LBC_CONFIG_DEVICEID:
		*ret = inf ? inf->ldi_deviceid : 0;
		return 0;
	    case LBC_CONFIG_REVISION:
		*ret = inf ? inf->ldi_revision : 0;
		return 0;
	}

	if (region != LAMEBUS_CONTROLLER_SLOT) {
		return -1;
	}

	switch (offset) {
	    case LBC_CTL_RAMSIZE:
		*ret = bus_ramsize;
		return 0;
	    case LBC_CTL_IRQS:
		*ret = bus_raised_interrupts;
		return 0;
	    case LBC_CTL_POWER:
		if (isold) {
			hang("Read from LAMEbus controller power register");
			*ret = 0;
		}
		else {
			*ret = 0xffffffff;
		}
		return 0;
	    case LBC_CTL_IRQE:
		*ret = bus_enabled_interrupts;
		return 0;
	    case LBC_CTL_CPUS:
		if (isold) {
			return -1;
		}
		if (ncpus == 32) {
			/* avoid nasal demons */
			*ret = 0xffffffff;
		}
		else {
			*ret = ((uint32_t)1 << ncpus) - 1;
		}
		return 0;
	    case LBC_CTL_CPUE:
		if (isold) {
			return -1;
		}
		*ret = get_cpue();
		return 0;
	    case LBC_CTL_SELF:
		if (isold) {
			return -1;
		}
		*ret = (uint32_t)1 << cpunum;
		return 0;
	}

	return -1;
}


static
int
lamebus_controller_store_config(int isold, uint32_t offset, uint32_t val)
{
	uint32_t region;

	lamebus_controller_region(offset, &region, &offset);
	if (region != LAMEBUS_CONTROLLER_SLOT) {
		return -1;
	}

	switch (offset) {
	    case LBC_CTL_POWER:
		if (val == 0) {
			schedule_event(POWEROFF_NSECS, NULL, 0, dopoweroff,
				       "poweroff");
		}
		else if (!isold) {
			if ((val & 0x80000000) == 0) {
				/* switched off mainboard, left others on */
				hang("Invalid power state");
			}
		}
		return 0;
	    case LBC_CTL_IRQE:
		bus_enabled_interrupts = val;
		irqupdate();
		return 0;
	    case LBC_CTL_CPUE:
		if (isold) {
			return -1;
		}
		set_cpue(val);
		return 0;
	    default:
		break;
	}

	return -1;
}

static
int
lamebus_controller_fetch(unsigned cpunum,
			 void *data, uint32_t offset, uint32_t *ret)
{
	int isold = (data != NULL);

	if (offset >= 32768) {
		if (isold) {
			return -1;
		}
		return lamebus_controller_fetch_cpu(offset-32768, ret);
	}
	else {
		return lamebus_controller_fetch_config(cpunum,
						       isold, offset, ret);
	}
}

static
int
lamebus_controller_store(unsigned cpunum,
			 void *data, uint32_t offset, uint32_t val)
{
	int isold = (data != NULL);

	(void)cpunum;

	if (offset >= 32768) {
		if (isold) {
			return -1;
		}
		return lamebus_controller_store_cpu(offset-32768, val);
	}
	else {
		return lamebus_controller_store_config(isold, offset, val);
	}
}

/***************************************************************/

static
void
lamebus_commonmainboard_init(int isold, int slot, int argc, char *argv[])
{
	int i;
	unsigned long j, tmp_ncpus, ncores;
	const char *myname = isold ? "oldmainboard" : "mainboard";

	Assert(slot==LAMEBUS_CONTROLLER_SLOT);

	/*
	 * Defaults
	 */
	bus_ramsize = 0; /* for now require configuration */
	tmp_ncpus = 1;
	ncores = 1;

	for (i=1; i<argc; i++) {
		if (!strncmp(argv[i], "ramsize=", 8)) {
			bus_ramsize = getsize(argv[i]+8);
		}
		else if (!isold && !strncmp(argv[i], "cpus=", 5)) {
			tmp_ncpus = strtoul(argv[i]+5, NULL, 0);
		}
		else if (!isold && !strncmp(argv[i], "cores=", 6)) {
			ncores = strtoul(argv[i]+6, NULL, 0);
		}
		else {
			msg("%s: invalid option `%s'", myname, argv[i]);
			die();
		}
	}

	if (tmp_ncpus == 0 || ncores == 0) {
		msg("%s: give me no CPUs and I'll give you no lies", myname);
		die();
	}
	if (ncores > 1) {
		msg("%s: no support for multicore CPUs yet", myname);
		die();
	}
	if (tmp_ncpus > 32) {
		msg("%s: too many CPUs", myname);
		die();
	}
	/* avoid overflow from unsigned long to unsigned */
	ncpus = tmp_ncpus;

	for (j=0; j<ncpus; j++) {
		cpus[j].cpu_enabled = 0;
		cpus[j].cpu_enabled_interrupts = 0xffffffff;
		cpus[j].cpu_ipi = 0;
		cpus[j].cpu_interrupting = 0;
		cpus[j].cpu_cram = domalloc(LAMEBUS_CRAM_SIZE);
	}
	cpus[0].cpu_enabled = 1;
}

static
void *
lamebus_oldmainboard_init(int slot, int argc, char *argv[])
{
	lamebus_commonmainboard_init(1 /*old*/, slot, argc, argv);

	/* not NULL to mark this as the old controller type, kinda gross */
	return &cpus[0];
}

static
void *
lamebus_mainboard_init(int slot, int argc, char *argv[])
{
	lamebus_commonmainboard_init(0 /*not old*/, slot, argc, argv);
	return NULL;
}

static
void
lamebus_commonmainboard_cleanup(void)
{
	unsigned j;

	for (j=0; j<ncpus; j++) {
		free(cpus[j].cpu_cram);
	}
}

static
void
lamebus_oldmainboard_cleanup(void *data)
{
	(void)data;
	lamebus_commonmainboard_cleanup();
}

static
void
lamebus_mainboard_cleanup(void *data)
{
	(void)data;
	lamebus_commonmainboard_cleanup();
}

static
void
lamebus_oldmainboard_dumpstate(void *data)
{
	(void)data;
	msg("LAMEbus uniprocessor controller rev %d", OLDMAINBOARD_REVISION);
	msg("    ramsize: %lu (%luk)", 
	    (unsigned long)bus_ramsize, 
	    (unsigned long)bus_ramsize/1024);
	msg("    irqs: 0x%08x", bus_raised_interrupts);
	msg("    irqe: 0x%08x", bus_enabled_interrupts);
	msg("    irqc: 0x%08x", cpus[0].cpu_interrupting);
}

static
void
lamebus_mainboard_dumpstate(void *data)
{
	unsigned i;

	(void)data;

	msg("LAMEbus multiprocessor controller rev %d", MAINBOARD_REVISION);
	msg("    ramsize: %lu (%luk)", 
	    (unsigned long)bus_ramsize, 
	    (unsigned long)bus_ramsize/1024);
	msg("    irqs: 0x%08x", bus_raised_interrupts);
	msg("    irqe: 0x%08x", bus_enabled_interrupts);
	msg("    cpus: %u", ncpus);
	msg("    cpue: 0x%08x", get_cpue());
	for (i=0; i<ncpus; i++) {
		msg("    cpu %d: %s", i,
		    cpus[i].cpu_enabled ? "ENABLED" : "DISABLED");
		msg("    cpu %d cirqe: 0x%08x", i,
		    cpus[i].cpu_enabled_interrupts);
		msg("    cpu %d cipi: %d", i, cpus[i].cpu_ipi);
		msg("    cpu %d interrupting: %d", i,
		    cpus[i].cpu_interrupting);
		msg("    cpu %d cram:", i);
		dohexdump(cpus[i].cpu_cram, LAMEBUS_CRAM_SIZE);
	}
}

static struct lamebus_device_info lamebus_oldmainboard_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_OLDMAINBOARD,
	OLDMAINBOARD_REVISION,
	lamebus_oldmainboard_init,
	lamebus_controller_fetch,
	lamebus_controller_store,
	lamebus_oldmainboard_dumpstate,
	lamebus_oldmainboard_cleanup,
};

static struct lamebus_device_info lamebus_mainboard_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_MAINBOARD,
	MAINBOARD_REVISION,
	lamebus_mainboard_init,
	lamebus_controller_fetch,
	lamebus_controller_store,
	lamebus_mainboard_dumpstate,
	lamebus_mainboard_cleanup,
};


/***************************************************************/

struct bus_device {
	const char *dev_name;
	const struct lamebus_device_info *dev_info;
	int dev_iscontroller;
};

static const struct bus_device devtable[] = {
	{ "busctl",             &lamebus_oldmainboard_info, 1 }, /* compat */
	{ "oldmainboard",       &lamebus_oldmainboard_info, 1 },
	{ "mainboard",          &lamebus_mainboard_info,    1 },
	{ "timer",              &timer_device_info,         0 }, 
	{ "disk",               &disk_device_info,          0 },
	{ "serial",             &serial_device_info,        0 },
	{ "screen",             &screen_device_info,        0 },
	{ "nic",                &net_device_info,           0 },
	{ "emufs",              &emufs_device_info,         0 },
	{ "trace",              &trace_device_info,         0 },
	{ "random",             &random_device_info,        0 },
	{ NULL, NULL, 0 }
};

static 
const struct bus_device *
find_dev(const char *name)
{
	int i;
	for (i=0; devtable[i].dev_name; i++) {
		if (!strcmp(devtable[i].dev_name, name)) {
			return &devtable[i];
		}
	}
	return NULL;
}

/*
 * Config file syntax is:
 *
 *     slot device-name args
 */
#define MAXARGS 128
unsigned
bus_config(const char *configfile,
	   const char *configextra[], unsigned numconfigextra)
{
	char *s;
	const struct bus_device *dev;
	char buf[1024], *argv[MAXARGS];
	int argc, slot, line=0;
	unsigned i;
	FILE *f;

	for (i=0; i<numconfigextra; i++) {
		s = strchr(configextra[i], ':');
		Assert(s);
		*s = 0;
		slot = atoi(configextra[i]);
		if (slot < 0 || slot >= LAMEBUS_NSLOTS) {
			*s = ':';
			msg("-C %s: Invalid slot number (0-31 allowed)",
			    configextra[i]);
			die();
		}
		*s = ':';
	}

	f = fopen(configfile, "r");
	if (!f) {
		msg("Cannot open config file %s", configfile);
		die();
	}

	while (fgets(buf, sizeof(buf),f)) {
		line++;
		s = strchr(buf, '#');
		if (s) *s = 0;
		argc=0;
		for (s=strtok(buf, " \t\r\n"); s; s=strtok(NULL, " \t\r\n")) {
			if (argc<MAXARGS) argv[argc++] = s;
		}
		if (argc>=MAXARGS) {
			msg("config %s: line %d: Too many args", 
			    configfile, line);
			die();
		}
		argv[argc] = NULL;
		if (argc==0) continue;

		slot = strtol(argv[0], &s, 0);
		if (strlen(s)>0 || slot<0 || slot>=LAMEBUS_NSLOTS) {
			msg("config %s: line %d: Invalid slot `%s' (should "
			    "be 0-%d)", 
			    configfile, line, argv[0], LAMEBUS_NSLOTS-1);
			die();
		}
      
		if (argc==1) {
			msg("config %s: line %d: slot %d: No device", 
			    configfile, line, slot);
			die();
		}

		if (devices[slot].ls_info!=NULL) {
			msg("config %s: line %d: slot %d: Already in use",
			    configfile, line, slot);
			die();
		}
		
		dev = find_dev(argv[1]);
		if (!dev) {
			msg("config %s: line %d: slot %d: No such "
			    "hardware `%s'", 
			    configfile, line, slot, argv[1]);
			die();
		}

		for (i=0; i<numconfigextra; i++) {
			if (configextra[i] == NULL) {
				continue;
			}
			s = strchr(configextra[i], ':');
			Assert(s != NULL);
			*s = 0;
			if (atoi(configextra[i]) == slot) {
				if (argc >= MAXARGS) {
					*s = ':';
					msg("-S %s: too many args for "
					    "this slot", configextra[i]);
					die();
				}
				argv[argc++] = s+1;
				configextra[i] = NULL;
			}
			else {
				*s = ':';
			}
		}
		
		{
			int isbus = dev->dev_iscontroller;
			int isbusslot = slot==LAMEBUS_CONTROLLER_SLOT;

			if ((isbus && !isbusslot) || (!isbus && isbusslot)) {
				msg("config %s: line %d: slot %d: "
				    "%s: Bus controller must "
				    "go in slot %d", 
				    configfile, line, slot, argv[1], 
				    LAMEBUS_CONTROLLER_SLOT);
				die();
			}
		}
		
		devices[slot].ls_info = dev->dev_info;
		devices[slot].ls_devdata = 
			dev->dev_info->ldi_init(slot, argc-1, argv+1);
	}
	
	fclose(f);

	for (i=0; i<numconfigextra; i++) {
		if (configextra[i] == NULL) {
			continue;
		}
		msg("-C %s: No device in that slot", configextra[i]);
		die();
	}
	
	if (bus_ramsize == 0) {
		msg("config %s: No system memory", configfile);
		die();
	}
	if (bus_ramsize & 0xfff) {
		msg("config %s: System memory size not page-aligned", 
		    configfile);
		die();
	}
	if (bus_ramsize > MAXMEM) {
		msg("config %s: System memory too large", configfile);
		die();
	}
	
	ram = calloc(bus_ramsize, 1);
	if (!ram) {
		msg("config %s: Cannot allocate system memory", configfile);
		die();
	}

	return ncpus;
}

void
bus_cleanup(void)
{
	int i;

	free(ram);
	ram = NULL;

	for (i=0; i<LAMEBUS_NSLOTS; i++) {
		if (devices[i].ls_info==NULL) {
			continue;
		}
		if (devices[i].ls_info->ldi_cleanup==NULL) {
			continue;
		}
		devices[i].ls_info->ldi_cleanup(devices[i].ls_devdata);
	}
}

void
bus_dumpstate(void)
{
	int i;

	for (i=0; i<LAMEBUS_NSLOTS; i++) {
		if (devices[i].ls_info==NULL) {
			continue;
		}
		msg("************ Slot %d ************", i);
		devices[i].ls_info->ldi_dumpstate(devices[i].ls_devdata);
	}

	msg("RAM:");
	dohexdump(ram, bus_ramsize);
}
