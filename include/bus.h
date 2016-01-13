#ifndef BUS_H
#define BUS_H

/*
 * Addresses are relative to the start of RAM pretending it's contiguous,
 * or relative to the start of I/O space. We split things up this way
 * because the actual memory layout is machine-dependent.
 *
 * The _mem_ functions are now kept in inlinemem.h
 */

//int bus_mem_fetch(uint32_t addr, uint32_t *);
//int bus_mem_store(uint32_t addr, uint32_t);
int bus_io_fetch(unsigned cpunum, uint32_t addr, uint32_t *);
int bus_io_store(unsigned cpunum, uint32_t addr, uint32_t);

/*
 * Set up bus and cards in bus. Returns number of CPUs to pass to cpu_init.
 */
unsigned bus_config(const char *configfile,
		    const char *configextra[], unsigned numconfigextra);

/*
 * Clean up bus in preparation for exit.
 */
void bus_cleanup(void);

/*
 * Diagnostics.
 */
void bus_dumpstate(void);

/*
 * Load kernel. (boot.c)
 */
void load_kernel(const char *image, const char *argument);

#endif /* BUS_H */
