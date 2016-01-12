/*
 * disk161
 *
 * Disk image tool for sys161.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/*
 * On Linux, or at least in some glibc versions, fcntl.h defines the
 * constants for flock(), but not the function, which is in sys/file.h
 * with a separate copy of the constants.
 */
#if defined(__linux__)
#include <sys/file.h>
#endif

#define SECTORSIZE 512
#define MINSIZE (128 * SECTORSIZE)
#define MAXSIZE 0x100000000LL

#define HEADERSIZE   SECTORSIZE
#define HEADERSTRING "System/161 Disk Image"

////////////////////////////////////////////////////////////
// Compat
/* XXX two copies of this are pasted here and in dev_disk.c */

/*
 * Apparently (as of September 2014) Solaris still doesn't have
 * flock(). What is this, 1990? Provide a wrapper around fcntl locks.
 *
 * If you don't have fcntl locks either, you're out of luck.
 */
#ifndef LOCK_EX

/*      LOCK_SH F_RDLCK - we don't use this */
#define LOCK_EX F_WRLCK
#define LOCK_UN F_UNLCK
#define LOCK_NB 0  /* assume we always want this */

#define flock(fd, op) myflock(fd, op)

static
int
myflock(int fd, int op)
{
	struct flock glop;

	glop.l_start = 0;
	glop.l_len = 0;
	glop.l_pid = getpid();
	glop.l_type = op;
	glop.l_whence = SEEK_SET;
	return fcntl(fd, F_SETLK, &glop);
}

#endif /* LOCK_EX */

////////////////////////////////////////////////////////////
// error wrappers

static
int
doopen(const char *file, int flags, mode_t mode)
{
	int fd;

	fd = open(file, flags, mode);
	if (fd < 0) {
		fprintf(stderr, "disk161: %s: %s\n",
			file, strerror(errno));
		exit(1);
	}

	return fd;
}

static
void
doread(const char *file, int fd, void *buf, size_t len)
{
	ssize_t r;

	r = read(fd, buf, len);
	if (r < 0) {
		fprintf(stderr, "disk161: %s: read: %s\n", file,
			strerror(errno));
		exit(1);
	}
	if ((size_t)r != len) {
		fprintf(stderr, "disk161: %s: reaD: Unexpected EOF\n", file);
		exit(1);
	}
}

static
void
dowrite(const char *file, int fd, const void *buf, size_t len)
{
	ssize_t r;

	r = write(fd, buf, len);
	if (r < 0) {
		fprintf(stderr, "disk161: %s: write: %s\n", file,
			strerror(errno));
		exit(1);
	}
	if ((size_t)r != len) {
		fprintf(stderr, "disk161: %s: write: Unexpected short count "
			"%zd of %zu\n", file, r, len);
		exit(1);
	}
}

static
void
dolseek(const char *file, int fd, off_t pos, int whence)
{
	if (lseek(fd, pos, whence) == -1) {
		fprintf(stderr, "disk161: %s: lseek: %s\n", file,
			strerror(errno));
	}
}

static
void
dotruncate(const char *file, int fd, off_t size)
{
	if (ftruncate(fd, size) < 0) {
		fprintf(stderr, "disk161: %s: %s\n", file, strerror(errno));
		exit(1);
	}
}

static
void
dofstat(const char *file, int fd, struct stat *st)
{
	if (fstat(fd, st) == -1) {
		fprintf(stderr, "disk161: %s: fstat: %s\n", file,
			strerror(errno));
		exit(1);
	}
}

static
void
doflock(const char *file, int fd, int mode)
{
	if (flock(fd, mode|LOCK_NB) == -1) {
		if (errno == EAGAIN) {
			fprintf(stderr, "disk161: %s: "
				"Locked by another process\n", file);
		}
		else {
			fprintf(stderr, "disk161: %s: flock: %s\n", file,
				strerror(errno));
		}
		exit(1);
	}
}

////////////////////////////////////////////////////////////
// common

