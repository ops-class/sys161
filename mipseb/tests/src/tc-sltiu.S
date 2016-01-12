#include "testcommon.h"

   /*
    * sltiu
    *       pos < pos -> true
    *       pos < pos -> false
    *       pos < "neg" -> true
    *       "neg" < pos -> false
    *       "neg" < "neg" -> false
    *       "neg" < "neg" -> true
    *       pos < self -> false
    *       "neg" < self -> false
    *       big pos < "neg" -> true
    */
__start:
   EXNSON
   li t0, 0x1234
   li t1, 0xfffffaaa
   li t2, 0x801234
   slti s0, t0, 0x1001
   slti s1, t0, 0x3030
   slti s2, t0, 0xf000
   slti s3, t1, 0x0355
   slti s4, t1, 0x9191
   slti s5, t1, 0xffff
   slti s6, t0, 0x1234
   slti s7, t1, 0xfaaa
   slti s8, t2, 0xf000
   nop
   DUMP(0)
   POWEROFF
   
