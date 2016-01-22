#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"

#include "util.h"
#include "console.h"
#include "gdb.h"
#include "cpu.h"
#include "bus.h"
#include "memdefs.h"
#include "main.h"

#include "context.h"

//#define SHOW_PACKETS


extern struct gdbcontext g_ctx;
extern int g_ctx_inuse;

static unsigned debug_cpu;

static
void
unset_breakcond(void)
{
	main_leave_debugger();
}

////////////////////////////////////////////////////////////
// support functions

static
void
printbyte(char *buf, size_t maxlen, uint32_t val)
{
	size_t len = strlen(buf);
	Assert(len < maxlen);

	snprintf(buf+len, maxlen - len, "%02x", val);
}

static
void
printword(char *buf, size_t maxlen, uint32_t val)
{
	size_t len = strlen(buf);
	Assert(len < maxlen);

	snprintf(buf+len, maxlen - len, "%08x", val);
}

static
uint8_t
hexbyte(const char *s, char **ret)
{
	char buf[3];
	int i, j;

	for (i=j=0; i<2 && s[i]; i++) {
		buf[j++] = s[i];
	}
	buf[j] = 0;
	*ret = (char *)&s[i];
	return strtoul(buf, NULL, 16);
}

static
unsigned
getthreadid(const char *s)
{
	char *ign;

	return hexbyte(s, &ign) - 10;
}

static
unsigned
mkthreadid(unsigned cpunum)
{
	return cpunum + 10;
}

////////////////////////////////////////////////////////////
// packet sending

static
void 
debug_send(struct gdbcontext *ctx, const char *string) 
{
	char checkstr[8];
	int check=0;
	int i;

	if (ctx->myfd < 0) {
		msg("Warning: sending debugger packet, no debugger");
		msg("(please file a bug report)");
	}

	for (i=0; string[i]; i++) {
		check += string[i];
	}

	check %= 256;
	snprintf(checkstr, sizeof(checkstr),  "#%02x", check);

#ifdef SHOW_PACKETS
	msg("Sending $%s%s", string, checkstr);
#endif

	write(ctx->myfd, "$", 1);
	write(ctx->myfd, string, strlen(string)); 
	write(ctx->myfd, checkstr, strlen(checkstr));
}

static
PF(2, 3)
void
debug_sendf(struct gdbcontext *ctx, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	debug_send(ctx, buf);
}

static
void
debug_notsupp(struct gdbcontext *ctx)
{
	const char rep[] = "$\0#00";
#ifdef SHOW_PACKETS
	msg("Sending $\\0#00");
#endif
	write(ctx->myfd, rep, sizeof(rep) - 1);
}

static
void
debug_send_stopinfo(struct gdbcontext *ctx)
{
	//debug_send(ctx, "S05");
	/*
	 * Sending the core number appears to require gdb 7; gdb 6 is
	 * confused by the 'c' into thinking it's a hex number. Also,
	 * you need a trailing semicolon, which gdb does *not*
	 * document.
	 */
	//debug_sendf(ctx, "T05thread:%x;core:%x;",
	//	    mkthreadid(debug_cpu), mkthreadid(debug_cpu));
	debug_sendf(ctx, "T05thread:%x;", mkthreadid(debug_cpu));
}

////////////////////////////////////////////////////////////
// external control interface

void
gdb_startbreak(int dontwait, int lethal)
{
	debug_cpu = cpudebug_get_break_cpu();
	if (g_ctx_inuse) {
		/* If connected, tell the debugger we stopped */
		debug_send_stopinfo(&g_ctx);
	}
	else {
		if (dontwait && lethal) {
			msg("Exiting instead of waiting for debugger...");
			crashdie();
		}
		else if (dontwait) {
			msg("Not waiting for debugger...");
			main_leave_debugger();
		}
		else {
			msg("Waiting for debugger connection...");
		}
	}
}

////////////////////////////////////////////////////////////
// packet operations

static
void
debug_register_print(struct gdbcontext *ctx)
{
	uint32_t regs[256];
	int i, nregs;
	char buf[BUFLEN];

	cpudebug_getregs(debug_cpu, regs, 256, &nregs);
	Assert(nregs <= 256);

	buf[0] = 0;
	for (i=0; i<nregs; i++) {
		printword(buf, sizeof(buf), regs[i]);
	}

	debug_send(ctx, buf);
}

