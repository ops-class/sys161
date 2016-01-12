#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "console.h"
#include "onsel.h"
#include "cpu.h"
#include "gdb.h"
#include "main.h"

#include "context.h"
//#include "lamebus.h"


static int g_listenfd = -1;
struct gdbcontext g_ctx;
int g_ctx_inuse = 0;

static int dontwait;

void
gdb_dontwait(void)
{
	dontwait = 1;
}

void
gdb_dumpstate(void)
{
	struct sockaddr_un su;
	struct sockaddr_in si;
	socklen_t len;

	msgl("gdb support: %sactive, ", g_ctx_inuse ? "" : "not ");

	if (g_listenfd < 0) {
		msg("not listening");
		return;
	}

	msgl("listening at ");

	len = sizeof(su);
	if (getsockname(g_listenfd, (struct sockaddr *)&su, &len)<0) {
		msg("[error: %s]", strerror(errno));
		return;
	}

	if (su.sun_family == AF_UNIX) {
		len -= (sizeof(su) - sizeof(su.sun_path));
		msg("%.*s", (int) len, su.sun_path);
		return;
	}
#if defined(AF_LOCAL) && AF_LOCAL!=AF_UNIX  /* just in case */
	if (su.sun_family == AF_LOCAL) {
		msg("%s", su.sun_path);
		return;
	}
#endif

	if (su.sun_family != AF_INET) {
		msg("[unknown address family %d]", su.sun_family);
		return;
	}

	len = sizeof(si);
	getsockname(g_listenfd, (struct sockaddr *)&si, &len);
	if (si.sin_addr.s_addr == INADDR_ANY) {
		msgl("* ");
	}
	else {
		msgl("%s ", inet_ntoa(si.sin_addr));
	}
	msg("port %d", ntohs(si.sin_port));
}

int
gdb_canhandle(uint32_t pcaddr)
{
	uint32_t start, end;

	if (g_listenfd < 0) {
		/* Remote debugging support not initialized */
		return 0;
	}

	/*
	 * Note that if g_ctx.myfd isn't open we still do builtin
	 * debugging - in that case we wait for a debugger connection.
	 *
	 * However, if dontwait has been set, and a debugger isn't
	 * connected, return false here so that the cpu's own
	 * breakpoint trap can fire.
	 */

	if (!g_ctx_inuse && dontwait) {
		return 0;
	}

	cpudebug_get_bp_region(&start, &end);

	return (pcaddr >= start && pcaddr < end);
}

static
void
gdb_cleanup(void *x)
{
	struct gdbcontext *ctx = x;
	Assert(ctx->myfd < 0);
	Assert(ctx==&g_ctx);

	g_ctx_inuse = 0;
}