static
off_t
getsize(const char *str)
{
	char *suffix;
	off_t value;

	errno = 0;
	value = strtoll(str, &suffix, 0);
	if (errno) {
		fprintf(stderr, "disk161: %s: Invalid number\n", str);
		exit(1);
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
	fprintf(stderr, "disk161: Invalid size suffix %s\n", suffix);
	exit(1);
}

static
off_t
filesize(const char *file, int fd)
{
	struct stat st;

	dofstat(file, fd, &st);
	return st.st_size;
}

static
void
checksize(off_t size)
{
	if (size % SECTORSIZE) {
		fprintf(stderr, "Size %lld not an even number of sectors\n",
			(long long)size);
		size = SECTORSIZE * ((size + SECTORSIZE - 1) / SECTORSIZE);
		fprintf(stderr, "Try %lld instead.\n", (long long)size);
		exit(1);
	}
	if (size < MINSIZE) {
		fprintf(stderr, "Size %lld too small\n", (long long)size);
		exit(1);
	}
	if (size >= MAXSIZE) {
		fprintf(stderr, "Size %lld too large\n", (long long)size);
		exit(1);
	}
}

static
void
checkheader(const char *file, int fd)
{
	char buf[SECTORSIZE];

	doread(file, fd, buf, sizeof(buf));
	buf[sizeof(buf) - 1] = 0;

	if (!strcmp(buf, HEADERSTRING)) {
		/* ok */
		return;
	}
	fprintf(stderr, "disk161: %s: Not a System/161 disk image\n", file);
	exit(1);
}

static
void
writeheader(const char *file, int fd)
{
	char buf[SECTORSIZE];

	memset(buf, 0, sizeof(buf));
	strcpy(buf, HEADERSTRING);

	dolseek(file, fd, 0, SEEK_SET);
	dowrite(file, fd, buf, sizeof(buf));
}

////////////////////////////////////////////////////////////
// create

static
void
docreate(const char *file, const char *sizespec, int doforce)
{
	int fd;
	off_t size;

	if (!doforce) {
		fd = open(file, O_RDONLY);
		if (fd >= 0) {
			fprintf(stderr, "disk161: %s: %s\n", file,
				strerror(EEXIST));
			exit(1);
		}
	}

	fd = doopen(file, O_RDWR|O_CREAT|O_TRUNC, 0664);
	doflock(file, fd, LOCK_EX);
	size = getsize(sizespec);
	checksize(size);
	dotruncate(file, fd, HEADERSIZE + size);
	writeheader(file, fd);
	doflock(file, fd, LOCK_UN);
	close(fd);
}

////////////////////////////////////////////////////////////
// info

static
void
doinfo(const char *file)
{
	int fd;
	struct stat st;
	long long amt;

	fd = doopen(file, O_RDWR, 0);
	checkheader(file, fd);
	dofstat(file, fd, &st);

	amt = st.st_size - HEADERSIZE;
	printf("%s size %lld bytes (%lld sectors; %lldK; %lldM)\n", file,
	       amt, amt / SECTORSIZE, amt / 1024, amt / (1024*1024));
	

	amt = st.st_blocks * 512LL;
	printf("%s spaceused %lld bytes (%lld sectors; %lldK; %lldM)\n", file,
	       amt, amt / SECTORSIZE, amt / 1024, amt / (1024*1024));

	close(fd);
}

////////////////////////////////////////////////////////////
// resize

static
void
doresize(const char *file, const char *sizespec)
{
	enum { M_SET, M_PLUS, M_MINUS } mode;
	off_t oldsize, newsize;
	int fd;

	if (*sizespec == '+') {
		sizespec++;
		mode = M_PLUS;
	}
	else if (*sizespec == '-') {
		sizespec++;
		mode = M_MINUS;
	}
	else {
		mode = M_SET;
	}

	newsize = getsize(sizespec);
	fd = doopen(file, O_RDWR, 0);
	doflock(file, fd, LOCK_EX);
	checkheader(file, fd);
	oldsize = filesize(file, fd);
	oldsize -= HEADERSIZE;
	switch (mode) {
	    case M_SET:
		break;
	    case M_PLUS:
		if (oldsize + newsize < oldsize) {
			/* overflow */
			fprintf(stderr, "+%s: Result too large\n", sizespec);
			exit(1);
		}
		newsize = oldsize + newsize;
		break;
	    case M_MINUS:
		if (oldsize < newsize) {
			/* underflow */
			fprintf(stderr, "-%s: Result too small\n", sizespec);
			exit(1);
		}
		newsize = oldsize - newsize;
		break;
	}

	checksize(newsize);
	dotruncate(file, fd, HEADERSIZE + newsize);
	doflock(file, fd, LOCK_UN);
	close(fd);
}

////////////////////////////////////////////////////////////
// main

static
void
usage(void)
{
	fprintf(stderr, "Usage: disk161 action [options] [arguments]\n");
	fprintf(stderr, "   disk161 create [-f] filename size\n"); 
	fprintf(stderr, "   disk161 info filename...\n");
	fprintf(stderr, "   disk161 resize filename [+-]size\n");
	exit(3);
}

int
main(int argc, char *argv[])
{
	const char *command;
	int doforce = 0;
	int ch;
	int i;

	if (argc < 2) {
		usage();
	}

	command = argv[1];
	argv++;
	argc--;

	while ((ch = getopt(argc, argv, "f"))!=-1) {
		switch (ch) {
		    case 'f': doforce = 1; break;
		    default: usage();
		}
	}

	if (!strcmp(command, "create")) {
		if (optind + 2 != argc) {
			usage();
		}
		docreate(argv[optind], argv[optind+1], doforce);
	}
	else if (!strcmp(command, "info") || !strcmp(command, "stat") ||
		 !strcmp(command, "stats") || !strcmp(command, "status")) {
		if (doforce) {
			usage();
		}
		for (i=optind; i<argc; i++) {
			doinfo(argv[i]);
		}
	}
	else if (!strcmp(command, "resize") || !strcmp(command, "setsize")) {
		if (optind + 2 != argc) {
			usage();
		}
		if (doforce) {
			usage();
		}
		doresize(argv[optind], argv[optind+1]);
	}
	else if (!strcmp(command, "help")) {
		usage();
	}
	else {
		fprintf(stderr, "disk161: Unknown command %s\n", command);
		usage();
	}

	return 0;
}

