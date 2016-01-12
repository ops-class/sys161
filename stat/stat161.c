#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "config.h"

#define MAXFIELDS	16
#define MAXHEADERLEN	16
#define PATH_SOCKET	".sockets/meter"
#define PROTO_VERSION	1

struct field {
	uint64_t lastval;
	unsigned width;
	char header[MAXHEADERLEN];
};

static struct field fields[MAXFIELDS];
static int nfields=0;
static int lines_since_header=100000;

static
void
reset(void)
{
	int i;

	for (i=0; i<MAXFIELDS; i++) {
		fields[i].lastval = 0;
		fields[i].width = 0;
		strcpy(fields[i].header, "??");
	}
	nfields = 0;
	lines_since_header = 10000;
}

static
uint64_t
getval(const char *s)
{
	/* We could (should?) use strtoull, but it's not always available. */
	uint64_t val = 0;
	unsigned i;

	for (i=0; s[i]; i++) {
		if (s[i]>='0' && s[i]<='9') {
			val = val*10 + (s[i]-'0');
		}
		else {
			break;
		}
	}
	return val;
}

static
void
setheaders(int nwords, char **words)
{
	int i;

	if (nwords > MAXFIELDS) {
		static int warned=0;
		if (!warned) {
			fprintf(stderr, "stat161: Too many data fields; "
				"increase MAXFIELDS and recompile\n");
			warned = 1;
		}
		nwords = MAXFIELDS;
	}

	for (i=0; i<nwords; i++) {
		if (strlen(words[i]) >= MAXHEADERLEN) {
			words[i][MAXHEADERLEN-1] = 0;
		}
		if (strlen(words[i]) > fields[i].width) {
			fields[i].width = strlen(words[i]);
		}
		strcpy(fields[i].header, words[i]);
	}

	nfields = nwords;
}

static
void
setwidths(int nwords, char **words)
{
	int i;
	for (i=0; i<nwords; i++) {
		unsigned width = atoi(words[i]);
		if (i < MAXFIELDS) {
			if (width > fields[i].width) {
				fields[i].width = width;
			}
		}
	}
}

static
void
showdata(int nwords, char **words)
{
	uint64_t val, printval;
	int i;
	char tmp[128];

	if (nwords != nfields) {
		printf("stat161: Invalid packet (wrong number of fields)\n");
		return;
	}

	if (lines_since_header > 21) {
		printf("\n");
		for (i=0; i<nfields; i++) {
			printf("%-*s ", fields[i].width, fields[i].header);
		}
		printf("\n");
		lines_since_header = 0;
	}

	for (i=0; i<nwords; i++) {
		val = getval(words[i]);
		printval = val - fields[i].lastval;
		fields[i].lastval = val;

		if (sizeof(uint64_t)==sizeof(unsigned long)) {
			snprintf(tmp, sizeof(tmp), "%lu", 
				 (unsigned long) printval);
		}
		else {
			snprintf(tmp, sizeof(tmp), "%llu", 
				 (unsigned long long) printval);
		}

		if (strlen(tmp) > fields[i].width) {
			fields[i].width = strlen(tmp);
		}

		printf("%-*s ", fields[i].width, tmp);
	}
	printf("\n");
	lines_since_header++;
}

static
void
processline(char *line)
{
	char *words[32];
	char *s;
	int nwords;

	nwords = 0;
	for (s = strtok(line, " \t\r\n"); s; s = strtok(NULL, " \t\r\n")) {
		if (nwords < 32) {
			words[nwords++] = s;
		}
	}

	if (nwords==0) {
		return;
	}
	if (!strcasecmp(words[0], "error")) {
		/* no actual useful messages */
		return;
	}
	else if (!strcasecmp(words[0], "hello") && nwords==2) {
		int ver = atoi(words[1]);
		if (ver != PROTO_VERSION) {
			fprintf(stderr, "stat161: Wrong protocol version %d\n",
				ver);
			exit(1);
		}
	}
	else if (!strcasecmp(words[0], "head") && nwords>1) {
		setheaders(nwords-1, words+1);
	}
	else if (!strcasecmp(words[0], "width") && nwords>1) {
		setwidths(nwords-1, words+1);
	}
	else if (!strcasecmp(words[0], "data") && nwords>1) {
		showdata(nwords-1, words+1);
	}
	else {
		printf("stat161: Invalid packet (improper header)\n");
	}
}

static
void
dometer(int s)
{
	char buf[4096], *x;
	size_t bufpos=0;
	int r;

	while (1) {
		assert(bufpos<=sizeof(buf));
		if (bufpos==sizeof(buf)) {
			fprintf(stderr, "stat161: Input overrun\n");
			bufpos = 0;
		}
		r = read(s, buf+bufpos, sizeof(buf)-bufpos);
		if (r<0) {
			fprintf(stderr, "stat161: read: %s\n",
				strerror(errno));
			return;
		}
		else if (r==0) {
			return;
		}

		bufpos += r;
		
		while ((x = memchr(buf, '\n', bufpos))!=NULL) {
			*x = 0;
			x++;
			bufpos -= (x-buf);
			processline(buf);
			memmove(buf, x, bufpos);
		}
	}
}

static
int
opensock(void)
{
	struct sockaddr_un su;
	socklen_t len;
	int s;

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s<0) {
		fprintf(stderr, "stat161: socket: %s\n", strerror(errno));
		exit(1);
	}

	memset(&su, 0, sizeof(su));
	su.sun_family = AF_UNIX;
	snprintf(su.sun_path, sizeof(su.sun_path), "%s", PATH_SOCKET);
	len = SUN_LEN(&su);
#ifdef HAS_SUN_LEN
	su.sun_len = len;
#endif

	if (connect(s, (struct sockaddr *)&su, len)<0) {
		if (errno!=ECONNREFUSED && errno!=ENOENT) {
			fprintf(stderr, "stat161: connect: %s\n", 
				strerror(errno));
			sleep(5);
		}
		else {
			sleep(1);
		}
		close(s);
		return -1;
	}

	return s;
}

static
void
loop(void)
{
	int s;

	printf("stat161: Connecting...\n");
	while (1) {
		s = opensock();
		if (s>=0) {
			printf("stat161: Connected.\n");
			reset();
			dometer(s);
			close(s);
			printf("stat161: Disconnected.\n");
		}
	}
}

int
main(void)
{
	signal(SIGPIPE, SIG_IGN);
	loop();
	return 0;
}
