
#include "console.h"   // make sure we have PF()

#ifndef TRACE_H
#define TRACE_H

#ifdef USE_TRACE

#define DOTRACE_KINSN	0	/* trace instructions in kernel mode */
#define DOTRACE_UINSN	1	/* trace instructions in user mode */
#define DOTRACE_JUMP	2	/* trace jump instructions */
#define DOTRACE_TLB     3	/* trace tlb operations */
#define DOTRACE_EXN     4	/* trace exceptions */
#define DOTRACE_IRQ     5	/* trace exceptions */
#define DOTRACE_DISK	6	/* trace disk ops */
#define DOTRACE_NET	7	/* trace net ops */
#define DOTRACE_EMUFS	8	/* trace emufs ops */
#define NDOTRACES	9

extern int g_traceflags[NDOTRACES];

int adjust_traceflag(int flag, int onoff); /* returns -1 on error */
void set_traceflags(const char *letters);
void print_traceflags(void);
void print_traceflags_usage(void);


/*
 * These five functions are actually in console.c.
 *
 * set_tracefile: set output destination
 * hwtrace: issue a trace message from a hardware device (not per-cpu)
 * cputrace: issue a trace message from a CPU
 * hwtracel/cputracel: same but without an implicit newline added
 */

void set_tracefile(const char *filename);
void hwtrace(const char *fmt, ...) PF(1,2);
void hwtracel(const char *fmt, ...) PF(1,2);
void cputrace(unsigned cpunum, const char *fmt, ...) PF(2,3);
void cputracel(unsigned cpunum, const char *fmt, ...) PF(2,3);


#define CPUTRACEL(k, cn, ...) \
	(g_traceflags[(k)] ? cputracel(cn, __VA_ARGS__) : (void)0)
#define CPUTRACE(k, cn, ...)  \
	(g_traceflags[(k)] ? cputrace(cn, __VA_ARGS__) : (void)0)

#define HWTRACEL(k, ...)   (g_traceflags[(k)] ? hwtracel(__VA_ARGS__): (void)0)
#define HWTRACE(k, ...)    (g_traceflags[(k)] ? hwtrace(__VA_ARGS__) : (void)0)



#else /* not USE_TRACE */


#define CPUTRACEL(k, ...)
#define CPUTRACE(k, ...)

#define HWTRACEL(k, ...)
#define HWTRACE(k, ...)


#endif /* USE_TRACE */

#endif /* TRACE_H */
