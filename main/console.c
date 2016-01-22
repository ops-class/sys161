#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include "config.h"

#include "exitcodes.h"
#include "onsel.h"
#include "console.h"
#include "cpu.h"
#include "main.h"
#include "trace.h"
#include "prof.h"


/*
 * Console I/O driver module.
 *
 * This file supports four separate channels:
 *     1. Input for the simulated system console (dev_serial.c or equivalent)
 *     2. Output from the simulated system console.
 *     3. Internally generated message output.
 *     4. Trace output.
 *
 * Input can come from stdin, or (in the FUTURE) from a script file
 * that has timed keystroke events. stdin may be a tty, a file, or a
 * pipe; even if it is a tty we need to accept characters arriving at
 * high speed to allow window-system pastes.
 *
 * By default the system console is connected to stdout, messages go
 * to stderr, and tracing (if any) also goes to stderr, all of which
 * is the tty.
 *
 * Since tracing can be voluminous, it is possible to send trace
 * messages to a file, or even (FUTURE) through a pipe to gzip. For
 * maximum utility of such logs, messages are also repeated there, and
 * console output is presented in a schematic format.
 */

////////////////////////////////////////////////////////////
// types and data

typedef enum {
	MT_CONSOLE,
	MT_MSG,
	MT_CPUTRACE,
	MT_HWTRACE,
} msgtypes;

struct output {
#ifdef USE_TRACE
	FILE *f;
#endif
	int fd;
	int needs_close;
	int is_tty;
	int needs_crs;

	int at_bol;
	msgtypes last_msgtype;
	unsigned last_cpunum;
};

static struct output *o_stdout;
static struct output *o_stderr;

#ifdef USE_TRACE
static struct output *o_tracefile;
static struct output *trace_to;		/* may be any of the three o_* */
#endif

static int stdin_generates_signals = 0;
static int stdin_is_tty = 0, stdin_tty_active = 0;
static int got_stdin_tios = 0;
static struct termios stdin_savetios, stdin_runtios;

static int console_up=0;

static void (*onkey)(void *data, int ch);
static void *onkeydata;

////////////////////////////////////////////////////////////
// TTY management

/*
 * We only install our termios settings when we're in the foreground.
 * Also, it's important to only fetch termios settings and derive ours
 * from them when we're in the foreground too; otherwise we start with
 * some other program's settings and that can cause interesting
 * complications.
 *
 * We only care seriously about termios settings for stdin and when
 * stdin is a tty. For stdout/stderr, we check ONLCR to figure out
 * whether to issue CRs (whether or not we're in the background, and
 * recheck if things change) but that's all.
 */

/*
 * Check if we're in the foreground. We are in the foreground if the
 * foreground process group registered in the tty is the same as our
 * process group.
 */
static
int
in_foreground(void)
{
	/* shouldn't be here and don't care unless stdin is the tty */
	Assert(stdin_is_tty);

	return tcgetpgrp(STDIN_FILENO) == getpgrp();
}

/*
 * Retrieve termios settings.
 */
static
void
tty_get_tios(void)
{
	if (tcgetattr(STDIN_FILENO, &stdin_savetios) == 0) {
		stdin_runtios = stdin_savetios;

#ifdef XCASE
		stdin_runtios.c_lflag &= ~XCASE;
#endif
		stdin_runtios.c_lflag &= ~(ICANON|ECHO|ECHONL|NOFLSH);
		if (stdin_generates_signals) {
			stdin_runtios.c_lflag |= ISIG;
		}
		else {
			stdin_runtios.c_lflag &= ~ISIG;
		}
		stdin_runtios.c_iflag &= ~(ICRNL|INLCR);
		stdin_runtios.c_cflag |= CREAD;
		stdin_runtios.c_cc[VTIME] = 0;
		stdin_runtios.c_cc[VMIN] = 0;

		got_stdin_tios = 1;
	}
}

/*
 * Install our termios settings. Get them if needed.
 */
static
int
tty_activate(void)
{
	if (stdin_is_tty && !stdin_tty_active) {
		if (in_foreground()) {
			if (!got_stdin_tios) {
				tty_get_tios();
			}
			tcsetattr(STDIN_FILENO, TCSADRAIN, &stdin_runtios);
			stdin_tty_active = 1;
			return 1;
		}
	}
	return 0;
}

/*
 * Restore termios settings.
 */
static
int
tty_deactivate(void)
{
	if (stdin_is_tty && stdin_tty_active) {
		Assert(got_stdin_tios);
		tcsetattr(STDIN_FILENO, TCSADRAIN, &stdin_savetios);
		stdin_tty_active = 0;
		return 1;
	}
	return 0;
}

