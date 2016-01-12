#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
