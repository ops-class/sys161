#ifndef PROF_H
#define PROF_H

/* call while loading the kernel image */
void prof_addtext(uint32_t textbase, uint32_t textsize);

/* call after loading the kernel image to turn on profiling */
void prof_setup(void);

/* call on exit (or whenever, actually) to write gmon.out */
void prof_write(void);

/* call from cpu code when a function-call instruction is reached */
void prof_call(uint32_t frompc, uint32_t topc);

/* call from ltrace to manipulate profiling state */
void prof_enable(void);
void prof_disable(void);
int prof_isenabled(void);
void prof_clear(void);

#endif /* PROF_H */