/*
 * Initialize: check if stdin is a tty, and if we're in the foreground
 * get the termios settings. If not, that'll have to wait until we are
 * in the foreground at some future point, if we ever are.
 */
static
void
tty_init(void)
{
	struct termios tios;

	if (tcgetattr(STDIN_FILENO, &tios) == 0) {
		stdin_is_tty = 1;
		if (in_foreground()) {
			tty_get_tios();
		}
	}
}

/*
 * Clean up: restore termios settings.
 */
static
void
tty_cleanup(void)
{
	tty_deactivate();
}

/*
 * Check a fd for being a tty, and if so, whether we need to
 * add \r before \n.
 */
static
void
tty_checkcrs(int fd, int *is_tty, int *needscrs)
{
	struct termios tios;

	if (tcgetattr(fd, &tios) < 0) {
		*is_tty = 0;
		*needscrs = 0;
		return;
	}
	*is_tty = 1;
	*needscrs = (tios.c_oflag & ONLCR) != 0;
}

////////////////////////////////////////////////////////////
// Output helpers

#ifndef USE_TRACE
static
size_t
dowrite(int fd, const char *buf, size_t len)
{
	int r;
	r = write(fd, buf, len);
	if (r<=0) {
		static int evil = 0;
		const char *errmsg;

		errmsg = (r==0) ? "zero-length write" : strerror(errno);
		if (evil < 1) {
			evil++;
			msg("write: %s", errmsg);
		}
		if (evil < 2) {
			evil++;
			console_cleanup();
		}
		exit(SYS161_EXIT_ERROR);
	}
	return (unsigned) r;
}

static
void
writestr(int fd, const char *buf, size_t len)
{
	size_t tot = 0;
	while (tot < len) {
		tot += dowrite(fd, buf+tot, len-tot);
	}
}
#endif /* not USE_TRACE */

////////////////////////////////////////////////////////////
// Common output logic

static
struct output *
output_create(FILE *f, int needs_close)
{
	struct output *o;

	o = malloc(sizeof(struct output));
	if (o==NULL) {
		fprintf(stderr, "malloc: Out of memory\n");
		die();
	}
#ifdef USE_TRACE
	o->f = f;
#endif
	o->fd = fileno(f);
	o->needs_close = needs_close;
	tty_checkcrs(fileno(f), &o->is_tty, &o->needs_crs);
	o->at_bol = 1;
	o->last_msgtype = MT_MSG;
	o->last_cpunum = 0;
	return o;
}

static
void
output_destroy(struct output *o)
{
#ifdef USE_TRACE
	if (o->f) {
		fflush(o->f);
	}
#endif
	if (o->needs_close) {
#ifdef USE_TRACE
		fclose(o->f);
		o->f = NULL;
#else
		close(o->fd);
#endif
		o->fd = -1;
	}
	free(o);
}

static
void
output_checktty(struct output *o)
{
	tty_checkcrs(o->fd, &o->is_tty, &o->needs_crs);
}

static
void
output_putc(struct output *o, int c)
{
#ifdef USE_TRACE
	fputc(c, o->f);
#else
	char ch = c;
	writestr(o->fd, &ch, 1);
#endif
}

static
void
output_eol(struct output *o)
{
	if (o->needs_crs) {
		output_putc(o, '\r');
	}
	output_putc(o, '\n');
}

static
void
output_vsay(struct output *o, const char *fmt, va_list ap)
{
#ifdef USE_TRACE
	vfprintf(o->f, fmt, ap);
#else
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	writestr(o->fd, buf, strlen(buf));
#endif
}

static
void
output_say(struct output *o, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vsay(o, fmt, ap);
	va_end(ap);
}

static
void
output_flush(struct output *o)
{
	if (!o->at_bol) {
		output_eol(o);
		o->at_bol = 1;
	}
#ifdef USE_TRACE
	if (o->f != NULL) {
		fflush(o->f);
	}
#endif
}

/*
 * Output a character. Use message type MT. If MT is not MT_CPUTRACE,
 * cpunum should always be 0.
 *
 * This is the same level of operation as output_{v,}msg{,l}.
 */
static
void
output_char(struct output *o, msgtypes mt, unsigned cpunum, int c)
{
	if (!o->at_bol &&
	    (o->last_msgtype != mt || cpunum != o->last_cpunum)) {
		output_eol(o);
		o->at_bol = 1;
	}
	if (c == '\n') {
		if (o->needs_crs) {
			output_putc(o, '\r');
		}
		output_putc(o, c);
		o->at_bol = 1;
	}
	else {
		output_putc(o, c);
		o->at_bol = 0;
	}
	o->last_msgtype = mt;
	o->last_cpunum = cpunum;
}