static
void
debug_read_mem(struct gdbcontext *ctx, const char *spec)
{
	uint32_t vaddr, length, i;
	uint32_t word;
	uint8_t byte;
	const char *curptr;
	char buf[BUFLEN];

	vaddr = strtoul(spec, (char **)&curptr, 16);
	length = strtoul(curptr+1, NULL, 16);

	buf[0] = 0;

	for (i=0; i<length && (vaddr+i)%4 != 0; i++) {
		if (cpudebug_fetch_byte(debug_cpu, vaddr+i, &byte)) {
			debug_send(ctx, "E03");
			return;
		}
		printbyte(buf, sizeof(buf), byte);
	}
	for (; i<length; i += 4) {
		if (cpudebug_fetch_word(debug_cpu, vaddr+i, &word)) {
			debug_send(ctx, "E03");
			return;
		}
		printword(buf, sizeof(buf), word);
	}
	debug_send(ctx, buf);
}

static
void
debug_write_mem(struct gdbcontext *ctx, const char *spec)
{
	uint32_t vaddr, length, i;
	uint8_t *bytes;
	const char *curptr;

	// AAAAAAA,LLL:DDDD
	// address,len,data
	vaddr = strtoul(spec, (char **) &curptr, 16);
	length = strtoul(curptr + 1, (char **)&curptr, 16);

	// curptr now points to the ':' which 
	// delimits the length from the data
	// so we advance it a little
	curptr++;

	bytes = domalloc(length);
	for (i=0; i<length; i++) {
		bytes[i] = hexbyte(curptr, (char **) &curptr);
	}

	for (i=0; i<length && (vaddr+i)%4 != 0; i++) {
		if (cpudebug_store_byte(debug_cpu, vaddr+i, bytes[i])) {
			debug_send(ctx, "E03");
			return;
		}
	}
	for (; i+4<=length; i+=4) {
		uint32_t word;
		memcpy(&word, bytes+i, sizeof(uint32_t));
		if (cpudebug_store_word(debug_cpu, vaddr+i, ntohl(word))) {
			debug_send(ctx, "E03");
			return;
		}
	}
	for (; i<length; i++) {
		if (cpudebug_store_byte(debug_cpu, vaddr+i, bytes[i])) {
			debug_send(ctx, "E03");
			return;
		}
	}

	free(bytes);
	debug_send(ctx, "OK");
}

static
void
debug_restart(struct gdbcontext *ctx, const char *addr)
{
	unsigned int realaddr;

	(void)ctx;  /* ? */

	if (*addr == '\0') {
		return;
	}
	msg("whee!  gdb changed the restart address");
	realaddr = strtoul(addr, NULL, 16);
	/*
	 * XXX: this should be the cpu set by the 'Hc' packet rather
	 * than the 'Hg' one, probably.
	 */
	cpu_set_entrypoint(debug_cpu, realaddr);
}

static
void
debug_checkthread(struct gdbcontext *ctx, const char *threadid)
{
	unsigned cpunum;

	cpunum = getthreadid(threadid);
	if (cpunum >= cpu_numcpus()) {
		debug_send(ctx, "E00");
		return;
	}

	if (!cpu_enabled(cpunum)) {
		debug_send(ctx, "E01");
		return;
	}

	debug_send(ctx, "OK");
}

static
void
debug_getthreadinfo(struct gdbcontext *ctx, const char *threadid)
{
	unsigned cpunum;
	char buf[128], xbuf[256];
	size_t i;

	cpunum = getthreadid(threadid);
	if (cpunum >= cpu_numcpus()) {
		debug_send(ctx, "E00");
		return;
	}

	/*
	 * Send back string of whatever info we want. Since gdb
	 * already fetches and prints the frame info, and fetches this
	 * but doesn't seem to print it, I dunno what to send.
	 */

	snprintf(buf, sizeof(buf), "CPU %u", cpunum);

	/* Code as hex for transmission */
	xbuf[0] = 0;
	for (i=0; buf[i]; i++) {
		printbyte(xbuf, sizeof(xbuf), (unsigned char)buf[i]);
	}
	debug_send(ctx, xbuf);
}

