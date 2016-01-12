#define BUFLEN 1024

struct gdbcontext {
	int myfd;
	char buf[BUFLEN];
	size_t bufptr;
};

void debug_exec(struct gdbcontext *ctx, const char *buf);
