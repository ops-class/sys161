#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "console.h"

#include "lamebus.h"
#include "busids.h"

/*
 * Note. We assume that random() ^ (random() << 16) fills a uint32_t with
 * 32 good random bits. random() is a BSD function that appears to be
 * defined to return values in the range 0 to 2^31-1, no matter what
 * RAND_MAX may be. The configure script should probably attempt to check
 * the random generator. (FUTURE)
 */


static
void *
rand_init(int slot, int argc, char *argv[])
{
	uint32_t seed = 0;
	int i;

	for (i=1; i<argc; i++) {
		if (!strncmp(argv[i], "seed=", 5)) {
			seed = atol(argv[i]+5);
		}
		else if (!strcmp(argv[i], "autoseed")) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			seed = tv.tv_sec ^ (tv.tv_usec << 8);
		}
		else {
			msg("random: slot %d: invalid option %s",
			    slot, argv[i]);
			die();
		}
	}

#ifdef __OpenBSD__
	/*
	 * In 2014 OpenBSD decided to arbitrarily change the behavior
	 * of srandom() to ignore the seed argument, making it not
	 * work. You have to call srandom_deterministic() instead.
	 */
	srandom_deterministic(seed);
#else
	srandom(seed);
#endif

	return NULL;
}

static
int
rand_fetch(unsigned cpunum, void *data, uint32_t offset, uint32_t *ret)
{
	(void)cpunum; // not used
	(void)data;   // not used

	if (offset==0) {
		*ret = random() ^ (random() << 16);
		return 0;
	}
	return -1;
}

static
int
rand_store(unsigned cpunum, void *data, uint32_t offset, uint32_t val)
{
	(void)cpunum;
	(void)data;
	(void)offset;
	(void)val;
	return -1;
}

static
void
rand_dumpstate(void *data)
{
	(void)data;
	msg("System/161 random generator rev %d", RANDOM_REVISION);
	msg("    (randomizer state not readily available)");
}

static
void
rand_cleanup(void *data)
{
	(void)data;
}

const struct lamebus_device_info random_device_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_RANDOM,
	RANDOM_REVISION,
	rand_init,
	rand_fetch,
	rand_store,
	rand_dumpstate,
	rand_cleanup,
};