////////////////////////////////////////////////////////////
// input packet processing

/* pkt is null-terminated */
void 
debug_exec(struct gdbcontext *ctx, const char *pkt) 
{
	char *cs;  /* start of the checksum */
	int i;
	int check = 0, scheck;

#ifdef SHOW_PACKETS
	msg("Got packet %s", pkt);
#endif

	if (pkt[0] != '$') {
		return;
	}

	cs = strchr(pkt, '#');
	if (cs == NULL) {
		return;
	}
	*cs = 0;
	cs++;

	for (i=1; pkt[i]; i++) {
		check += pkt[i];
	}

	scheck = strtoul(cs, NULL, 16);
	if (scheck != check % 256) {
		write(ctx->myfd, "-", 1);
		return;
	} else {
		write(ctx->myfd, "+", 1);
	}

	switch (pkt[1]) {
	    case '!':
		/*
		 * Enable extended mode -- extended mode is apparently
		 * where gdb can ask to rerun the program. We can't do
		 * that... although it could probably be arranged.
		 *
		 * Reply with "OK" if extended mode is enabled.
		 */
		debug_notsupp(ctx);
		break;
	    case '?':
		/* Report why the target stopped. */
		debug_send_stopinfo(ctx);
		break;
	    case 'A':
		/*
		 * Set argv[]. We can't do this, although it might make
		 * sense if we grow support for reloading the kernel.
		 */
		debug_notsupp(ctx);
		break;
	    case 'b':
		if (pkt[2] == 0) {
			/* Set serial bit rate; deprecated. */
			debug_notsupp(ctx);
			break;
		}
		else if (pkt[2] == 'c') {
			/* Backward continue - resume executing, backwards */
		}
		else if (pkt[2] == 's') {
			/* Backward single step - execute one insn backwards */
		}
		else {
			/* ? */
		}
		debug_notsupp(ctx);
		break;
	    case 'B':
		/* Set or clear breakpoint; old, deprecated. */
		debug_notsupp(ctx);
		break;
	    case 'c':
		/*
		 * Continue. If an address follows the 'c', continue
		 * from that address. debug_restart() does that.
		 */
		debug_restart(ctx, pkt + 2);
		unset_breakcond();
		break;
	    case 'C':
		/*
		 * Continue with signal. A signal number in hex
		 * follows the C; that may be followed with a
		 * semicolon and an address to continue at. We don't
		 * have signals per se; in theory we could fake up
		 * some mapping of signals to hardware traps, but that
		 * would be difficult to arrange and not serve much
		 * purpose.
		 */
		debug_notsupp(ctx);
		break;
	    case 'd':
		/* Toggle debug flag (whatever that means); deprecated. */
		debug_notsupp(ctx);
		break;
	    case 'D':
		/*
		 * Detach. With a process-id, detach only one process,
		 * if the multiprocess extension is in use.
		 */
		debug_send(ctx, "OK");
		unset_breakcond();
		break;
	    case 'F':
		/*
		 * File I/O extension reply; for now at least we have
		 * no use for this.
		 */
		debug_notsupp(ctx);
		break;
	    case 'g':
		/* Read general-purpose registers */
		debug_register_print(ctx);
		break;
	    case 'G':
		/*
		 * Write general-purpose registers. The G is followed
		 * by a register dump in the same format as the 'g'
		 * reply.
		 */
		// XXX gcc docs say this is required
		debug_notsupp(ctx);
		break;
	    case 'H':
		/*
		 * Hc followed by a thread ID sets the thread that
		 * step and continue operations should affect; Hg
		 * followed by a thread ID sets the thread that
		 * other operations should affect.
		 */
		if (pkt[2] == 'c') {
			debug_notsupp(ctx);
		}
		else if (pkt[2] == 'g') {
			unsigned cpunum;

			cpunum = getthreadid(pkt+3);
			if (cpunum >= cpu_numcpus()) {
				debug_send(ctx, "E00");
				break;
			}
			debug_cpu = cpunum;
			debug_send(ctx, "OK");
		}
		else {
			debug_send(ctx, "OK");
		}
		break;
	    case 'i':
		/* Step by cycle count */
		debug_notsupp(ctx);
		break;
	    case 'I':
		/* Step by cycle count with signal (apparently) */
		debug_notsupp(ctx);
		break;
	    case 'k':
		/* Kill the target */
		// don't do this - debugger hangs up and we get SIGPIPE
		//debug_send(ctx, "OK");
		msg("Debugger requested kill");
		reqdie();
		// To continue running instead of dying, do this instead
		//unset_breakcond();
		break;
	    case 'm':
		/* Read target memory */
		debug_read_mem(ctx, pkt + 2);
		break;
	    case 'M':
		/* Write target memory */
		debug_write_mem(ctx, pkt + 2);
		break;
	    case 'p':
		/* read one register */
		debug_notsupp(ctx);
		break;
	    case 'P':
		/* write one register */
		debug_notsupp(ctx);
		break;
	    case 'q':
		/* General query */
		if (strcmp(pkt + 2, "C") == 0) {
			/* Return current thread id */
			debug_sendf(ctx,"QC%x", mkthreadid(debug_cpu));
		}
		else if (!strcmp(pkt+2, "fThreadInfo")) {
			char buf[128];
			unsigned i;

			strcpy(buf, "m");
			for (i=0; i<cpu_numcpus(); i++) {
				if (!cpu_enabled(i)) {
					continue;
				}
				if (i > 0) {
					strcat(buf, ",");
				}
				printbyte(buf, sizeof(buf), mkthreadid(i));
			}
			debug_send(ctx, buf);
		}
		else if (!strcmp(pkt+2, "sThreadInfo")) {
			debug_send(ctx, "l");
		}
		else if (strcmp(pkt+2, "Offsets") == 0) {
			/* Return info about load-time relocations */ 
			debug_notsupp(ctx);
		}
		else if (strcmp(pkt+2, "Supported") == 0) {
			/* Features handshake */
			debug_notsupp(ctx);
		}
		else if (!strncmp(pkt+2, "ThreadExtraInfo,", 16)) {
			debug_getthreadinfo(ctx, pkt+2+16);
		}
		else {
			debug_notsupp(ctx);
		}
		break;
	    case 'Q':
		/* General set mode (I think) */
		debug_notsupp(ctx);
		break;
	    case 'r':
		/* Reset target - deprecated */
		debug_notsupp(ctx);
		break;
	    case 'R':
		/*
		 * Restart target (only with extended mode enabled).
		 * Do not reply.
		 */
		break;
	    case 's':
		/* Single step */
		debug_restart(ctx, pkt + 2);
		onecycle();
		debug_send_stopinfo(ctx);
		break;
	    case 'S':
		/* Single step with signal */
		debug_notsupp(ctx);
		break;
	    case 't':
		/* search memory for a pattern (underdocumented) */
		debug_notsupp(ctx);
		break;
	    case 'T':
		/* check if a thread is alive (OK - yes; E.. - no) */
		debug_checkthread(ctx, pkt + 2);
		break;
	    case 'v':
		/* various longer commands */
		debug_notsupp(ctx);
		break;
	    case 'X':
		/* Write target memory with a more efficient encoding */
		debug_notsupp(ctx);
		break;
	    case 'z':
	    case 'Z':
		/* insert (Z) or remove (z) one of the following: */
		switch (pkt[2]) {
		    case '0':
			/* set or clear memory breakpoint */
			debug_notsupp(ctx);
			break;
		    case '1':
			/* set or clear hardware breakpoint */
			debug_notsupp(ctx);
			break;
		    case '2':
			/* set or clear write watchpoint */
			debug_notsupp(ctx);
			break;
		    case '3':
			/* set or clear read watchpoint */
			debug_notsupp(ctx);
			break;
		    case '4':
			/* set or clear access watchpoint */
			debug_notsupp(ctx);
			break;
		    default:
			/* ? */
			debug_notsupp(ctx);
			break;
		}
		break;
	    default:
		debug_notsupp(ctx);
		break;
	}
}
