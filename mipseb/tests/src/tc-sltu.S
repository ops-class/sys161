#include "testcommon.h"

   /*
    * sltu
    *       pos < pos -> true
    *       pos < pos -> false
    *       pos < "neg" -> true
    *       "neg" < pos -> false
    *       "neg" < "neg" -> false
    *       "neg" < "neg" -> true
    *       pos < self -> false
    *       "neg" < self -> false
    */
__start:
   EXNSON
   li t0, 503655
   li t1, 707
   li t2, -533
   li t3, -625943
   li t4, 503655
   li t5, -625943
   sltu s0, t1, t0
   sltu s1, t0, t1
   sltu s2, t0, t2
   sltu s3, t2, t0
   sltu s4, t3, t2
   sltu s5, t2, t3
   sltu s6, t0, t4
   sltu s7, t3, t5
   nop
   DUMP(0)
   POWEROFF
   
