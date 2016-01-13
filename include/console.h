#ifndef CONSOLE_H
#define CONSOLE_H

/* Tell GCC to check printf formats. */
#ifdef __GNUC__
#define PF(a,b) __attribute__((__format__(__printf__, a, b)))
#define DEAD  __attribute__((__noreturn__))
#else
#define PF(a,b)
#define DEAD
#endif

void console_earlyinit(void);
void console_init(int pass_sigs);
void console_cleanup(void);

void console_beep(void);
void console_putc(int ch);
void console_onkey(void *, void (*func)(void *, int));

DEAD void die(void);                       /* for config/user/runtime errors */
DEAD void crashdie(void);                  /* for software errors */
DEAD void reqdie(void);                    /* for explicit exit requests */
PF(1,2) void msg(const char *fmt, ...);    /* general messages */
PF(1,2) void msgl(const char *fmt, ...);   /* msg w/o newline */
DEAD PF(1,2) void smoke(const char *fmt, ...); /* for internal errors */
PF(1,2) void hang(const char *fmt, ...);   /* for errors programming the hw */

void console_pause(void);


#define Assert(x) ((x) ? (void)0 : \
        smoke("Assertion failed: %s, line %d of %s", #x, __LINE__, __FILE__))

#if 0
#ifdef USE_DEBUG
#define DEBUG(args) msg args
#define DEBUGL(args) msgl args
#define PAUSE() console_pause()
#else
#define DEBUG(args)
#define DEBUGL(args)
#define PAUSE()
#endif
#endif

#endif /* CONSOLE_H */
