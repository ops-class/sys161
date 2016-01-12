/*
 * Transparent profiling support.
 */

#include <sys/types.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "gmon.h"
#include "speed.h"
#include "clock.h"
#include "console.h"
#include "cpu.h"
#include "prof.h"


#ifdef USE_TRACE

#define PROFILE_FILE "gmon.out"
#define PROFILE_HZ (1000000000/PROFILE_NSECS)

/*
 * Let's use 16-byte profiling bins.
 * We could probably afford to use more memory, but 16 bytes
 * should give us perfectly acceptable results.
 */
#define PROF_BINSIZE  16
#define PROF_ADDR2BIN(addr) (((addr)-prof_textbase)/PROF_BINSIZE)

struct cgentry {
	struct cgentry *next;
	struct gmon_callgraph_entry gce;
};

struct cgbin {
	struct cgentry *list;
};

static uint32_t prof_textbase = 0;
static uint32_t prof_textend = 0;
static uint32_t prof_samplenum;
static uint16_t *prof_sampledata;
static struct cgbin *prof_cg;
static int prof_on = 0;
static int prof_active = 0;

void
prof_enable(void)
{
	if (prof_on) {
		prof_active = 1;
	}
}

void
prof_disable(void)
{
	prof_active = 0;
}

int
prof_isenabled(void)
{
	return prof_active;
}

static
void
prof_sample(void *junk1, uint32_t junk2)
{
	uint32_t pc;
	unsigned bin;

	(void)junk1;
	(void)junk2;

	if (prof_active) {
		pc = cpuprof_sample();
		bin = PROF_ADDR2BIN(pc);

		if (bin < prof_samplenum) {
			prof_sampledata[bin]++;
		}
	}

	schedule_event(PROFILE_NSECS, NULL, 0, prof_sample, 
		       "profiling sampler");
}

void
prof_call(uint32_t frompc, uint32_t topc)
{
	unsigned bin;
	struct cgbin *cb;
	struct cgentry *ce;

	if (prof_active==0) {
		return;
	}

	bin = PROF_ADDR2BIN(frompc);
	if (bin >= prof_samplenum) {
		/* out of range; skip */
		return;
	}

	cb = &prof_cg[bin];

	for (ce = cb->list; ce; ce = ce->next) {
		if (ce->gce.gce_from == frompc &&
		    ce->gce.gce_to == topc) {
			ce->gce.gce_count++;
			return;
		}
	}

	/* need to add a new entry */
	ce = malloc(sizeof(struct cgentry));
	if (ce==NULL) {
		msg("malloc failed updating profiling data");
		die();
	}
	ce->gce.gce_from = frompc;
	ce->gce.gce_to = topc;
	ce->gce.gce_count = 1;

	/* add to head of list */
	ce->next = cb->list;
	cb->list = ce;
}

void
prof_clear(void)
{
	struct cgentry *ce;
	unsigned i;

	if (prof_on == 0) {
		return;
	}

	for (i=0; i<prof_samplenum; i++) {
		prof_sampledata[i] = 0;
		while (prof_cg[i].list != NULL) {
			ce = prof_cg[i].list;
			prof_cg[i].list = prof_cg[i].list->next;
			free(ce);
		}
	}
}

static
void
writebyte(int val, FILE *f)
{
	uint8_t byte = val;
	fwrite(&byte, 1, sizeof(uint8_t), f);
}

void
prof_write(void)
{
	FILE *f;
	struct gmon_file_header gfh;
	struct gmon_histogram_header ghh;
	struct gmon_callgraph_entry gcetmp;
	struct cgbin *cb;
	struct cgentry *ce;
	unsigned i;
	unsigned long len;

	if (prof_on == 0) {
		return;
	}

	f = fopen(PROFILE_FILE, "w");
	if (!f) {
		msg("Could not open %s (skipping)", PROFILE_FILE);
		return;
	}

	/* file header */
	memset(&gfh, 0, sizeof(gfh));
	memcpy(gfh.gfh_magic, "gmon", 4);
	gfh.gfh_version = htonl(GMON_VERSION);
	fwrite(&gfh, 1, sizeof(gfh), f);

	/* histogram */
	writebyte(GMON_RT_HISTOGRAM, f);
	memset(&ghh, 0, sizeof(ghh));
	ghh.ghh_lowpc = htonl(prof_textbase);
	ghh.ghh_highpc = htonl(prof_textend);
	ghh.ghh_size = htonl(prof_samplenum);
	ghh.ghh_hz = htonl(PROFILE_HZ);
	strcpy(ghh.ghh_name, "seconds");
	ghh.ghh_abbrev = 's';
	fwrite(&ghh, 1, sizeof(ghh), f);
	for (i=0; i<prof_samplenum; i++) {
		uint16_t tmp = htons(prof_sampledata[i]);
		fwrite(&tmp, 1, sizeof(tmp), f);
	}

	/* call graph */
	for (i=0; i<prof_samplenum; i++) {
		cb = &prof_cg[i];
		for (ce = cb->list; ce; ce = ce->next) {
			writebyte(GMON_RT_CALLGRAPH, f);
			gcetmp.gce_from = htonl(ce->gce.gce_from);
			gcetmp.gce_to = htonl(ce->gce.gce_to);
			gcetmp.gce_count = htonl(ce->gce.gce_count);
			fwrite(&gcetmp, 1, sizeof(gcetmp), f);
		}
	}

	fflush(f);
	if (ferror(f)) {
		msg("Warning: error writing %s", PROFILE_FILE);
	}
	else {
		len = ftell(f);
		msg("%lu bytes written to %s", len, PROFILE_FILE);
	}
	fclose(f);
}

void
prof_addtext(uint32_t textbase, uint32_t textsize)
{
	uint32_t textend;

	/* Round size up to a multiple of PROF_BINSIZE, which is a power of 2*/
	textsize = (textsize+PROF_BINSIZE+1) & ~(uint32_t)PROF_BINSIZE;

	if (textsize==0) {
		/* just in case */
		return;
	}

	textend = textbase + textsize;

	if (prof_textbase==0 && prof_textend==0) {
		prof_textbase = textbase;
		prof_textend = textend;
	}
	else {
		if (textbase < prof_textbase) {
			prof_textbase = textbase;
		}
		if (textend > prof_textend) {
			prof_textend = textend;
		}
	}
	if (prof_textend <= prof_textbase) {
		smoke("Profiling text region corrupt");
	}
}

void
prof_setup(void)
{
	unsigned i;

	if (prof_textbase==0 && prof_textend==0) {
		/* no text to profile? */
		return;
	}

	if (prof_textend <= prof_textbase) {
		smoke("Profiling text region corrupt");
	}

	/* note: prof_textend has already been rounded up appropriately */
	prof_samplenum = (prof_textend - prof_textbase) / PROF_BINSIZE;

	prof_sampledata = malloc(sizeof(uint16_t)*prof_samplenum);
	if (prof_sampledata==NULL) {
		msg("malloc failed");
		die();
	}

	prof_cg = malloc(sizeof(struct cgbin)*prof_samplenum);
	if (prof_cg==NULL) {
		msg("malloc failed");
		die();
	}

	for (i=0; i<prof_samplenum; i++) {
		prof_sampledata[i] = 0;
		prof_cg[i].list = NULL;
	}

	prof_on = 1;
	prof_active = 1;
	schedule_event(PROFILE_NSECS, NULL, 0, prof_sample, 
		       "profiling sampler");
}

#endif /* USE_TRACE */