/*
 * Output a message, without an implicit newline, va_list-style.
 *
 * Use message type MT. If MT is not MT_TRACE, cpunum should always be 0.
 */
static
void
output_vmsgl(msgtypes mt, unsigned cpunum,
	     struct output *o, const char *fmt, va_list ap)
{
	if (!o->at_bol &&
	    (o->last_msgtype != mt || cpunum != o->last_cpunum)) {
		output_eol(o);
		o->at_bol = 1;
	}
	if (o->at_bol) {
		switch (mt) {
		    case MT_CONSOLE:
			output_say(o, "console: ");
			break;
		    case MT_MSG:
			output_say(o, "sys161: ");
			break;
		    case MT_CPUTRACE:
			output_say(o, "trace: %02x ", cpunum);
			break;
		    case MT_HWTRACE:
			output_say(o, "trace: -- ");
			break;
		}
	}
	output_vsay(o, fmt, ap);
	o->at_bol = 0;
	o->last_msgtype = mt;
	o->last_cpunum = cpunum;
}

static
void
output_vmsg(msgtypes mt, unsigned cpunum,
	    struct output *o, const char *fmt, va_list ap)
{
	output_vmsgl(mt, cpunum, o, fmt, ap);
	output_eol(o);
	o->at_bol = 1;
}

#ifdef USE_TRACE
static
void
output_msg(msgtypes mt, unsigned cpunum,
	   struct output *o, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsg(mt, cpunum, o, fmt, ap);
	va_end(ap);
}
#endif

////////////////////////////////////////////////////////////
// Common input logic

/*
 * Read a character.
 */
static
int
console_getc(void)
{
	char ch;
	int r;

	r = read(STDIN_FILENO, &ch, 1);
	if (r<0) {
		smoke("Read error on stdin: %s", strerror(errno));
	}
	else if (r==0) {
		/* EOF - send back -1 and hope nothing breaks */
		return -1;
	}
	/* be sure to return 0-255 and never -1 for a real character */
	return (int)(unsigned)(unsigned char)ch;
}

/*
 * Select hook for the console.
 */
static
int
console_sel(void *unused)
{
	int ch;
	(void)unused;

	ch = console_getc();
	if (ch=='\a') {
		/* ^G (BEL) - interrupt */

		/* this should have no effect, but just in case */
		cpu_stopcycling();

		/* drop to the debugger */
		main_enter_debugger(0 /* not lethal */);
	}
	else if (ch == -1) {
		/*
		 * EOF - disconnect the console from the select loop,
		 * but _don't_ exit. For now, this is needed to be
		 * able to run with /dev/null as input. In the FUTURE
		 * maybe we can do this better. Assume that if we get
		 * hung up on so we get a real EOF from our tty that
		 * we'll be sent SIGHUP.
		 */
		return -1;
	}
	else if (onkey) {
		onkey(onkeydata, ch);
	}

	return 0;
}

void
console_onkey(void *data, void (*func)(void *, int))
{
	onkeydata = data;
	onkey = func;
}

////////////////////////////////////////////////////////////
// Output routines for hardware devices

void
console_putc(int c)
{
	output_char(o_stdout, MT_CONSOLE, 0, c);
#ifdef USE_TRACE
	if (o_tracefile) {
		char tmp[4];
		int ix=0;
		if ((c >= 32 && c<127)||(c>=32+128 && c<255)) {
			tmp[ix++] = c;
		}
		else {
			tmp[ix++] = '\\';
			switch (c) {
			    case '\a': tmp[ix++]='a'; break; 
			    case '\b': tmp[ix++]='b'; break; 
			    case '\t': tmp[ix++]='t'; break; 
			    case '\n': tmp[ix++]='n'; break; 
			    case '\v': tmp[ix++]='v'; break; 
			    case '\f': tmp[ix++]='f'; break; 
			    case '\r': tmp[ix++]='r'; break; 
			    default:
				snprintf(tmp+ix, 2, "%02x", c);
				ix+=2;
				break;
			}
		}
		tmp[ix++] = 0;
		output_msg(MT_CONSOLE, 0, o_tracefile, 
			   "`%s' (%d / 0x%x)", tmp, c, c);
	}
	fflush(o_stdout->f);
#endif
}

void
console_beep(void)
{
	if (o_stdout->is_tty) {
		console_putc('\a');
#ifdef USE_TRACE
		if (o_tracefile) {
			output_msg(MT_MSG, 0, o_tracefile, "[BEEP]");
		}
#endif
	}
	else {
		msg("[BEEP]");
	}
}

