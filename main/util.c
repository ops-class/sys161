#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "console.h"
#include "util.h"


void *
domalloc(size_t len)
{
	void *x = malloc(len);
	if (!x) {
		smoke("Out of memory");
	}
	return x;
}

void
dohexdump(const char *buf, size_t len)
{
	static char zeros[16];
	size_t x, i;
	int c;
	int skipping, saidanything;

	memset(zeros, 0, sizeof(zeros));
	skipping = 0;
	saidanything = 0;

	for (x=0; x<len; x += 16) {
		if (!memcmp(buf+x, zeros, 16) && saidanything) {
			if (!skipping) {
				msg("       *");
			}
			skipping = 1;
			continue;
		}
		skipping = 0;
		saidanything = 1;
		msgl("%6lx:", (unsigned long) x);
		for (i=0; i<16 && x+i<len; i++) {
			msgl("%02x ", (unsigned)(unsigned char)buf[x+i]);
		}
		for (i=0; i<16 && x+i<len; i++) {
			c = buf[x+i];
			if (!isprint(c) || !isascii(c)) {
				c = '.';
			}
			msgl("%c", c);
		}
		/* gcc warns if we just do msg("") */
		msg("%s", "");
	}
	msg("%6lx:", (unsigned long) x);
}

/* XXX this shouldn't be here. */
#define SECTORSIZE 512

off_t
getsize(const char *str)
{
	char *suffix;
	off_t value;

	errno = 0;
	value = strtoll(str, &suffix, 0);
	if (errno) {
		msg("%s: Invalid number", str);
		die();
	}
	if (*suffix == 0 || !strcmp(suffix, "b")) {
		/* bytes */
		return value;
	}
	if (!strcmp(suffix, "s")) {
		/* sectors */
		return value * SECTORSIZE;
	}
	if (!strcmp(suffix, "k") || !strcmp(suffix, "K")) {
		/* Kb */
		return value*1024;
	}
	if (!strcmp(suffix, "m") || !strcmp(suffix, "M")) {
		/* Mb */
		return value*1024*1024;
	}
	if (!strcmp(suffix, "g") || !strcmp(suffix, "G")) {
		/* Gb */
		return value*1024*1024*1024;
	}
	msg("%s: Invalid size suffix %s\n", str, suffix);
	die();
}
