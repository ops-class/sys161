#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h> // for mkdir()
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"

#include "exitcodes.h"
#include "util.h"
#include "console.h"
#include "trace.h"
#include "doom.h"
#include "prof.h"
#include "meter.h"
#include "gdb.h"
#include "cpu.h"
#include "bus.h"
#include "clock.h"
#include "speed.h"
#include "onsel.h"
#include "main.h"
#include "version.h"


/* number of cpu cycles between select polls */
#define ROTOR 50000

/* Global stats */
struct stats g_stats;

/* Flag for interrupting runloop or stoploop due to poweroff */
static int shutoff_flag;

/* Are we stopped in the debugger? */
static int stopped_in_debugger;
static int stop_is_lethal;
static int no_debugger_wait;

/* Did we get an explicit debugger request? */
static int got_debugrequest;

/*
 * Event dispatching model, as of 20140730:
 *
 * 1. main() calls run() calls runloop() and stoploop().
 * stoploop() is separate because it can be called beforehand to
 * wait for an initial debugger connection. Nothing calls back
 * into the main loop from anywhere else.
 *
 * 2. At the main loop level we can be stopped in the debugger or not.
 * (This is about whether the cpu is executing and time is going
 * forward; whether a debugger is actually connected is separate.)
 * The state transition is dispatched from the main loop; other events
 * can request this state transition but should not attempt to
 * directly enact it.
 *
 * 3. At times the main loop will call select, via tryselect(), which
 * dispatches externally caused events. This includes incoming network
 * packets, connections or input data for the various control sockets,
 * and characters typed on the console.
 *
 * 4. At times the main loop will also call into the clock subsystem,
 * which dispatches internally scheduled events. This includes I/O
 * completions and other virtual hardware conditions.
 *
 * 5. Otherwise the main loop calls into the cpu to run. This (in
 * addition to computing) dispatches explicitly triggered events,
 * including I/O starts, interprocessor interrupts, and so forth.
 *
 * Events of any kind may call:
 *    main_poweroff
 *    main_enter_debugger
 *    main_leave_debugger
 * to manipulate the main loop state, which will only take effect
 * once execution returns to the main loop.
 *
 * CPU-triggered events may also need to call cpu_stopcycling() in
 * order to cause this return to the main loop immediately instead of
 * at an arbitrary and unpredictable future point.
 *
 * AN UGLY EXCEPTION to this model is onecycle(), which is called from
 * the gdb interface code (in an externally triggered event) to
 * execute a single cpu cycle. Because it only executes one cycle,
 * however, there is no need to call cpu_stopcycling() in connection
 * with it.
 */

void
main_note_debugrequest(void)
{
	got_debugrequest = 1;
}

void
main_poweroff(void)
{
	shutoff_flag = 1;
}

void
main_enter_debugger(int lethal)
{
	stopped_in_debugger = 1;
	stop_is_lethal = lethal;
}

void
main_leave_debugger(void)
{
	stopped_in_debugger = 0;
	stop_is_lethal = 0;
}

/*
 * This is its own function because it's called from the gdb support
 * to single-step. We only bill the time cpu_cycles reports it
 * actually spent. It may not, if it hits a builtin breakpoint.
 * Builtin breakpoints need to be completely transparent, even to the
 * extent of not wasting a single cycle; then it's at least sort of
 * possible to debug those race conditions where there's a
 * two-instruction window for an interrupt to cause a crash.
 */
void
onecycle(void)
{
	uint64_t ticks;

	ticks = cpu_cycles(1);
	clock_ticks(ticks);
}

static
void
stoploop(void)
{
	gdb_startbreak(no_debugger_wait, stop_is_lethal);
	while (stopped_in_debugger && !shutoff_flag) {
		(void)tryselect(0, 0);
	}
}

static
void
runloop(void)
{
	unsigned rotor;
	uint64_t goticks, wentticks;

	rotor = ROTOR;
	while (!shutoff_flag) {
		goticks = clock_getrunticks();
		if (goticks > rotor) {
			goticks = rotor;
		}
		wentticks = cpu_cycles(goticks);
		clock_ticks(wentticks);

		rotor -= wentticks;
		if (rotor == 0) {
			rotor = ROTOR;
			(void)tryselect(1, 0);
		}

		if (stopped_in_debugger) {
			stoploop();
		}

		if (cpu_running_mask == 0) {
			HWTRACE(DOTRACE_IRQ, ("Waiting for interrupt"));
			clock_waitirq();
		}
	}
}

