#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "console.h"
#include "onsel.h"


#define MAXSELS 64

static struct {
	int sd_fd;
	void *sd_data;
	int (*sd_func)(void *data);
	void (*sd_rfunc)(void *data);
} selections[MAXSELS];
static int nsels=0;

uint64_t extra_selecttime;

static
int
findsel(void)
{
	int i;
	for (i=0; i<nsels; i++) {
		if (selections[i].sd_fd < 0) {
			return i;
		}
	}
	if (nsels < MAXSELS) {
		return nsels++;
	}
	return -1;
}

void
onselect(int fd, void *data, int (*func)(void *), void (*rfunc)(void *))
{
	int ix = findsel();
	if (ix<0) {
		smoke("Ran out of select() records in mainloop");
	}

	selections[ix].sd_fd = fd;
	selections[ix].sd_data = data;
	selections[ix].sd_func = func;
	selections[ix].sd_rfunc = rfunc;
}

void
notonselect(int fd)
{
	int i;

	for (i=0; i<nsels; i++) {
		if (selections[i].sd_fd == fd) {
			if (selections[i].sd_rfunc) {
				selections[i].sd_rfunc(selections[i].sd_data);
			}
			selections[i].sd_fd = -1;
			return;
		}
	}
	smoke("notonselect: fd %d not found", fd);
}

uint64_t
tryselect(int dotimeout, uint64_t nsecs)
{
	int i, r, hifd=-1;
	fd_set myset;
	struct timeval timeout, before, after;
	uint32_t sleptsecs;
	uint64_t sleptnsecs;

	if (dotimeout) {
		timeout.tv_sec = nsecs / 1000000000ULL;
		timeout.tv_usec = (nsecs % 1000000000ULL) / 1000;
	}

	FD_ZERO(&myset);
	for (i=0; i<nsels; i++) {
		int fd = selections[i].sd_fd;
		if (fd<0) continue;
		FD_SET(fd, &myset);
		if (fd > hifd) hifd = fd;
	}

	if (!dotimeout || nsecs > 0) {
		gettimeofday(&before, NULL);
	}
	r = select(hifd+1, &myset, NULL, NULL, dotimeout ? &timeout : NULL);
	if (r < 0) {
		return 0;
	}

	if (!dotimeout || nsecs > 0) {
		gettimeofday(&after, NULL);

		if (after.tv_sec < before.tv_sec ||
		    (after.tv_sec == before.tv_sec &&
		     after.tv_usec < before.tv_usec)) {
			/* just in case */
			sleptnsecs = 0;
		}
		else {
			sleptsecs = after.tv_sec - before.tv_sec;
			sleptnsecs = 1000000000ULL * sleptsecs;
			sleptnsecs += 1000 * after.tv_usec;
			sleptnsecs -= 1000 * before.tv_usec;
		}
	}
	else {
		sleptnsecs = 0;
	}

	if (r == 0) {
		/* nothing to dispatch */
		return sleptnsecs;
	}

	extra_selecttime = sleptnsecs;
	for (i=0; i<nsels; i++) {
		int fd = selections[i].sd_fd;
		if (fd < 0 || !FD_ISSET(fd, &myset)) {
			continue;
		}

		r = selections[i].sd_func(selections[i].sd_data);
		if (r) {
			if (selections[i].sd_rfunc) {
				selections[i].sd_rfunc(selections[i].sd_data);
			}
			selections[i].sd_fd = -1;
		}
	}
	extra_selecttime = 0;

	return sleptnsecs;
}