////////////////////////////////////////////////////////////
// Message output

static
void
commondie(int code)
{
#ifdef USE_TRACE
	prof_write();
#endif
	console_cleanup();
	exit(code);
}

void
die(void)
{
	commondie(SYS161_EXIT_ERROR);
}

void
crashdie(void)
{
	commondie(SYS161_EXIT_CRASH);
}

void
reqdie(void)
{
	commondie(SYS161_EXIT_REQUESTED);
}

void
msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsg(MT_MSG, 0, o_stderr ? o_stderr : o_stdout, fmt, ap);
	va_end(ap);
}

void
msgl(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsgl(MT_MSG, 0, o_stderr ? o_stderr : o_stdout, fmt, ap);
	va_end(ap);
}

void
smoke(const char *fmt, ...)
{
	va_list ap;
	
	va_start(ap, fmt);
	output_vmsg(MT_MSG, 0, o_stderr ? o_stderr : o_stdout, fmt, ap);
	va_end(ap);
	
	msg("The hardware has failed.");
	msg("In real life this is where the smoke starts pouring out.");
	
	console_cleanup();
	abort();
}

void
hang(const char *fmt, ...)  // was crash()
{
	va_list ap;
	
	va_start(ap, fmt);
	output_vmsg(MT_MSG, 0, o_stderr ? o_stderr : o_stdout, fmt, ap);
	va_end(ap);
	
	msg("You did something the hardware didn't like.");
	msg("In real life the machine would hang for no apparent reason,");
	msg("or maybe start to act strangely.");

	output_flush(o_stdout);
	if (o_stderr != NULL) {
		output_flush(o_stderr);
	}
#ifdef USE_TRACE
	if (o_tracefile != NULL) {
		output_flush(o_tracefile);
	}
#endif

	// wait for debugger connection
	cpu_stopcycling();
	main_enter_debugger(1 /* lethal */);
	
	//console_cleanup();
	//exit(1);
}

////////////////////////////////////////////////////////////
// trace output

#ifdef USE_TRACE

void
set_tracefile(const char *filename)
{
	FILE *f;

	if (o_tracefile != NULL) {
		smoke("Multiple calls to set_tracefile");
	}

	if (filename && !strcmp(filename, "-")) {
		trace_to = o_stdout;
		return;
	}
	else if (filename) {
		f = fopen(filename, "w");
		if (f == NULL) {
			msg("Cannot open tracefile %s: %s", 
			    filename, strerror(errno));
			die();
		}
		o_tracefile = output_create(f, 1);
		o_tracefile->last_msgtype = MT_HWTRACE;
		trace_to = o_tracefile;
	}
	else {
		trace_to = o_stderr ? o_stderr : o_stdout;
	}
}

void
cputrace(unsigned cpunum, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsg(MT_CPUTRACE, cpunum, trace_to, fmt, ap);
	va_end(ap);
}

void
cputracel(unsigned cpunum, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsgl(MT_CPUTRACE, cpunum, trace_to, fmt, ap);
	va_end(ap);
}

void
hwtrace(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsg(MT_HWTRACE, 0, trace_to, fmt, ap);
	va_end(ap);
}

void
hwtracel(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	output_vmsgl(MT_HWTRACE, 0, trace_to, fmt, ap);
	va_end(ap);
}

#endif

