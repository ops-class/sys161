#define EX_IRQ     0   // interrupt
#define EX_MOD     1   // tlb page read-only
#define EX_TLBL    2   // tlb miss on load
#define EX_TLBS    3   // tlb miss on store
#define EX_ADEL    4   // address error on load
#define EX_ADES    5   // address error on store
#define EX_IBE     6   // bus error on instruction fetch
#define EX_DBE     7   // bus error on data load *or* store
#define EX_SYS     8   // syscall
#define EX_BP      9   // breakpoint
#define EX_RI      10  // reserved (illegal) instruction
#define EX_CPU     11  // coprocessor unusable
#define EX_OVF     12  // arithmetic overflow
