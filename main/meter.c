#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "config.h"

#include "speed.h"
#include "clock.h"
#include "console.h"
#include "onsel.h"
#include "main.h" /* for g_stats */
#include "meter.h"

#define PROTOCOL_VERSION  2

////////////////////////////////////////////////////////////

#define METER_BUFSIZE 128

struct meter {
	uint64_t interval;
	int fd;
	char buf[METER_BUFSIZE];
	size_t bufpos;
};

static int meter_socket = -1;

static
PF(2, 3)
void
meter_say(struct meter *m, const char *fmt, ...)
{
	char buf[4096];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	write(m->fd, buf, strlen(buf));
}

static
void
meter_hello(struct meter *m)
{
	meter_say(m, "HELLO %d\r\n", PROTOCOL_VERSION);
}

static
void
meter_header(struct meter *m)
{
	meter_say(m, "HEAD nsec kinsns uinsns udud idle"
		  " irqs exns disk con emu net\r\n");
	meter_say(m, "WIDTH 11 9 9 4 9 4 4 4 5 4 4\r\n");
}

static
void
meter_report(struct meter *m)
{
	char buf2[512];
	uint64_t timestamp;
	uint64_t kcycles, ucycles, icycles;
	uint64_t kretired, uretired;

	timestamp = clock_monotime();

#if 0
	kcycles = g_stats.s_kcycles;
	ucycles = g_stats.s_ucycles;
	icycles = g_stats.s_icycles;
#else
	unsigned i;

	/*
	 * XXX: we ought to send per-cpu stats. The protocol might
	 * need some revising to do it well though. For now we'll
	 * send stats comparable to the uniprocessor sys161.
	 *
	 * Update 20130530: that didn't really produce sensible stats
	 * in some ways, especially for unbalanced workloads, so just
	 * send totals across all cpus.
	 */
	kcycles = ucycles = icycles = 0;
	kretired = uretired = 0;
	for (i=0; i<g_stats.s_numcpus; i++) {
		kcycles += g_stats.s_percpu[i].sp_kcycles;
		ucycles += g_stats.s_percpu[i].sp_ucycles;
		icycles += g_stats.s_percpu[i].sp_icycles;
		kretired += g_stats.s_percpu[i].sp_kretired;
		uretired += g_stats.s_percpu[i].sp_uretired;
		icycles += g_stats.s_tot_icycles;
	}
	/* this will have to change if we ever implement dual-issue :-) */
	Assert(kretired <= kcycles);
	Assert(uretired <= ucycles);
#endif

	snprintf(buf2, sizeof(buf2), "%llu %llu %llu %llu",
		 (unsigned long long) kretired,
		 (unsigned long long) uretired,
		 (unsigned long long) (ucycles - uretired),
		 (unsigned long long) icycles);

	meter_say(m, "DATA %llu %s %lu %lu %lu %lu %lu %lu\r\n",
		 (unsigned long long) timestamp,
		 buf2,
		 (unsigned long) g_stats.s_irqs,
		 (unsigned long) g_stats.s_exns,
		 (unsigned long) (g_stats.s_rsects + g_stats.s_wsects),
		 (unsigned long) (g_stats.s_rchars + g_stats.s_wchars),
		 (unsigned long) (g_stats.s_remu + g_stats.s_wemu + 
				  g_stats.s_memu),
		 (unsigned long) (g_stats.s_rpkts + g_stats.s_wpkts));
}

static
void
meter_update(void *x, uint32_t junk)
{
	struct meter *m = x;

	(void)junk;

	if (m->fd < 0) {
		free(m);
		return;
	}

	meter_report(m);
	schedule_event(m->interval, m, 0, meter_update, "perfmeter");
}

