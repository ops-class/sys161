#include "testcommon.h"
#include "basic-exn.h"

   /*
    * unsigned add: positive + positive -> out-of-range positive
    */
__start:
   EXNSON
   li t0, 0x90000000
   li t1, 0xa0000000
   add t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
