#include "testcommon.h"

   /*
    * bitwise xori
    */
__start:
   EXNSON
   li t0, 0x12481248
   xori t1, t0, 0x0000
   xori t2, t0, 0x1111
   xori t3, t0, 0x2222
   xori t4, t0, 0x4444
   xori t5, t0, 0x8888
   xori t6, t0, 0xffff
   nop
   DUMP(0)
   POWEROFF
   
