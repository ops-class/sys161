/*
 * hub161
 *
 * Hub for sys161 network devices.
 *
 * The hub listens on an AF_UNIX datagram socket and redistributes all
 * the packets it receives to all the senders it knows about.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include "config.h"

#include "array.h"

#define DEFAULT_SOCKET  ".sockets/hub"

#define HUB_ADDR        0x0000
#define BROADCAST_ADDR  0xffff
#define FRAME_MAGIC     0xa4b3
#define MAXPACKET       4096

struct linkheader {
	uint16_t lh_frame;
	uint16_t lh_from;
	uint16_t lh_packetlen;
	uint16_t lh_to;
};

struct sender {
	uint16_t sdr_addr;
	struct sockaddr_un sdr_sun;
	socklen_t sdr_len;
	int sdr_errors;
};

////////////////////////////////////////////////////////////

static struct array *senders;
static int sock;

////////////////////////////////////////////////////////////

static
void
checksender(uint16_t addr, struct sockaddr_un *rsun, socklen_t rlen)
{
	int n, i;
	struct sender *sdr;
	int pathlen;

	assert(senders != NULL);
	assert(rsun != NULL);
	assert(addr != BROADCAST_ADDR);

	n = array_getnum(senders);
	for (i=0; i<n; i++) {
		sdr = array_getguy(senders, i);
		assert(sdr != NULL);
		if (sdr->sdr_addr == addr) {
			memcpy(&sdr->sdr_sun, rsun, sizeof(*rsun));
			sdr->sdr_len = rlen;
			return;
		}
	}
	
	sdr = malloc(sizeof(struct sender));
	if (!sdr) {
		fprintf(stderr, "hub161: out of memory\n");
		exit(1);
	}

	pathlen = rlen;
	pathlen = pathlen - (sizeof(*rsun) - sizeof(rsun->sun_path));

	printf("hub161: adding %04x from %.*s\n", addr, pathlen, 
	       rsun->sun_path);
	if (rsun->sun_path[0]!='/') {
		printf("hub161: (not absolute pathname, may not work)\n");
	}

	sdr->sdr_addr = addr;
	memcpy(&sdr->sdr_sun, rsun, sizeof(*rsun));
	sdr->sdr_len = rlen;
	sdr->sdr_errors = 0;

	if (array_add(senders, sdr)) {
		fprintf(stderr, "hub161: Out of memory\n");
		exit(1);
	}
}

static
void
dosend(const char *pkt, size_t len)
{
	struct sender *sdr;
	int r, n, i;

	assert(senders != NULL);
	assert(pkt != NULL);

	n = array_getnum(senders);
	for (i=0; i<n; i++) {
		sdr = array_getguy(senders, i);
		assert(sdr != NULL);
		r = sendto(sock, pkt, len, 0, 
			   (struct sockaddr *)&sdr->sdr_sun,
			   sdr->sdr_len);
		if (r < 0) {
			fprintf(stderr, "hub161: sendto %04x: %s\n",
				sdr->sdr_addr, strerror(errno));
			sdr->sdr_errors++;
		}
	}
}

static
void
killsenders(void)
{
	struct sender *sdr;
	int n, i;

	assert(senders != NULL);

	n = array_getnum(senders);
	for (i=0; i<n; i++) {
		sdr = array_getguy(senders, i);
		assert(sdr != NULL);

		if (sdr->sdr_errors > 5) {
			printf("hub161: dropping %04x\n", sdr->sdr_addr);
			array_remove(senders, i);
			i--;
			n--;
			free(sdr);
		}
	}
}

////////////////////////////////////////////////////////////

static
void
opensock(const char *sockname)
{
	struct sockaddr_un su;
	socklen_t len;
	struct stat st;
	int one=1;

	/*
	 * I know this isn't race-free. It's not meant to be - it's meant
	 * to protect people from accidentally doing "hub161 source.c" and
	 * losing their source file.
	 */

	if (lstat(sockname, &st)==0) {
		if (S_ISSOCK(st.st_mode)) {
			unlink(sockname);
		}
		else {
			fprintf(stderr, "hub161: %s: File exists\n", sockname);
			exit(1);
		}
	}

	sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		fprintf(stderr, "hub161: socket: %s\n", strerror(errno));
		exit(1);
	}

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 
		   (void *)&one, sizeof(one));

	su.sun_family = AF_UNIX;
	strcpy(su.sun_path, sockname);
	len = SUN_LEN(&su);
