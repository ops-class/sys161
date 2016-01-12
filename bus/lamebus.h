#include "trace.h"

#ifndef LAMEBUS_H
#define LAMEBUS_H

/*
 * Info for a simulated device.
 */
struct lamebus_device_info {
   uint32_t ldi_vendorid;
   uint32_t ldi_deviceid;
   uint32_t ldi_revision;
   void   *(*ldi_init)(int slot, int argc, char *argv[]);
   int     (*ldi_fetch)(unsigned, void *, uint32_t offset, uint32_t *rt);
   int     (*ldi_store)(unsigned, void *, uint32_t offset, uint32_t val);
   void    (*ldi_dumpstate)(void *);
   void    (*ldi_cleanup)(void *);
};

/*
 * Existing devices.
 */
extern const struct lamebus_device_info
   timer_device_info,
   disk_device_info,
   serial_device_info,
   screen_device_info,
   net_device_info,
   emufs_device_info,
   trace_device_info,
   random_device_info;

/*
 * Interrupt management.
 */
void raise_irq(int slot);
void lower_irq(int slot);
int check_irq(int slot);

#endif /* LAMEBUS_H */