static
void
initstats(unsigned ncpus)
{
	unsigned i;

	g_stats.s_percpu = domalloc(ncpus * sizeof(*g_stats.s_percpu));
	g_stats.s_numcpus = ncpus;

	for (i=0; i<ncpus; i++) {
		g_stats.s_percpu[i].sp_kcycles = 0;
		g_stats.s_percpu[i].sp_kretired = 0;
		g_stats.s_percpu[i].sp_ucycles = 0;
		g_stats.s_percpu[i].sp_uretired = 0;
		g_stats.s_percpu[i].sp_icycles = 0;
		g_stats.s_percpu[i].sp_lls = 0;
		g_stats.s_percpu[i].sp_okscs = 0;
		g_stats.s_percpu[i].sp_badscs = 0;
		g_stats.s_percpu[i].sp_syncs = 0;
	}
}

static
uint64_t
showstats(void)
{
	uint64_t totcycles;
	unsigned i;

	totcycles = g_stats.s_tot_rcycles + g_stats.s_tot_icycles;
	msg("%llu cycles (%llu run, %llu global-idle)",
	    (unsigned long long)totcycles,
	    (unsigned long long)g_stats.s_tot_rcycles,
	    (unsigned long long)g_stats.s_tot_icycles);

	for (i=0; i<g_stats.s_numcpus; i++) {
		msg("  cpu%u: %llu kern, %llu user, %llu idle; "
		    "%llu ll, %llu/%llu sc, %llu sync", i,
		    (unsigned long long) g_stats.s_percpu[i].sp_kcycles,
		    (unsigned long long) g_stats.s_percpu[i].sp_ucycles,
		    (unsigned long long) g_stats.s_percpu[i].sp_icycles,
		    (unsigned long long) g_stats.s_percpu[i].sp_lls,
		    (unsigned long long) g_stats.s_percpu[i].sp_okscs,
		    (unsigned long long) g_stats.s_percpu[i].sp_badscs,
		    (unsigned long long) g_stats.s_percpu[i].sp_syncs);
	}

	msg("%u irqs %u exns %ur/%uw disk %ur/%uw console %ur/%uw/%um emufs"
	    " %ur/%uw net",
	    g_stats.s_irqs,
	    g_stats.s_exns,
	    g_stats.s_rsects,
	    g_stats.s_wsects,
	    g_stats.s_rchars,
	    g_stats.s_wchars,
	    g_stats.s_remu,
	    g_stats.s_wemu,
	    g_stats.s_memu,
	    g_stats.s_rpkts,
	    g_stats.s_wpkts);

	return totcycles;
}

void
main_dumpstate(void)
{
	msg("mainloop: shutoff_flag %d stopped_in_debugger %d",
	    shutoff_flag, stopped_in_debugger);
#ifdef USE_TRACE
	print_traceflags();
#endif
	gdb_dumpstate();
	showstats();
	clock_dumpstate();
	cpu_dumpstate();
	bus_dumpstate();
}

static
void
run(void)
{
	struct timeval starttime, endtime;
	uint64_t totcycles;
	double time;

	gettimeofday(&starttime, NULL);

	runloop();

	gettimeofday(&endtime, NULL);

	endtime.tv_sec -= starttime.tv_sec;
	if (endtime.tv_usec < starttime.tv_usec) {
		endtime.tv_sec--;
		endtime.tv_usec += 1000000;
	}
	endtime.tv_usec -= starttime.tv_usec;

	time = endtime.tv_sec + endtime.tv_usec/1000000.0;

	totcycles = showstats();

	msg("Elapsed real time: %llu.%06lu seconds (%g mhz)",
	    (unsigned long long) endtime.tv_sec,
	    (unsigned long) endtime.tv_usec,
	    totcycles/(time*1000000.0));
}

////////////////////////////////////////////////////////////

/*
 * We don't use normal getopt because we need to stop on the
 * first non-option argument, and normal getopt has no standard
 * way to specify that.
 */

static const char *myoptarg;
static int myoptind, myoptchr;

static
int
mygetopt(int argc, char **argv, const char *myopts)
{
	int myopt;
	const char *p;

	if (myoptind==0) {
		myoptind = 1;
	}

	do {
		if (myoptind >= argc) {
			return -1;
		}
		
		if (myoptchr==0) {
			if (argv[myoptind][0] != '-') {
				return -1;
			}
			myoptchr = 1;
		}

		myopt = argv[myoptind][myoptchr];

		if (myopt==0) {
			myoptind++;
			myoptchr = 0;
		}
		else {
			myoptchr++;
		}

	} while (myopt == 0);

	if (myopt == ':' || (p = strchr(myopts, myopt))==NULL) {
		return '?';
	}
	if (p[1]==':') {
		/* option takes argument */
		if (strlen(argv[myoptind]+myoptchr)>0) {
			myoptarg = argv[myoptind]+myoptchr;
		}
		else {
			myoptarg = argv[++myoptind];
			if (myoptarg==NULL) {
				return '?';
			}
		}
		myoptind++;
		myoptchr = 0;
	}

	return myopt;
}

////////////////////////////////////////////////////////////