static
void
processline(struct meter *m, char *line)
{
#define MAXWORDS 8
	char *words[MAXWORDS];
	char *s, *ctx;
	unsigned long newinterval;
	int nwords;

	nwords = 0;
	for (s = strtok_r(line, " \t\r\n", &ctx);
	     s != NULL;
	     s = strtok_r(NULL, " \t\r\n", &ctx)) {
		if (nwords >= MAXWORDS) {
			meter_say(m, "BAD Too many words in command\r\n");
			return;
		}
		words[nwords++] = s;
	}
	if (nwords==0) {
		return;
	}

	if (!strcasecmp(words[0], "interval") && nwords == 2) {
		/*
		 * Note that we could lift the limit of 2s max by
		 * using strtoull here, but reliable availability of
		 * strtoull on vendor Unix platforms (e.g. Solaris,
		 * Illumos) is still spotty.
		 */
		errno = 0;
		newinterval = strtoul(words[1], &s, 0);
		if (errno || *s != 0) {
			meter_say(m, "BAD Invalid number\r\n");
			return;
		}
		if (newinterval < MIN_METER_NSECS) {
			meter_say(m, "BAD Interval too small\r\n");
			return;
		}
		if (newinterval > MAX_METER_NSECS) {
			meter_say(m, "BAD Interval too large\r\n");
			return;
		}
		m->interval = newinterval;
	}
	else {
		meter_say(m, "BAD Invalid command\r\n");
		return;
	}
}

static
int
meter_receive(void *x)
{
	static const char overflowmsg[] = "BAD Input overflow\r\n";

	struct meter *m = x;
	char *s;
	int r;

	if (m->bufpos >= sizeof(m->buf)) {
		/* Input overflow */
		write(m->fd, overflowmsg, strlen(overflowmsg));
		m->bufpos = 0;
	}

	r = read(m->fd, m->buf, sizeof(m->buf) - m->bufpos);
	if (r <= 0) {
		/* error/EOF? close connection; m will be freed next update */
		close(m->fd);
		m->fd = -1;
		return -1;
	}
	m->bufpos += r;

	while ((s = memchr(m->buf, '\n', m->bufpos)) != NULL) {
		*s = 0;
		s++;
		m->bufpos -= (s-m->buf);
		processline(m, m->buf);
		memmove(m->buf, s, m->bufpos);
	}

	return 0;
}

////////////////////////////////////////////////////////////
//
// Setup code

static
int
meter_accept(void *x)
{
	struct meter *m;
	struct sockaddr_storage sa;
	socklen_t salen;
	int remotefd;

	(void)x; /* not used */

	salen = sizeof(sa);
	remotefd = accept(meter_socket, (struct sockaddr *)&sa, &salen);
	if (remotefd < 0) {
		/* ...? */
		return 0;
	}

	m = malloc(sizeof(struct meter));
	if (m==NULL) {
		msg("malloc failed allocating meter data for new connection");
		write(remotefd, "ERROR\r\n", 7);
		close(remotefd);
		return 0;
	}

	m->interval = DEFAULT_METER_NSECS;
	m->fd = remotefd;
	m->bufpos = 0;
	onselect(remotefd, m, meter_receive, NULL);

	meter_hello(m);
	meter_header(m);
	meter_update(m, 0);

	return 0;
}

static
int
meter_listen(const char *name)
{
	struct sockaddr_un su;
	socklen_t len;
	int sfd;

	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sfd < 0) {
		msg("socket: %s", strerror(errno));
		return -1;
	}

	memset(&su, 0, sizeof(su));
	su.sun_family = AF_UNIX;
	snprintf(su.sun_path, sizeof(su.sun_path), "%s", name);
	len = SUN_LEN(&su);
#ifdef HAS_SUN_LEN
	su.sun_len = len;
#endif

	if (bind(sfd, (struct sockaddr *) &su, len) < 0) {
		msg("bind: %s", strerror(errno));
		close(sfd);
		return -1;
	}

	if (listen(sfd, 2) < 0) {
		msg("listen: %s", strerror(errno));
		close(sfd);
		return -1;
	}

	return sfd;
}

void
meter_init(const char *pathname)
{
	int sfd;
	sfd = meter_listen(pathname);
	if (sfd < 0) {
		msg("Could not set up meter socket; metering disabled");
		return;
	}

	meter_socket = sfd;
	onselect(meter_socket, NULL, meter_accept, NULL);
}
