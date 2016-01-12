#include "testcommon.h"

   /*
    * bitwise ori
    */
__start:
   EXNSON
   li t0, 0x12481248
   ori t1, t0, 0x0000
   ori t2, t0, 0x1111
   ori t3, t0, 0x2222
   ori t4, t0, 0x4444
   ori t5, t0, 0x8888
   ori t6, t0, 0xffff
   nop
   DUMP(0)
   POWEROFF
   
