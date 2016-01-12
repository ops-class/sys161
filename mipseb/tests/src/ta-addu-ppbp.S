#include "testcommon.h"
#include "basic-exn.h"

   /*
    * signed addu: positive + positive -> out-of-range positive
    */
__start:
   EXNSON
   li t0, 0x12345678
   li t1, 0x7fff8000
   addu t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