static
int
gdb_receive(void *x)
{
	struct gdbcontext *ctx = x;
	int nread;

	size_t offset;
	char *packet;
	size_t packetlen;
	size_t maxpacklen;
	char *hash;
	size_t hashpos;
	size_t usedlen;

	char tmpbuf[sizeof(ctx->buf)+1];

	if (ctx->bufptr >= sizeof(ctx->buf)) {
		msg("gdbcomm: Input buffer overflow");
		ctx->bufptr = 0;
	}

	nread = read(ctx->myfd, ctx->buf + ctx->bufptr,
		     sizeof(ctx->buf) - ctx->bufptr);

	if (nread <= 0) {
		if (nread < 0) {
			msg("gdbcomm: read: %s", strerror(errno));
		}
		else {
			msg("gdbcomm: read: EOF from debugger");
		}
		main_leave_debugger();
		close(ctx->myfd);
		ctx->myfd = -1;
		return -1;
	}

	ctx->bufptr += nread;

	while ((packet = memchr(ctx->buf, '$', ctx->bufptr)) != NULL) {

		offset = packet - ctx->buf;
		Assert(offset < ctx->bufptr);
		maxpacklen = ctx->bufptr - offset;

		hash = memchr(packet, '#', maxpacklen);
		if (hash==NULL) {
			/* incomplete packet - stop until we get the rest */
			break;
		}

		/*
		 * There are two additional check characters after the $.
		 *    $....#aa
		 *
		 * packetlen is 3 more than hashpos, not 2, because
		 * hashpos, as a length, would not include the hash,
		 * and we need to include both the hash and both check
		 * characters.
		 */
		hashpos = hash  - packet;
		packetlen = hashpos+3;
		if (packetlen > maxpacklen) {
			/* incomplete packet, come back later */
			break;
		}

		/* 
		 * At this point, we have a packet, and it goes from
		 * 'packet' to 'packet+packetlen'.
		 */

		Assert(packetlen+1 < sizeof(tmpbuf));

		/* Copy so we can null-terminate without trashing anything. */
		memmove(tmpbuf, packet, packetlen);
		tmpbuf[packetlen] = 0;

		/* Process it. */
		debug_exec(ctx, tmpbuf);

		/* Keep only the part of the buffer we haven't used yet. */
		usedlen = offset+packetlen;
		Assert(usedlen <= ctx->bufptr);

		ctx->bufptr -= usedlen;
		memmove(ctx->buf, ctx->buf+usedlen, ctx->bufptr);

		/* Loop back and check for another packet */
	}

	return 0;
}

////////////////////////////////////////////////////////////
//
// Setup code

static
int
accepter(void *x)
{
	struct gdbcontext *ctx;
	struct sockaddr_storage sa;
	socklen_t salen;
	int remotefd;

	(void)x; /* not used */

	salen = sizeof(sa);
	remotefd = accept(g_listenfd, (struct sockaddr *)&sa, &salen);
	if (remotefd < 0) {
		/* :-? */
		return 0;
	}

	if (g_ctx_inuse) {
		/*
		 * Hardcode the checksum; the code for sending these things
		 * is file-static in gdb_be.c. :-|
		 *
		 * It doesn't appear to make much difference if we
		 * send anything anyway.
		 */
		const char *errmsg = "$E99#b7";
		write(remotefd, errmsg, strlen(errmsg));
		close(remotefd);
		return 0;
	}

	g_ctx_inuse = 1;
	ctx = &g_ctx;
	msg("New debugger connection");

	ctx->myfd = remotefd;
	ctx->bufptr = 0;

	onselect(remotefd, ctx, gdb_receive, gdb_cleanup);

	cpu_stopcycling();
	main_enter_debugger(0 /* not lethal */);

	return 0;
}

static
int
setup_inet(int port)
{
	int sfd, one=1;
	struct sockaddr_in sn;
	
	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0) {
		msg("socket: %s", strerror(errno));
		return -1;
	}

	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, 
		   (void *)&one, sizeof(one));

	memset(&sn, 0, sizeof(sn));
	sn.sin_family = AF_INET;
	sn.sin_addr.s_addr = INADDR_ANY;
	sn.sin_port = htons(port);

	if (bind(sfd, (struct sockaddr *) &sn, sizeof(sn)) < 0) {
		msg("bind: %s", strerror(errno));
		return -1;
	}

	return sfd;
}

static
int
setup_unix(const char *name)
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
	return sfd;
}

static
void
common_init(int sfd)
{
	if (sfd == -1) {
		msg("Could not bind debug socket; debugging disabled");
		return;
	}

	if (listen(sfd, 1) < 0) {
		msg("listen: %s", strerror(errno));
		msg("Could not set up debug socket; debugging disabled");
		close(sfd);
		return;
	}

	g_listenfd = sfd;
	onselect(sfd, NULL, accepter, NULL);
}

void
gdb_inet_init(int port)
{
	int sfd;
	sfd = setup_inet(port);
	common_init(sfd);
}

void
gdb_unix_init(const char *pathname)
{
	int sfd;
	sfd = setup_unix(pathname);
	common_init(sfd);
}