#ifdef HAS_SUN_LEN
	su.sun_len = len;
#endif

	if (bind(sock, (struct sockaddr *)&su, len) < 0) {
		fprintf(stderr, "hub161: bind: %s\n", strerror(errno));
		exit(1);
	}
}

static
void
closesock(void)
{
	close(sock);
}

////////////////////////////////////////////////////////////

static
void
loop(void)
{
	char packetbuf[MAXPACKET];
	size_t packetlen;
	struct sockaddr_un rsun;
	socklen_t rlen;
	struct linkheader *lh;
	int r;
	
	while (1) {
		rlen = sizeof(rsun);
		r = recvfrom(sock, packetbuf, sizeof(packetbuf), 0,
			     (struct sockaddr *)&rsun, &rlen);
		if (r<0) {
			fprintf(stderr, "hub161: recvfrom: %s\n", 
				strerror(errno));
			continue;
		}
		packetlen = r;

		assert(rlen <= sizeof(rsun));
		assert(rsun.sun_family==AF_UNIX);
		assert(packetlen <= sizeof(packetbuf));
#ifdef HAS_SUN_LEN
		assert(rlen <= rsun.sun_len);
		if (rlen < rsun.sun_len) {
			/*
			 * This means the address (pathname) didn't fit
			 * in the sockaddr.
			 *
			 * Beware: rsun.sun_path isn't necessarily null
			 * terminated, so don't print it without a length
			 * limit.
			 */
			fprintf(stderr, "hub161: packet from too-long "
				"pathname\n");
			continue;
		}
		assert(rlen == rsun.sun_len);
#endif

		if (packetlen < sizeof(struct linkheader)) {
			fprintf(stderr, "hub161: runt packet (size %lu)\n",
				(unsigned long) packetlen);
			continue;
		}

		lh = (struct linkheader *)packetbuf;

		if (ntohs(lh->lh_frame) != FRAME_MAGIC) {
			fprintf(stderr, "hub161: frame error [%04x]\n",
				ntohs(lh->lh_frame));
			continue;
		}

		if ((size_t)ntohs(lh->lh_packetlen) != packetlen) {
			fprintf(stderr, "hub161: bad size [%04x %04lx]\n",
				ntohs(lh->lh_packetlen),
				(unsigned long) packetlen);
			continue;
		}

		if (ntohs(lh->lh_from) == BROADCAST_ADDR) {
			fprintf(stderr, "hub161: packet came from broadcast "
				"addr (dropped)\n");
			continue;
		}

		checksender(ntohs(lh->lh_from), &rsun, rlen);

		if (ntohs(lh->lh_to) == HUB_ADDR) {
			/* to us - don't forward it */
			continue;
		}

		dosend(packetbuf, packetlen);
		killsenders();
	}
}

////////////////////////////////////////////////////////////

static
void
usage(void)
{
	fprintf(stderr, "Usage: hub161 [socketname]\n");
	fprintf(stderr, "    Default socket is %s\n", DEFAULT_SOCKET);
	exit(3);
}

int
main(int argc, char *argv[])
{
	const char *sockname = DEFAULT_SOCKET;
	int ch;

	while ((ch = getopt(argc, argv, ""))!=-1) {
		switch (ch) {
		    default: usage();
		}
	}
	if (optind < argc) {
		sockname = argv[optind++];
	}
	if (optind < argc) {
		usage();
	}

	senders = array_create();
	if (!senders) {
		fprintf(stderr, "hub161: Out of memory\n");
		exit(1);
	}

	opensock(sockname);
	printf("hub161: Listening on %s\n", sockname);
	loop();
	closesock();

	return 0;
}
