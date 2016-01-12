#include "testcommon.h"
#include "basic-exn.h"

   /*
    * unsigned subu: positive - positive -> positive
    */
__start:
   EXNSON
   li t0, 0x90000000
   li t1, 0x30000000
   subu t2, t0, t1
   nop
   DUMP(0)
   POWEROFF