#ifdef USE_TRACE
/* only useful when tracing to a file */
static
void
console_sig(int sig)
{
	static int evil=0;
	if (evil==0) {
		evil = 1;	// protect against recursive invocation
		if (o_tracefile && o_tracefile->f) {
			fflush(o_tracefile->f);
		}
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static
void
console_getsignals(void)
{
	/* On fatal signals, try to fflush the trace file before croaking */
	signal(SIGHUP, console_sig);
	signal(SIGINT, console_sig);
	signal(SIGQUIT, console_sig);
	signal(SIGILL, console_sig);
	signal(SIGABRT, console_sig);
	signal(SIGFPE, console_sig);
	signal(SIGKILL, console_sig);	// won't work, but try anyway
	signal(SIGBUS, console_sig);
	signal(SIGSEGV, console_sig);
	signal(SIGALRM, console_sig);
	signal(SIGTERM, console_sig);
	signal(SIGURG, console_sig);
	signal(SIGUSR1, console_sig);
	signal(SIGUSR2, console_sig);
#ifdef SIGXCPU
	signal(SIGXCPU, console_sig);
#endif
#ifdef SIGXFSZ
	signal(SIGXFSZ, console_sig);
#endif
#ifdef SIGVTALRM
	signal(SIGVTALRM, console_sig);
#endif
#ifdef SIGPROF
	signal(SIGPROF, console_sig);
#endif
#ifdef SIGPWR
	signal(SIGPWR, console_sig);
#endif

	/*
	 * Don't crash on SIGPIPE; ignore it instead. We poll all our
	 * fds regularly and check for EOF, so we don't need it, and
	 * if something closes between calling select and writing to
	 * it we die unnecessarily.
	 */
	signal(SIGPIPE, SIG_IGN);
}

#endif /* USE_TRACE */

////////////////////////////////////////////////////////////
// Foreground/background logic

/*
 * SIGTSTP handler: before stopping, check if we need to restore the
 * tty settings, and if so, also stop polling stdin. (Not that it
 * matters while we're stopped, but it makes the continue logic
 * simpler to do it this way.) Then post SIGSTOP to really stop.
 */
static
void
onstop(int sig)
{
	(void)sig;

	if (tty_deactivate()) {
		notonselect(STDIN_FILENO);
	}
	raise(SIGSTOP);
}

/*
 * SIGCONT handler: check if we need to install our tty settings,
 * and if so, also start polling stdin. Then check the tty settings
 * for the output files, in case we've been backgrounded and the
 * (minor) thing we care about changed.
 *
 * Finally, reinstall the signal handlers in case we're on a dumb OS.
 */
static
void
oncont(int sig)
{
	(void)sig;

	if (tty_activate()) {
		onselect(STDIN_FILENO, NULL, console_sel, NULL);
	}

	output_checktty(o_stdout);
	if (o_stderr != NULL) {
		output_checktty(o_stderr);
	}
	/* o_tracefile can only be a regular file; don't need to checktty it */

	/* in case we have idiot svr4 oneshot signals */
	signal(SIGTSTP, onstop);
	signal(SIGCONT, oncont);
}

/*
 * Install the signal handlers for stop/background handling.
 */
static
void
get_bg_signals(void)
{
	signal(SIGTSTP, onstop);
	signal(SIGCONT, oncont);
}

////////////////////////////////////////////////////////////
// Setup and shutdown code

/*
 * Console initialization comes in two phases; one very early (even
 * before command-line handling) so we can consistently use msg() for
 * diagnostics, and the second after command-line handling so we know
 * whether to pass on ^C.
 */

void
console_earlyinit(void)
{
	struct stat stdout_stat, stderr_stat;

	if (fstat(STDOUT_FILENO, &stdout_stat)) {
		fprintf(stderr, "fstat stdout: %s\n", strerror(errno));
		exit(1);
	}
	if (fstat(STDERR_FILENO, &stderr_stat)) {
		fprintf(stderr, "fstat stderr: %s\n", strerror(errno));
		exit(1);
	}

	o_stdout = output_create(stdout, 0);

	if (stdout_stat.st_dev == stderr_stat.st_dev &&
	    stdout_stat.st_ino == stderr_stat.st_ino) {
		/* same object */
		o_stderr = NULL;
	}
	else {
		o_stderr = output_create(stderr, 0);
	}
	
#ifdef USE_TRACE
	o_tracefile = NULL;
	if (o_stderr != NULL) {
		trace_to = o_stderr;
	}
	else {
		trace_to = o_stdout;
	}
#endif
}

void
console_init(int pass_signals)
{
	if (console_up) {
		smoke("Multiple calls to console_init");
	}

#ifdef USE_TRACE
	console_getsignals();
#else
	signal(SIGPIPE, SIG_IGN);
#endif /* USE_TRACE */

	stdin_generates_signals = !pass_signals;
	tty_init();
	if (tty_activate()) {
		onselect(STDIN_FILENO, NULL, console_sel, NULL);
	}
	get_bg_signals();
	console_up = 1;
}

void
console_cleanup(void)
{
	if (o_stdout != NULL) {
		output_flush(o_stdout);
	}
	if (o_stderr != NULL) {
		output_flush(o_stderr);
	}
#ifdef USE_TRACE
	if (o_tracefile != NULL) {
		output_flush(o_tracefile);
	}
#endif
	tty_cleanup();
	if (console_up) {
		console_up = 0;
	}

#ifdef USE_TRACE
	if (o_tracefile != NULL) {
		output_destroy(o_tracefile);
		o_tracefile = NULL;
		trace_to = o_stderr ? o_stderr : o_stdout;
	}
#endif
	if (o_stderr != NULL) {
		output_destroy(o_stderr);
		o_stderr = NULL;
	}
	output_destroy(o_stdout);
	o_stdout = NULL;
}