static
void
usage(void)
{
	msg("System/161 %s, compiled %s %s", VERSION, __DATE__, __TIME__);
	msg("Usage: sys161 [sys161 options] kernel [kernel args...]");
	msg("   sys161 options:");
	msg("     -c config      Use alternate config file");
	msg("     -C slot:arg    Override config file argument");
	msg("     -D count       Set disk I/O doom counter");
#ifdef USE_TRACE
	msg("     -f file        Trace to specified file");
	msg("     -P             Collect kernel execution profile");
#else
	msg("     -f file        (trace161 only)");
	msg("     -P             (trace161 only)");
#endif
	msg("     -p port        Listen for gdb over TCP on specified port");
	msg("     -s             Pass signal-generating characters through");
#ifdef USE_TRACE
	msg("     -t[kujtxidne]  Set tracing flags");
	print_traceflags_usage();
#else
	msg("     -t[flags]      (trace161 only)");
#endif
	msg("     -w             Wait for debugger before starting");
	msg("     -X             Don't wait for debugger; exit instead");
	msg("     -Z seconds     Set watchdog timer to specified time");
	die();
}

#define MAXCONFIGEXTRA 128

int
main(int argc, char *argv[])
{
	int port = 2344;
	const char *config = "sys161.conf";
	const char *configextra[MAXCONFIGEXTRA];
	unsigned numconfigextra = 0;
	const char *kernel = NULL;
	int usetcp=0;
	char *argstr = NULL;
	int j, opt;
	size_t argsize=0;
	int debugwait=0;
	int pass_signals=0;
	int timeout;
#ifdef USE_TRACE
	int profiling=0;
#endif
	int doom = 0;
	unsigned ncpus;

	/* This must come absolutely first so msg() can be used. */
	console_earlyinit();
	
	if (sizeof(uint32_t)!=4) {
		/*
		 * Just in case.
		 */
		msg("sys161 requires sizeof(uint32_t)==4");
		die();
	}

	while ((opt = mygetopt(argc, argv, "c:C:D:f:p:Pst:wXZ:"))!=-1) {
		switch (opt) {
		    case 'c': config = myoptarg; break;
		    case 'C':
			if (numconfigextra >= MAXCONFIGEXTRA) {
				msg("Too many -C options");
				die();
			}
			if (strchr(myoptarg, ':') == NULL) {
				msg("Invalid -C option");
				die();
			}
			configextra[numconfigextra++] = myoptarg;
			break;
		    case 'D': doom = atoi(myoptarg); break;
		    case 'f':
#ifdef USE_TRACE
			set_tracefile(myoptarg);
#endif
			break;
		    case 'p': port = atoi(myoptarg); usetcp=1; break;
		    case 'P':
#ifdef USE_TRACE
			profiling = 1;
#endif
			break;
		    case 's': pass_signals = 1; break;
		    case 't': 
#ifdef USE_TRACE
			set_traceflags(myoptarg); 
#endif
			break;
		    case 'w': debugwait = 1; break;
		    case 'X': no_debugger_wait = 1; break;
		    case 'Z':
			timeout = atoi(myoptarg);
			if (timeout <= 1) {
				msg("Invalid timeout (must be at least 2)");
				die();
			}
			clock_setprogresstimeout(timeout);
			break;
		    default: usage();
		}
	}
	if (myoptind==argc) {
		usage();
	}
	kernel = argv[myoptind++];
	
	for (j=myoptind; j<argc; j++) {
		argsize += strlen(argv[j])+1;
	}
	argstr = malloc(argsize+1);
	if (!argstr) {
		msg("malloc failed");
		die();
	}
	*argstr = 0;
	for (j=myoptind; j<argc; j++) {
		strcat(argstr, argv[j]);
		if (j<argc-1) strcat(argstr, " ");
	}

	/* This must come before bus_config in case a network card needs it */
	mkdir(".sockets", 0700);
	
	console_init(pass_signals);
	clock_init();
	ncpus = bus_config(config, configextra, numconfigextra);
	if (doom) {
		doom_establish(doom);
	}

	initstats(ncpus);
	cpu_init(ncpus);

	if (usetcp) {
		gdb_inet_init(port);
	}
	else {
		unlink(".sockets/gdb");
		gdb_unix_init(".sockets/gdb");
	}

	unlink(".sockets/meter");
	meter_init(".sockets/meter");

	load_kernel(kernel, argstr);

	msg("System/161 %s, compiled %s %s", VERSION, __DATE__, __TIME__);
#ifdef USE_TRACE
	print_traceflags();
	if (profiling) {
		prof_setup();
	}
#endif

	if (debugwait) {
		stopped_in_debugger = 1;
		stoploop();
	}

	if (no_debugger_wait) {
		gdb_dontwait();
	}

	run();

#ifdef USE_TRACE
	prof_write();
#endif

	bus_cleanup();
	clock_cleanup();
	console_cleanup();
	
	return got_debugrequest ? SYS161_EXIT_CRASH : SYS161_EXIT_NORMAL;
}
