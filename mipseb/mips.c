#include <sys/types.h>
#include <arpa/inet.h>
#include <string.h>
#include "config.h"

#include "util.h" 
#include "cpu.h"
#include "bus.h"
#include "console.h"
#include "clock.h"
#include "gdb.h"
#include "main.h"
#include "trace.h"
#include "prof.h"
#include "memdefs.h"
#include "inlinemem.h"

#include "mips-insn.h"
#include "mips-ex.h"
#include "bootrom.h"

// XXX: For now, leave this off, as it isn't compatible with address space ids
//#define USE_TLBMAP


/* number of tlb entries */
#define NTLB  64

/* tlb fields */
#define TLBLO_GLOBAL		0x00000100
#define TLBLO_VALID		0x00000200
#define TLBLO_DIRTY		0x00000400
#define TLBLO_NOCACHE		0x00000800
#define TLBHI_PID		0x00000fc0
#define TLB_PAGEFRAME		0xfffff000

/* status register fields */
/*
 * The status register varies by MIPS model.
 * On all models, bits 28-31 enable coprocessors 0-3.
 * On all models, bits 8-15 enable interrupt lines.
 * On all models, bit 22 enables the bootstrap exception vectors.
 * Bits 0-7 are for interrupt and user/supervisor control.
 * Bits 16-21 and 23-27 are for miscellaneous control.
 *
 * Bits 0-7 on the r2000/r3000:
 *    0		interrupt enable
 *    1		processor is in user mode
 *    2-3	saved copy of bits 0-1 from exception
 *    4-5	saved copy of bits 2-3 from exception
 *    6-7	0
 *
 * Bits 0-7 on post-r3000 (incl. mips32):
 *    0		interrupt enable
 *    1		exception level (set for exceptions)
 *    2		error level (set for error exceptions)
 *    3		processor is in "supervisor" mode
 *    4		processor is in user mode
 *    5		0 (enable 64-bit user space on mips64)
 *    6		0 (enable 64-bit "supervisor" address space on mips64)
 *    7		0 (enable 64-bit kernel address space on mips64)
 * Turning on both the user and "supervisor" mode bits is an error.
 *
 * Bits 16-21, 23-27 on the r3000:
 *    16	isolate data cache
 *    17	swap caches
 *    18	disable cache parity
 *    19	becomes 1 if a cache miss occurs with cache isolated
 *    20	becomes 1 if a cache parity error occurs
 *    21	becomes 1 on duplicate TLB entries (irretrievable)
 *    23-24	0
 *    25	reverse endianness (later r3000 only AFAIK)
 *    26-27	0
 *
 * Bits 16-21, 23-27 on mips32:
 *    16-17	implementation-dependent
 *    18	0
 *    19	becomes 1 on NMI reset (write 0 to clear; don't write 1)
 *    20	becomes 1 on soft reset (write 0 to clear; don't write 1)
 *    21	becomes 1 on dup TLB entries (write 0 to clear; don't write 1)
 *    23	0 (enable 64-bit mode on mips64)
 *    24	0 (enable MDMX extensions on mips64)
 *    25	reverse endianness
 *    26	0 (enable extended FPU mode on mips64)
 *    27	enable reduced power mode
 *
 * Other meanings for bits 16-21, 23-27 seen in docs:
 *    16	disable cache parity exceptions
 *    17	enable use of an ECC register
 *    18	hit or miss indicator for last CACHE instruction
 *    23	instruction cache lock mode
 *    24	data cache lock mode
 *    27	enable non-blocking loads (whatever those are)
 *
 * We use the r2k/r3k meaning of bits 0-7, although at some future point
 * I'd like to have a way to enable the later version and its accompanying
 * exception handling.
 *
 * For bits 16-21, 23-27:
 *    16-17	machine check if enabled
 *    18	0 (compatible with both r3k and mips32)
 *    19	mips32 meaning (note: we don't support NMIs yet)
 *    20	mips32 meaning (note: we don't support soft reset yet)
 *    21	mips32 meaning (compatible with r3k) (note: we machine check
 *                on duplicate TLB entries)
 *    23-24	mips32 meaning, except machine check if set to 1
 *    25	reverse endianness (not supported; machine check if set to 1)
 *    26	mips32 meaning, except machine check if set to 1
 *    27	0 (might implement a reduced power mode in the future)
 * Note that we don't support machine check exceptions; instead we do the
 * debugger thing.
 */
#define STATUS_COPENABLE	0xf0000000	/* coprocessor enable bits */
/*      STATUS_LOWPOWER		0x08000000	   reduced power mode */
#define STATUS_XFPU64		0x04000000	/* 64-bit only FPU mode */
#define STATUS_REVENDIAN	0x02000000	/* reverse endian mode */
#define STATUS_MDMX64		0x01000000	/* 64-bit only MDMX exts */
#define STATUS_MODE64		0x00800000	/* 64-bit instructions */
#define STATUS_BOOTVECTORS	0x00400000	/* boot exception vectors */
#define STATUS_ERRORCAUSES	0x00380000	/* cause bits for error exns */
/*      STATUS_CACHEPARITY	0x00040000	   disable cache parity */
#define STATUS_R3KCACHE		0x00030000	/* r3k cache control bits */
#define STATUS_HARDMASK_TIMER	0x00008000	/* on-chip timer irq enabled */
#define STATUS_HARDMASK_UNUSED4	0x00004000	/* unused hardware irq lines */
#define STATUS_HARDMASK_FPU	0x00002000	/* FPU irq enabled */
#define STATUS_HARDMASK_UNUSED2	0x00001000	/* unused hardware irq lines */
#define STATUS_HARDMASK_IPI	0x00000800	/* lamebus ipi enabled */
#define STATUS_HARDMASK_LB	0x00000400	/* lamebus irq enabled */
#define STATUS_SOFTMASK		0x00000300	/* mask bits for soft irqs */
/*				0x000000c0  	   RESERVED set to 0 */
#define STATUS_KUo		0x00000020	/* old_usermode */
#define STATUS_IEo		0x00000010	/* old_irqon */
#define STATUS_KUp		0x00000008	/* prev_usermode */
#define STATUS_IEp		0x00000004	/* prev_irqon */
#define STATUS_KUc		0x00000002	/* current_usermode */
#define STATUS_IEc		0x00000001	/* current_irqon */

/* cause register fields */
#define CAUSE_BD		0x80000000	/* branch-delay flag */
/*				0x40000000	   RESERVED set to 0 */
#define CAUSE_CE		0x30000000	/* coprocessor # of exn */
/*				0x0fff0000	   RESERVED set to 0 */
#define CAUSE_HARDIRQ_TIMER	0x00008000	/* on-chip timer bit */
/*				0x00007000	   unused hardware irqs */
#define CAUSE_HARDIRQ_IPI	0x00000800	/* lamebus IPI bit */
#define CAUSE_HARDIRQ_LB	0x00000400	/* lamebus hardware irq bit */
#define CAUSE_SOFTIRQ		0x00000300	/* soft interrupt triggers */
/*				0x000000c0	   RESERVED set to 0 */
#define CAUSE_EXCODE		0x0000003c	/* exception code */
/*				0x00000003	   RESERVED Set to 0 */

/* tlb random register parameters (it ranges from 8 to 63) */
#define RANDREG_MAX		56
#define RANDREG_OFFSET		8

/* top bit in all config registers tells if the next one exists or not */
#define CONFIG_NEXTSEL_PRESENT	0x80000000

/* config0 register fields */
/*      CONFIG0_LOCAL           0x7fff0000 	for local use */
#define CONFIG0_ENDIAN   	0x00008000	/* endianness */
#define CONFIG0_TYPE            0x00006000	/* architecture type */
#define CONFIG0_REVISION        0x00001c00	/* architecture revision */
#define CONFIG0_MMU             0x000003f0	/* mmu type */
/*      zero                    0x0000007f */
#define CONFIG0_KSEG0_COHERE	0x00000007	/* cache coherence for kseg0 */

/* values for CONFIG0_ENDIAN */
#define CONFIG0_ENDIAN_BIG	0x00008000
#define CONFIG0_ENDIAN_LITTLE	0x00000000

/* values for CONFIG0_TYPE */
#define CONFIG0_TYPE_MIPS32     0x00000000
#define CONFIG0_TYPE_MIPS64_32  0x00002000
#define CONFIG0_TYPE_MIPS64     0x00004000
/*      reserved                0x00006000 */

/* value for CONFIG0_REVISION (others reserved) */
#define CONFIG0_REVISION_1	0x00000000

/* values for CONFIG0_MMU (_VINTAGE is a reserved value in mips32) */
#define CONFIG0_MMU_NONE        0x00000000	/* no mmu */
#define CONFIG0_MMU_TLB         0x000000f0	/* mips32 tlb */
#define CONFIG0_MMU_BAT         0x00000100	/* mips32 base-and-bounds */
#define CONFIG0_MMU_FIXED       0x000001f0	/* standard fixed mappings */
#define CONFIG0_MMU_VINTAGE     0x000003f0	/* mips-I MMU (sys161 only) */

/* values for CONFIG0_KSEG0_COHERE */
#define CONFIG0_KSEG0_COHERE_UNCACHED	2
#define CONFIG0_KSEG0_COHERE_CACHED	3

/* config1 register fields */
#define CONFIG1_TLBSIZE		0x7e000000	/* number of TLB entries - 1 */
#define CONFIG1_ICACHE_SETS	0x01c00000	/* icache sets per way */
#define CONFIG1_ICACHE_LINE	0x00380000	/* icache line size */
#define CONFIG1_ICACHE_ASSOC	0x00070000	/* icache associativity */
#define CONFIG1_DCACHE_SETS	0x0000e000	/* dcache sets per way */
#define CONFIG1_DCACHE_LINE	0x00001c00	/* dcache line size */
#define CONFIG1_DCACHE_ASSOC	0x00000380	/* dcache associativity */
#define CONFIG1_COP2            0x00000040	/* cop2 exists */
#define CONFIG1_MDMX64		0x00000020	/* MDMX64 implemented */
#define CONFIG1_PERFCTRS	0x00000010	/* perf counters implemented */
#define CONFIG1_WATCH		0x00000008	/* watch regs implemented */
#define CONFIG1_MIPS16		0x00000004	/* mips16 implemented */
#define CONFIG1_EJTAG		0x00000002	/* ejtag implemented */
#define CONFIG1_FPU		0x00000001	/* fpu implemented */

/* values for CONFIG1_TLBSIZE (valid inputs 1-64) */
#define CONFIG1_MK_TLBSIZE(n)	(((n)-1) << 25)

/* values for CONFIG1_[ID]CACHE_* */
#define CONFIG1_SETS_64		0
#define CONFIG1_SETS_128	1
#define CONFIG1_SETS_256	2
#define CONFIG1_SETS_512	3
#define CONFIG1_SETS_1024	4
#define CONFIG1_SETS_2048	5
#define CONFIG1_SETS_4096	6
#define CONFIG1_LINE_NONE	0
#define CONFIG1_LINE_4		1
#define CONFIG1_LINE_8		2
#define CONFIG1_LINE_16		3
#define CONFIG1_LINE_32		4
#define CONFIG1_LINE_64		5
#define CONFIG1_LINE_128	6
#define CONFIG1_MK_ASSOC(n)	(n-1)	/* valid values 1-8 */
#define CONFIG1_MK_CACHE(s, l, a) (((s) << 6) | ((l) << 3) | (a))
#define CONFIG1_MK_ICACHE(s, l, a) (CONFIG1_MK_CACHE(s, l, a) << 16)
#define CONFIG1_MK_DCACHE(s, l, a) (CONFIG1_MK_CACHE(s, l, a) << 7)

/*
 * Coprocessor registers have a register number (0-31) and then also
 * a "select" number 0-7, which is basically a bank number. So you can
 * have up to 256 registers per coprocessor.
 */
#define REGSEL(reg, sel) (((reg) << 3) | (sel))

/* system coprocessor (cop0) registers and selects */
#define C0_INDEX   REGSEL(0, 0)
#define C0_RANDOM  REGSEL(1, 0)
#define C0_TLBLO   REGSEL(2, 0)
#define C0_CONTEXT REGSEL(4, 0)
#define C0_VADDR   REGSEL(8, 0)
#define C0_COUNT   REGSEL(9, 0)
#define C0_TLBHI   REGSEL(10, 0)
#define C0_COMPARE REGSEL(11, 0)
#define C0_STATUS  REGSEL(12, 0)
#define C0_CAUSE   REGSEL(13, 0)
#define C0_EPC     REGSEL(14, 0)
#define C0_PRID    REGSEL(15, 0)
#define C0_CFEAT   REGSEL(15, 1)
#define C0_IFEAT   REGSEL(15, 2)
#define C0_CONFIG0 REGSEL(16, 0)
#define C0_CONFIG1 REGSEL(16, 1)
#define C0_CONFIG2 REGSEL(16, 2)
#define C0_CONFIG3 REGSEL(16, 3)
#define C0_CONFIG4 REGSEL(16, 4)
#define C0_CONFIG5 REGSEL(16, 5)
#define C0_CONFIG6 REGSEL(16, 6)
#define C0_CONFIG7 REGSEL(16, 7)

/* Version IDs for C0_PRID */
#define PRID_VALUE_ANCIENT	0xbeef    /* sys161 <= 0.95 */
#define PRID_VALUE_OLD		0x03ff    /* sys161 1.x and 1.99.x<=1.99.06 */
#define PRID_VALUE_CURRENT	0x00a1    /* sys161 2.x */

/* Feature flags for C0_CFEAT and C0_IFEAT */
/* (none yet) */

/* MIPS hardwired memory segments */
#define KSEG2	0xc0000000
#define KSEG1	0xa0000000
#define KSEG0	0x80000000
#define KUSEG	0x00000000

#ifdef USE_TLBMAP
/* tlbmap value for "nothing" */
#define TM_NOPAGE    255
#endif

/* number of general registers */
#define NREGS 32

struct mipstlb {
	int mt_global;		// 1: translation is global
	int mt_valid;		// 1: translation is valid for use
	int mt_dirty;		// 1: write enable
	int mt_nocache;		// 1: cache disable
	uint32_t mt_pfn;	// page number part of physical address
	uint32_t mt_vpn;	// page number part of virtual address
	uint32_t mt_pid;	// address space id
};

/* possible states for a cpu */
enum cpustates {
	CPU_DISABLED,
	CPU_IDLE,
	CPU_RUNNING,
};

struct mipscpu {
	// state of this cpu
	enum cpustates state;

	// my cpu number
	unsigned cpunum;

	// general registers
	int32_t r[NREGS];

	// special registers
	int32_t lo, hi;

	// pipeline stall logic
	int lowait, hiwait; // cycles to wait for lo/hi to become ready

	// "jumping" is set by the jump instruction.
	// "in_jumpdelay" is set during decoding of the instruction in a jump 
	// delay slot.
	int jumping;
	int in_jumpdelay;

	// pc/exception stuff
	//
	// at instruction decode time, pc points to the delay slot and
	// nextpc points to the instruction after. thus, a branch
	// instruction alters nextpc. expc points to the instruction
	// being executed (unless it's in the delay slot of a jump).

	uint32_t expc;         // pc for exception (not incr. while jumping)
	
	uint32_t pc;           // pc
	uint32_t nextpc;       // succeeding pc
	uint32_t pcoff;	// page offset of pc
	uint32_t nextpcoff;	// page offset of nextpc
	const uint32_t *pcpage;	// precomputed memory page of pc
	const uint32_t *nextpcpage;	// precomputed memory page of nextpc

	// mmu
	struct mipstlb tlb[NTLB];
	struct mipstlb tlbentry;	// cop0 register 2 (lo) and 10 (hi)
#ifdef USE_TLBMAP
	uint8_t tlbmap[1024*1024];	// vpn -> tlbentry map
#endif

	/*
	 * tlb index register (cop0 register 0)
	 */
	int tlbindex;		// not shifted; 0-63
	int tlbpf;		// true if a tlpb failed

	/*
	 * tlb random register (cop0 register 1)
	 */
	int tlbrandom;		// not shifted, not bounded, 0-based

	// exception stuff

	/*
	 * status register (cop0 register 12)
	 */
	int old_usermode;
	int old_irqon;
	int prev_usermode;
	int prev_irqon;
	int current_usermode;
	int current_irqon;
	uint32_t status_hardmask_lb;	// nonzero if lamebus irq is enabled
	uint32_t status_hardmask_ipi;	// nonzero if ipi is enabled
	uint32_t status_hardmask_fpu;	// nonzero if FPU irq is enabled
	uint32_t status_hardmask_void;	// unused hard irq bits
	uint32_t status_hardmask_timer;	// nonzero if timer irq is enabled
	uint32_t status_softmask;	// soft interrupt masking bits
	uint32_t status_bootvectors;	// nonzero if BEV bit on
	uint32_t status_copenable;	// coprocessor 0-3 enable bits
	
	/*
	 * cause register (cop0 register 13)
	 */
	int cause_bd;			// NOT shifted
	uint32_t cause_ce;		// already shifted
	uint32_t cause_softirq;	// already shifted
	uint32_t cause_code;		// already shifted

	/*
	 * config registers
	 */
	uint32_t ex_config0;	// cop0 register 16 sel 0
	uint32_t ex_config1;	// cop0 register 16 sel 1
	uint32_t ex_config2;	// cop0 register 16 sel 2
	uint32_t ex_config3;	// cop0 register 16 sel 3
	uint32_t ex_config4;	// cop0 register 16 sel 4
	uint32_t ex_config5;	// cop0 register 16 sel 5
	uint32_t ex_config6;	// cop0 register 16 sel 6
	uint32_t ex_config7;	// cop0 register 16 sel 7

	/*
	 * other cop0 registers
	 */
	uint32_t ex_context;	// cop0 register 4
	uint32_t ex_epc;	// cop0 register 14
	uint32_t ex_vaddr;	// cop0 register 8
	uint32_t ex_prid;	// cop0 register 15
	uint32_t ex_cfeat;	// cop0 register 15 sel 1
	uint32_t ex_ifeat;	// cop0 register 15 sel 2
	uint32_t ex_count;	// cop0 register 9
	uint32_t ex_compare;	// cop0 register 11
	int ex_compare_used;	// timer irq disabled if not set

	/*
	 * interrupt bits
	 */
	int irq_lamebus;
	int irq_ipi;
	int irq_timer;

	/*
	 * LL/SC hooks
	 */
	int ll_active;
	uint32_t ll_addr;
	uint32_t ll_value;

	/*
	 * debugger hooks
	 */
	int hit_breakpoint;
};

#define IS_USERMODE(cpu) ((cpu)->current_usermode)

static struct mipscpu *mycpus;
static unsigned ncpus;

/*
 * Hold cpu->state == CPU_RUNNING across all cpus, for rapid testing.
 */
uint32_t cpu_running_mask;

#define RUNNING_MASK_OFF(cn) (cpu_running_mask &= ~((uint32_t)1 << (cn)))
#define RUNNING_MASK_ON(cn)  (cpu_running_mask |= (uint32_t)1 << (cn))

/*
 * Number of cycles into cpu_cycles().
 */
uint64_t cpu_cycles_count;

/*************************************************************/

static const char *exception_names[13] = {
	"interrupt",
	"TLB modify",
	"TLB miss - load",
	"TLB miss - store",
	"Address error - load",
	"Address error - store",
	"Bus error - code",
	"Bus error - data",
	"System call",
	"Breakpoint",
	"Illegal instruction",
	"Coprocessor unusable",
	"Arithmetic overflow",
};

static
const char *
exception_name(int code)
{
	if (code >= 0 && code < 13) {
		return exception_names[code];
	}
	smoke("Name of invalid exception code requested");
	return "???";
}

/*************************************************************/

/*
 * These are further down.
 */
static int precompute_pc(struct mipscpu *cpu);
static int precompute_nextpc(struct mipscpu *cpu);

/*
 * The MIPS doesn't clear the TLB on reset, so it's perfectly correct
 * to do nothing here. However, let's initialize it to all entries that
 * can't be matched.
 */

static
void
reset_tlbentry(struct mipstlb *mt, int index)
{
	mt->mt_global = 0;
	mt->mt_valid = 0;
	mt->mt_dirty = 0;
	mt->mt_nocache = 0;
	mt->mt_pfn = 0;
	mt->mt_vpn = 0x81000000 + index*0x1000;
	mt->mt_pid = 0;
}

static
void
mips_init(struct mipscpu *cpu, unsigned cpunum)
{
	int i;

	cpu->state = CPU_DISABLED;
	cpu->cpunum = cpunum;
	for (i=0; i<NREGS; i++) {
		cpu->r[i] = 0;
	}
	cpu->lo = cpu->hi = 0;
	cpu->lowait = cpu->hiwait = 0;

	for (i=0; i<NTLB; i++) {
		reset_tlbentry(&cpu->tlb[i], i);
	}
	reset_tlbentry(&cpu->tlbentry, NTLB);
#ifdef USE_TLBMAP
	memset(cpu->tlbmap, TM_NOPAGE, sizeof(cpu->tlbmap));
#endif
	cpu->tlbindex = 0;
	cpu->tlbpf = 0;
	cpu->tlbrandom = RANDREG_MAX-1;

	cpu->old_usermode = 0;
	cpu->old_irqon = 0;
	cpu->prev_usermode = 0;
	cpu->prev_irqon = 0;
	cpu->current_usermode = 0;
	cpu->current_irqon = 0;
	cpu->status_hardmask_lb = 0;
	cpu->status_hardmask_ipi = 0;
	cpu->status_hardmask_fpu = 0;
	cpu->status_hardmask_void = 0;
	cpu->status_hardmask_timer = 0;
	cpu->status_softmask = 0;
	cpu->status_bootvectors = STATUS_BOOTVECTORS;
	cpu->status_copenable = 0;

	cpu->cause_bd = 0;
	cpu->cause_ce = 0;
	cpu->cause_softirq = 0;
	cpu->cause_code = 0;

	/* config register 0 - misc info */
	cpu->ex_config0 = CONFIG_NEXTSEL_PRESENT |
		CONFIG0_ENDIAN_BIG |
		CONFIG0_TYPE_MIPS32 |
		CONFIG0_REVISION_1 |
		CONFIG0_MMU_VINTAGE |
		CONFIG0_KSEG0_COHERE_CACHED;

	/* config register 1 - mostly L1 cache info */
	/* for now report a 4K each 4-way 16-byte-line icache and dcache */
	cpu->ex_config1 =
		CONFIG1_MK_TLBSIZE(NTLB) |
		CONFIG1_MK_ICACHE(CONFIG1_SETS_64, CONFIG1_LINE_16,
				  CONFIG1_MK_ASSOC(4)) |
		CONFIG1_MK_DCACHE(CONFIG1_SETS_64, CONFIG1_LINE_16,
				  CONFIG1_MK_ASSOC(4));

	/* config register 2 - L2/L3 cache info */
	cpu->ex_config2 = 0;

	/* config register 3 - architecture extensions */
	cpu->ex_config3 = 0;

	/* config registers 4 and 5 - not defined in docs I have */
	cpu->ex_config4 = 0;
	cpu->ex_config5 = 0;

	/* config registers 6 and 7 - implementation-specific */
	cpu->ex_config6 = 0;
	cpu->ex_config7 = 0;

	/* other cop0 registers */
	cpu->ex_context = 0;
	cpu->ex_epc = 0;
	cpu->ex_vaddr = 0;
	cpu->ex_prid = PRID_VALUE_CURRENT;
	cpu->ex_cfeat = 0;
	cpu->ex_ifeat = 0;
	cpu->ex_count = 1;
	cpu->ex_compare = 0;
	cpu->ex_compare_used = 0;

	cpu->irq_lamebus = 0;
	cpu->irq_ipi = 0;
	cpu->irq_timer = 0;

	cpu->ll_active = 0;
	cpu->ll_addr = 0;
	cpu->ll_value = 0;

	cpu->jumping = cpu->in_jumpdelay = 0;
	cpu->expc = 0;

	/* pc starts at 0xbfc00000; nextpc at 0xbfc00004 */
	cpu->pc = 0xbfc00000;
	cpu->nextpc = 0xbfc00004;

	/* this must be last after other stuff is initialized */
	if (precompute_pc(cpu)) {
		smoke("precompute_pc failed in mips_init");
	}
	if (precompute_nextpc(cpu)) {
		smoke("precompute_nextpc failed in mips_init");
	}

}

static
uint32_t
tlbgetlo(const struct mipstlb *mt)
{
	uint32_t val = mt->mt_pfn;
	if (mt->mt_global) {
		val |= TLBLO_GLOBAL;
	}
	if (mt->mt_valid) {
		val |= TLBLO_VALID;
	}
	if (mt->mt_dirty) {
		val |= TLBLO_DIRTY;
	}
	if (mt->mt_nocache) {
		val |= TLBLO_NOCACHE;
	}
	return val;
}

static
uint32_t
tlbgethi(const struct mipstlb *mt)
{
	uint32_t val = mt->mt_vpn;
	val |= (mt->mt_pid << 6);
	return val;
}

static
void
tlbsetlo(struct mipstlb *mt, uint32_t val)
{
	mt->mt_global = val & TLBLO_GLOBAL;
	mt->mt_valid = val & TLBLO_VALID;
	mt->mt_dirty = val & TLBLO_DIRTY;
	mt->mt_nocache = val & TLBLO_NOCACHE;
	mt->mt_pfn = val & TLB_PAGEFRAME;
}

static
void
tlbsethi(struct mipstlb *mt, uint32_t val)
{
	mt->mt_vpn = val & TLB_PAGEFRAME;
	mt->mt_pid = (val & TLBHI_PID) >> 6;
}

#define TLBTRP(t) CPUTRACEL(DOTRACE_TLB, \
			    cpu->cpunum, \
			    "%05x %s%s%s%s", \
				(t)->mt_pfn >> 12, \
				(t)->mt_global ? "G" : "-", \
				(t)->mt_valid ? "V" : "-", \
				(t)->mt_dirty ? "D" : "-", \
				(t)->mt_nocache ? "N" : "-")
#define TLBTRV(t) CPUTRACEL(DOTRACE_TLB, \
			    cpu->cpunum, \
			    "%05x/%03x -> ", \
				(t)->mt_vpn >> 12, (t)->mt_pid)
#define TLBTR(t) {TLBTRV(t);TLBTRP(t);}

static
void
tlbmsg(const char *what, int index, const struct mipstlb *t)
{
	msgl("%s: ", what);
	if (index>=0) {
		msgl("index %d, %s", index, index < 10 ? " " : "");
	}
	else {
		msgl("tlbhi/lo, ");
	}
	msgl("vpn 0x%08lx, ", (unsigned long) t->mt_vpn);
	     
	if (t->mt_global) {
		msgl("global, ");
	}
	else {
		msgl("pid %d, %s", (int) t->mt_pid,
		     t->mt_pid < 10 ? " " : "");
	}

	msg("ppn 0x%08lx (%s%s%s)",
	    (unsigned long) t->mt_pfn,
	    t->mt_valid ? "V" : "-",
	    t->mt_dirty ? "D" : "-",
	    t->mt_nocache ? "N" : "-");
}

static
void
check_tlb_dups(struct mipscpu *cpu, int newix)
{
	uint32_t vpn, pid;
	int gbl, i;

	vpn = cpu->tlb[newix].mt_vpn;
	pid = cpu->tlb[newix].mt_pid;
	gbl = cpu->tlb[newix].mt_global;

	for (i=0; i<NTLB; i++) {
		if (i == newix) {
			continue;
		}
		if (vpn != cpu->tlb[i].mt_vpn) {
			continue;
		}

		/*
		 * We've got two translations for the same virtual page.
		 * If both translations would ever match at once, it's bad.
		 * This is true if *either* is global or if the pids are
		 * the same. Note that it doesn't matter if the valid bits
		 * are set - translations that are not valid are still 
		 * accessed.
		 */

		if (gbl ||
		    cpu->tlb[i].mt_global ||
		    pid == cpu->tlb[i].mt_pid) {
			msg("Duplicate TLB entries!");
			tlbmsg("New entry", newix, &cpu->tlb[newix]);
			tlbmsg("Old entry", i, &cpu->tlb[i]);
			hang("Duplicate TLB entries for vpage %x",
			     cpu->tlb[i].mt_vpn);
		}
	}
}

static
inline
int
findtlb(const struct mipscpu *cpu, uint32_t vpage)
{
#ifdef USE_TLBMAP
	uint8_t tm;

	tm = cpu->tlbmap[vpage >> 12];
	if (tm == TM_NOPAGE) {
		return -1;
	}
	return tm;
#else
	int i;
	for (i=0; i<NTLB; i++) {
		const struct mipstlb *mt = &cpu->tlb[i];
		if (mt->mt_vpn!=vpage) continue;
		if (mt->mt_pid==cpu->tlbentry.mt_pid || mt->mt_global) {
			return i;
		}
	}

	return -1;
#endif
}

static
void
probetlb(struct mipscpu *cpu)
{
	uint32_t vpage;
	int ix;

	vpage = cpu->tlbentry.mt_vpn;
	ix = findtlb(cpu, vpage);

	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, "tlbp:       ");
	TLBTRV(&cpu->tlbentry);

	if (ix<0) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, "NOT FOUND");
		cpu->tlbpf = 1;
	}
	else {
		TLBTRP(&cpu->tlb[ix]);
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, ": [%d]", ix);
		cpu->tlbindex = ix;
		cpu->tlbpf = 0;
	}
}

static
void
writetlb(struct mipscpu *cpu, int ix, const char *how)
{
	(void)how;
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, "%s: [%2d] ", how, ix);
	TLBTR(&cpu->tlb[ix]);
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, " ==> ");
	TLBTR(&cpu->tlbentry);
	CPUTRACE(DOTRACE_TLB, cpu->cpunum, " ");

#ifdef USE_TLBMAP
	cpu->tlbmap[cpu->tlb[ix].mt_vpn >> 12] = TM_NOPAGE;
#endif

	cpu->tlb[ix] = cpu->tlbentry;

#ifdef USE_TLBMAP
	cpu->tlbmap[cpu->tlb[ix].mt_vpn >> 12] = ix;
#endif

	check_tlb_dups(cpu, ix);

	/* 
	 * If the OS coder is a lunatic, the mapping for the pc might
	 * have changed. If this causes an exception, we don't need
	 * to do anything special, though.
	 */
	(void) precompute_pc(cpu);
	(void) precompute_nextpc(cpu);
}

static
void
do_wait(struct mipscpu *cpu)
{
	/* Only wait if no interrupts are already pending */
	if (!cpu->irq_lamebus && !cpu->irq_ipi && !cpu->irq_timer) {
		cpu->state = CPU_IDLE;
		RUNNING_MASK_OFF(cpu->cpunum);
	}
}

static
void
do_rfe(struct mipscpu *cpu)
{
	//uint32_t bits;

	if (IS_USERMODE(cpu)) {
		smoke("RFE in usermode not caught by instruction decoder");
	}

	cpu->current_usermode = cpu->prev_usermode;
	cpu->current_irqon = cpu->prev_irqon;
	cpu->prev_usermode = cpu->old_usermode;
	cpu->prev_irqon = cpu->old_irqon;
	CPUTRACE(DOTRACE_EXN, cpu->cpunum,
		 "Return from exception: %s mode, interrupts %s, sp %x",
		 (cpu->current_usermode) ? "user" : "kernel",
		 (cpu->current_irqon) ? "on" : "off",
		 cpu->r[29]);

	/*
	 * Re-lookup the translations for the pc, because we might have
	 * changed to usermode.
	 *
	 * Furthermore, hack the processor state so the exception happens
	 * with things pointing to the instruction happening after the rfe,
	 * not the rfe itself.
	 */
	
	cpu->in_jumpdelay = 0;
	cpu->expc = cpu->pc;

	precompute_pc(cpu);
	precompute_nextpc(cpu);
}

/*
 * This corrects the state of the processor if we're handling a
 * breakpoint with the remote gdb code. The remote gdb code should use
 * cpu->expc as the place execution stopped; in that case something
 * vaguely reasonable will happen if some nimrod puts a breakpoint in
 * a branch delay slot.
 *
 * This does not invalidate ll_active.
 */
static
void
phony_exception(struct mipscpu *cpu)
{
	cpu->jumping = 0;
	cpu->in_jumpdelay = 0;
	cpu->pc = cpu->expc;
	cpu->nextpc = cpu->pc + 4;

	/*
	 * These shouldn't fail because we were just executing with 
	 * the same values.
	 */
	if (precompute_pc(cpu)) {
		smoke("precompute_pc failed in phony_exception");
	}
	if (precompute_nextpc(cpu)) {
		smoke("precompute_nextpc failed in phony_exception");
	}
}

static
void
exception(struct mipscpu *cpu, int code, int cn_or_user, uint32_t vaddr)
{
	//uint32_t bits;
	int boot = (cpu->status_bootvectors) != 0;

	CPUTRACE(DOTRACE_EXN, cpu->cpunum,
		 "exception: code %d (%s), expc %x, vaddr %x, sp %x", 
		 code, exception_name(code), cpu->expc, vaddr, cpu->r[29]);

	if (code==EX_IRQ) {
		g_stats.s_irqs++;
	}
	else {
		g_stats.s_exns++;
	}

	cpu->cause_bd = cpu->in_jumpdelay;
	if (code==EX_CPU) {
		cpu->cause_ce = ((uint32_t)cn_or_user << 28);
	}
	else {
		cpu->cause_ce = 0;
	}
	cpu->cause_code = code << 2;

	cpu->jumping = 0;
	cpu->in_jumpdelay = 0;
	
	cpu->ll_active = 0;

	// roll the status bits
	cpu->old_usermode = cpu->prev_usermode;
	cpu->old_irqon = cpu->prev_irqon;
	cpu->prev_usermode = cpu->current_usermode;
	cpu->prev_irqon = cpu->current_irqon;
	cpu->current_usermode = 0;
	cpu->current_irqon = 0;

	cpu->ex_vaddr = vaddr;
	cpu->ex_context &= 0xffe00000;
	cpu->ex_context |= ((vaddr & 0x7ffff000) >> 10);
	
	cpu->ex_epc = cpu->expc;

	if ((code==EX_TLBL || code==EX_TLBS) && cn_or_user) {
		// use special UTLB exception vector
		cpu->pc = boot ? 0xbfc00100 : 0x80000000;
	}
	else {
		cpu->pc = boot ? 0xbfc00180 : 0x80000080;
	}
	cpu->nextpc = cpu->pc + 4;

	/*
	 * These cannot fail. Furthermore, if for some reason they do,
	 * they'll likely recurse forever and not return, so checking
	 * the return value is fairly pointless.
	 */
	(void) precompute_pc(cpu);
	(void) precompute_nextpc(cpu);
}

/*
 * Warning: do not change this function without making corresponding
 * changes to debug_translatemem (below).
 */
static
inline
int
translatemem(struct mipscpu *cpu, uint32_t vaddr, int iswrite, uint32_t *ret)
{
	uint32_t seg;
	uint32_t paddr;

	// MIPS hardwired memory layout:
	//    0xc0000000 - 0xffffffff   kseg2 (kernel, tlb-mapped)
	//    0xa0000000 - 0xbfffffff   kseg1 (kernel, unmapped, uncached)
	//    0x80000000 - 0x9fffffff   kseg0 (kernel, unmapped, cached)
	//    0x00000000 - 0x7fffffff   kuseg (user, tlb-mapped)
	//
	// Since we don't implement cache, we can consider kseg0 and kseg1
	// equivalent (except remember that the base of each maps to paddr 0.)

	/*
	 * On intel at least it's noticeably faster this way:
	 * compute seg first, but don't use it when checking for address
	 * error, only for whether we're in a direct-mapped segment.
	 *
	 * My guess is that gcc's instruction scheduler isn't good enough to
	 * handle this on its own and we get pipeline stalls with a more
	 * sensible organization.
	 *
	 * XXX retest this with modern gcc
	 */
	seg = vaddr >> 30;

	if ((vaddr >= 0x80000000 && IS_USERMODE(cpu)) || (vaddr & 0x3)!=0) {
		exception(cpu, iswrite ? EX_ADES : EX_ADEL, 0, vaddr);
		return -1;
	}

	if (seg==2) {
		paddr = vaddr & 0x1fffffff;
	}
	else {
		uint32_t vpage;
		uint32_t off;
		uint32_t ppage;
		int ix;

		vpage = vaddr & 0xfffff000;
		off   = vaddr & 0x00000fff;

		CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
			  "tlblookup:  %05x/%03x -> ", 
			  vpage >> 12, cpu->tlbentry.mt_pid);

		cpu->tlbentry.mt_vpn = vpage;
		ix = findtlb(cpu, vpage);

		if (ix<0) {
			int exc = iswrite ? EX_TLBS : EX_TLBL;
			int isuseraddr = vaddr < 0x80000000;
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, "MISS");
			exception(cpu, exc, isuseraddr, vaddr);
			return -1;
		}
		TLBTRP(&cpu->tlb[ix]);
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ": [%d]", ix);

		if (!cpu->tlb[ix].mt_valid) {
			int exc = iswrite ? EX_TLBS : EX_TLBL;
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - INVALID");
			exception(cpu, exc, 0, vaddr);
			return -1;
		}
		if (iswrite && !cpu->tlb[ix].mt_dirty) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - READONLY");
			exception(cpu, EX_MOD, 0, vaddr);
			return -1;
		}
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OK");
		ppage = cpu->tlb[ix].mt_pfn;
		paddr = ppage|off;
	}

	*ret = paddr;
	return 0;
}

/*
 * Special version of translatemem for use from the gdb access code. It
 * does not touch the cpu state and always considers itself to be in 
 * supervisor mode.
 *
 * Warning: do not change this function without making corresponding
 * changes to the original translatemem (above).
 */
static
inline
int
debug_translatemem(const struct mipscpu *cpu, uint32_t vaddr, 
		   int iswrite, uint32_t *ret)
{
	uint32_t paddr;

	if ((vaddr & 0x3)!=0) {
		return -1;
	}

	if ((vaddr >> 30)==2) {
		paddr = vaddr & 0x1fffffff;
	}
	else {
		uint32_t vpage;
		uint32_t off;
		uint32_t ppage;
		int ix;

		vpage = vaddr & 0xfffff000;
		off   = vaddr & 0x00000fff;

		CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
			  "tlblookup (debugger):  %05x/%03x -> ", 
			  vpage >> 12, cpu->tlbentry.mt_pid);

		ix = findtlb(cpu, vpage);

		if (ix<0) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, "MISS");
			return -1;
		}
		TLBTRP(&cpu->tlb[ix]);
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ": [%d]", ix);

		if (!cpu->tlb[ix].mt_valid) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - INVALID");
			return -1;
		}
		if (iswrite && !cpu->tlb[ix].mt_dirty) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - READONLY");
			return -1;
		}
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OK");
		ppage = cpu->tlb[ix].mt_pfn;
		paddr = ppage|off;
	}

	*ret = paddr;
	return 0;

}

static
inline
int
accessmem(struct mipscpu *cpu, uint32_t paddr, int iswrite, uint32_t *val)
{
	int buserr;

	/*
	 * Physical memory layout: 
	 *    0x00000000 - 0x1fbfffff     RAM
	 *    0x1fc00000 - 0x1fdfffff     Boot ROM
	 *    0x1fe00000 - 0x1fffffff     LAMEbus mapped I/O
	 *    0x20000000 - 0xffffffff     RAM
	 */
	
	if (paddr < 0x1fc00000) {
		if (iswrite) {
			buserr = bus_mem_store(paddr, *val);
		}
		else {
			buserr = bus_mem_fetch(paddr, val);
		}
	}
	else if (paddr < 0x1fe00000) {
		if (iswrite) {
			buserr = -1; /* ROM is, after all, read-only */
		}
		else {
			buserr = bootrom_fetch(paddr - 0x1fc00000, val);
		}
	}
	else if (paddr < 0x20000000) {
		if (iswrite) {
			buserr = bus_io_store(cpu->cpunum,
					      paddr-0x1fe00000, *val);
		}
		else {
			buserr = bus_io_fetch(cpu->cpunum,
					      paddr-0x1fe00000, val);
		}
	}
	else {
		if (iswrite) {
			buserr = bus_mem_store(paddr-0x00400000, *val);
		}
		else {
			buserr = bus_mem_fetch(paddr-0x00400000, val);
		}
	}

	if (buserr) {
		exception(cpu, EX_DBE, 0, 0);
		return -1;
	}
	
	return 0;
}

/*
 * This is a special version of accessmem used for instruction fetch.
 * It returns a pointer to an entire page of memory which can then be
 * used repeatedly. Note that it doesn't call exception() - it just
 * returns NULL if the memory doesn't exist.
 */
static
inline
const uint32_t *
mapmem(uint32_t paddr)
{
	/*
	 * Physical memory layout: 
	 *    0x00000000 - 0x1fbfffff     RAM
	 *    0x1fc00000 - 0x1fdfffff     Boot ROM
	 *    0x1fe00000 - 0x1fffffff     LAMEbus mapped I/O
	 *    0x20000000 - 0xffffffff     RAM
	 */
	paddr &= 0xfffff000;

	if (paddr < 0x1fc00000) {
		return bus_mem_map(paddr);
	}

	if (paddr < 0x1fe00000) {
		return bootrom_map(paddr - 0x1fc00000);
	}

	if (paddr < 0x20000000) {
		/* don't allow executing from I/O registers */
		return NULL;
	}

	return bus_mem_map(paddr-0x00400000);
}

/*
 * iswrite should be true if *this* domem operation is a write.
 *
 * willbewrite should be true if any domem operation on this
 * cycle is (or will be) a write, even if *this* one isn't.
 * (This is for preventing silly exception behavior when writing
 * a sub-word quantity.)
 */
static
int
domem(struct mipscpu *cpu, uint32_t vaddr, uint32_t *val, 
      int iswrite, int willbewrite)
{
	uint32_t paddr;
	
	if (translatemem(cpu, vaddr, willbewrite, &paddr)) {
		return -1;
	}

	return accessmem(cpu, paddr, iswrite, val);
}

static
int
precompute_pc(struct mipscpu *cpu)
{
	uint32_t physpc;
	if (translatemem(cpu, cpu->pc, 0, &physpc)) {
		return -1;
	}
	cpu->pcpage = mapmem(physpc);
	if (cpu->pcpage == NULL) {
		exception(cpu, EX_IBE, 0, 0);
		if (cpu->pcpage == NULL) {
			smoke("Bus error invoking exception handler");
		}
		return -1;
	}
	cpu->pcoff = physpc & 0xfff;
	return 0;
}

static
int
precompute_nextpc(struct mipscpu *cpu)
{
	uint32_t physnext;
	if (translatemem(cpu, cpu->nextpc, 0, &physnext)) {
		return -1;
	}
	cpu->nextpcpage = mapmem(physnext);
	if (cpu->nextpcpage == NULL) {
		exception(cpu, EX_IBE, 0, 0);
		if (cpu->nextpcpage == NULL) {
			smoke("Bus error invoking exception handler");
		}
		return -1;
	}
	cpu->nextpcoff = physnext & 0xfff;
	return 0;
}


typedef enum {
	S_SBYTE,
	S_UBYTE,
	S_SHALF,
	S_UHALF,
	S_WORDL,
	S_WORDR,
} memstyles;

static
void
doload(struct mipscpu *cpu, memstyles ms, uint32_t addr, uint32_t *res)
{
	switch (ms) {
	    case S_SBYTE:
	    case S_UBYTE:
	    {
		uint32_t val;
		uint8_t bval = 0;
		if (domem(cpu, addr & 0xfffffffc, &val, 0, 0)) return;
		switch (addr & 3) {
			case 0: bval = (val & 0xff000000)>>24; break;
			case 1: bval = (val & 0x00ff0000)>>16; break;
			case 2: bval = (val & 0x0000ff00)>>8; break;
			case 3: bval = val & 0x000000ff; break;
		}
		if (ms==S_SBYTE) *res = (int32_t)(int8_t)bval;
		else *res = bval;
	    }
	    break;

	    case S_SHALF:
	    case S_UHALF:
	    {
		uint32_t val;
		uint16_t hval = 0;
		if (domem(cpu, addr & 0xfffffffd, &val, 0, 0)) return;
		switch (addr & 2) {
			case 0: hval = (val & 0xffff0000)>>16; break;
			case 2: hval = val & 0x0000ffff; break;
		}
		if (ms==S_SHALF) *res = (int32_t)(int16_t)hval;
		else *res = hval;
	    }
	    break;
     
	    case S_WORDL:
	    {
		uint32_t val;
		uint32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &val, 0, 0)) return;
		switch (addr & 0x3) {
		    case 0: mask = 0xffffffff; shift=0; break;
		    case 1: mask = 0xffffff00; shift=8; break;
		    case 2: mask = 0xffff0000; shift=16; break;
		    case 3: mask = 0xff000000; shift=24; break;
		}
		val <<= shift;
		*res = (*res & ~mask) | (val & mask);
	    }
	    break;
	    case S_WORDR:
	    {
		uint32_t val;
		uint32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &val, 0, 0)) return;
		switch (addr & 0x3) {
			case 0: mask = 0x000000ff; shift=24; break;
			case 1: mask = 0x0000ffff; shift=16; break;
			case 2: mask = 0x00ffffff; shift=8; break;
			case 3: mask = 0xffffffff; shift=0; break;
		}
		val >>= shift;
		*res = (*res & ~mask) | (val & mask);
	    }
	    break;
	    default:
		smoke("doload: Illegal addressing mode");
	}
}

static
void
dostore(struct mipscpu *cpu, memstyles ms, uint32_t addr, uint32_t val)
{
	switch (ms) {
	    case S_UBYTE:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		switch (addr & 3) {
		    case 0: mask = 0xff000000; shift=24; break;
		    case 1: mask = 0x00ff0000; shift=16; break;
		    case 2: mask = 0x0000ff00; shift=8; break;
		    case 3: mask = 0x000000ff; shift=0; break;
		}
		if (domem(cpu, addr & 0xfffffffc, &wval, 0, 1)) return;
		wval = (wval & ~mask) | ((val&0xff) << shift);
		if (domem(cpu, addr & 0xfffffffc, &wval, 1, 1)) return;
	    }
	    break;

	    case S_UHALF:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		switch (addr & 2) {
			case 0: mask = 0xffff0000; shift=16; break;
			case 2: mask = 0x0000ffff; shift=0; break;
		}
		if (domem(cpu, addr & 0xfffffffd, &wval, 0, 1)) return;
		wval = (wval & ~mask) | ((val&0xffff) << shift);
		if (domem(cpu, addr & 0xfffffffd, &wval, 1, 1)) return;
	    }
	    break;
	
	    case S_WORDL:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &wval, 0, 1)) return;
		switch (addr & 0x3) {
			case 0: mask = 0xffffffff; shift=0; break;
			case 1: mask = 0x00ffffff; shift=8; break;
			case 2: mask = 0x0000ffff; shift=16; break;
			case 3: mask = 0x000000ff; shift=24; break;
		}
		val >>= shift;
		wval = (wval & ~mask) | (val & mask);

		if (domem(cpu, addr & 0xfffffffc, &wval, 1, 1)) return;
	    }
	    break;
	    case S_WORDR:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &wval, 0, 1)) return;
		switch (addr & 0x3) {
			case 0: mask = 0xff000000; shift=24; break;
			case 1: mask = 0xffff0000; shift=16; break;
			case 2: mask = 0xffffff00; shift=8; break;
			case 3: mask = 0xffffffff; shift=0; break;
		}
		val <<= shift;
		wval = (wval & ~mask) | (val & mask);

		if (domem(cpu, addr & 0xfffffffc, &wval, 1, 1)) return;
	    }
	    break;

	    default:
		smoke("dostore: Illegal addressing mode");
	}
}

static 
void
abranch(struct mipscpu *cpu, uint32_t addr)
{
	CPUTRACE(DOTRACE_JUMP, cpu->cpunum, 
		 "jump: %x -> %x", cpu->nextpc-8, addr);

	if ((addr & 0x3) != 0) {
		exception(cpu, EX_ADEL, 0, addr);
		return;
	}

	// Branches update nextpc (which points to the insn after 
	// the delay slot).

	cpu->nextpc = addr;
	cpu->jumping = 1;

	/*
	 * If the instruction in the delay slot is RFE, don't call
	 * precompute_nextpc. Instead, clear the precomputed nextpc
	 * stuff and call precompute_pc once the RFE has done its
	 * thing. This is important to make sure the new PC is fetched
	 * in user mode if the RFE is switching thereto.
	 */
	if (bus_use_map(cpu->pcpage, cpu->pcoff) == FULLOP_RFE) {
		cpu->nextpcpage = NULL;
		cpu->nextpcoff = 0;
	}
	else {
		/* if this fails, no special action is required */
		precompute_nextpc(cpu);
	}
}

static
void
ibranch(struct mipscpu *cpu, uint32_t imm)
{
	// The mips book is helpfully not specific about whether the
	// address to take the upper bits of is the address of the
	// jump or the delay slot or what. it just says "the current
	// program counter", which I shall interpret as the address of
	// the delay slot. Fortunately, one isn't likely to ever be
	// executing a jump that lies across a boundary where it would
	// matter.
	//
	// (Note that cpu->pc aims at the delay slot by the time we
	// get here.)
   
	uint32_t addr = (cpu->pc & 0xf0000000) | imm;
	abranch(cpu, addr);
}

static
void
rbranch(struct mipscpu *cpu, int32_t rel)
{
	uint32_t addr = cpu->pc + rel;  // relative to addr of delay slot
	abranch(cpu, addr);
}

static
uint32_t
getstatus(struct mipscpu *cpu)
{
	uint32_t val;

	val = cpu->status_copenable;
	/* STATUS_LOWPOWER always reads 0 */
	/* STATUS_XFPU64 always reads 0 */
	/* STATUS_REVENDIAN always reads 0 */
	/* STATUS_MDMX64 always reads 0 */
	/* STATUS_MODE64 always reads 0 */
	val |= cpu->status_bootvectors;
	/* STATUS_ERRORCAUSES always reads 0 */
	/* STATUS_CACHEPARITY always reads 0 */
	/* STATUS_R3KCACHE always reads 0 */
	val |= cpu->status_hardmask_timer;
	val |= cpu->status_hardmask_void;
	val |= cpu->status_hardmask_fpu;
	val |= cpu->status_hardmask_ipi;
	val |= cpu->status_hardmask_lb;
	val |= cpu->status_softmask;

	if (cpu->old_usermode) val |= STATUS_KUo;
	if (cpu->old_irqon) val |= STATUS_IEo;
	if (cpu->prev_usermode) val |= STATUS_KUp;
	if (cpu->prev_irqon) val |= STATUS_IEp;
	if (cpu->current_usermode) val |= STATUS_KUc;
	if (cpu->current_irqon) val |= STATUS_IEc;

	return val;
}

static
void
setstatus(struct mipscpu *cpu, uint32_t val)
{
	cpu->status_copenable = val & STATUS_COPENABLE;
	/* STATUS_LOWPOWER is ignored (for now) */
	/* STATUS_XFPU64 is ignored because we aren't 64-bit */
	/* STATUS_REVENDIAN is ignored (for now) */
	/* STATUS_MDMX64 is ignored because we aren't 64-bit */
	/* STATUS_MODE64 is ignored because we aren't 64-bit */
	cpu->status_bootvectors = val & STATUS_BOOTVECTORS;
	if (val & STATUS_ERRORCAUSES) {
		/*
		 * Writing to these bits can turn them off but not on.
		 * And because for the moment we don't implement the
		 * conditions that would turn them on (instead we drop
		 * to the debugger for such things) we can just ignore
		 * the write.
		 */
	}
	/* STATUS_CACHEPARITY is ignored */
	if (val & STATUS_R3KCACHE) {
		hang("Status register write attempted to use "
		     "r2000/r3000 cache control");
	}
	cpu->status_hardmask_timer = val & STATUS_HARDMASK_TIMER;
	cpu->status_hardmask_void = val & 
		(STATUS_HARDMASK_UNUSED2 | STATUS_HARDMASK_UNUSED4);
	cpu->status_hardmask_fpu = val & STATUS_HARDMASK_FPU;
	cpu->status_hardmask_ipi = val & STATUS_HARDMASK_IPI;
	cpu->status_hardmask_lb = val & STATUS_HARDMASK_LB;
	cpu->status_softmask = val & STATUS_SOFTMASK;

	cpu->old_usermode = val & STATUS_KUo;
	cpu->old_irqon = val & STATUS_IEo;
	cpu->prev_usermode = val & STATUS_KUp;
	cpu->prev_irqon = val & STATUS_IEp;
	cpu->current_usermode = val & STATUS_KUc;
	cpu->current_irqon = val & STATUS_IEc;
}


static
uint32_t
getcause(struct mipscpu *cpu)
{
	uint32_t val;
	val = cpu->cause_ce | cpu->cause_softirq | cpu->cause_code;

	if (cpu->cause_bd) {
		val |= CAUSE_BD;
	}

	if (cpu->irq_lamebus) {
		val |= CAUSE_HARDIRQ_LB;
	}
	if (cpu->irq_ipi) {
		val |= CAUSE_HARDIRQ_IPI;
	}
	if (cpu->irq_timer) {
		val |= CAUSE_HARDIRQ_TIMER;
	}

	return val;
}

static
void
setcause(struct mipscpu *cpu, uint32_t val)
{
	/* c0_cause is read-only except for the soft irq bits */
	cpu->cause_softirq = val & CAUSE_SOFTIRQ;
}

static
uint32_t
getindex(struct mipscpu *cpu)
{
	uint32_t val = cpu->tlbindex << 8;
	if (cpu->tlbpf) {
		val |= 0x80000000;
	}
	return val;
}

static
void
setindex(struct mipscpu *cpu, uint32_t val)
{
	cpu->tlbindex = (val >> 8) & 63;
	cpu->tlbpf = val & 0x80000000;
}

static
uint32_t
getrandom(struct mipscpu *cpu)
{
	cpu->tlbrandom %= RANDREG_MAX;
	return (cpu->tlbrandom+RANDREG_OFFSET) << 8;
}

/*************************************************************/

// disassembly support
#ifdef USE_TRACE
static
const char *
regname(unsigned reg)
{
	switch (reg) {
	    case 0: return "$z0";
	    case 1: return "$at";
	    case 2: return "$v0";
	    case 3: return "$v1";
	    case 4: return "$a0";
	    case 5: return "$a1";
	    case 6: return "$a2";
	    case 7: return "$a3";
	    case 8: return "$t0";
	    case 9: return "$t1";
	    case 10: return "$t2";
	    case 11: return "$t3";
	    case 12: return "$t4";
	    case 13: return "$t5";
	    case 14: return "$t6";
	    case 15: return "$t7";
	    case 16: return "$s0";
	    case 17: return "$s1";
	    case 18: return "$s2";
	    case 19: return "$s3";
	    case 20: return "$s4";
	    case 21: return "$s5";
	    case 22: return "$s6";
	    case 23: return "$s7";
	    case 24: return "$t8";
	    case 25: return "$t9";
	    case 26: return "$k0";
	    case 27: return "$k1";
	    case 28: return "$gp";
	    case 29: return "$sp";
	    case 30: return "$s8";
	    case 31: return "$ra";
	}
	return "$??";
}

static int tracehow;		// how to trace the current instruction

#endif

#define LINK2(rg)  (cpu->r[rg] = cpu->nextpc)
#define LINK LINK2(31)

/* registers as lvalues */
#define RTx  (cpu->r[rt])
#define RSx  (cpu->r[rs])
#define RDx  (cpu->r[rd])

/* registers as signed 32-bit rvalues */
#define RTs  ((int32_t)RTx)
#define RSs  ((int32_t)RSx)
#define RDs  ((int32_t)RDx)

/* registers as unsigned 32-bit rvalues */
#define RTu  ((uint32_t)RTx)
#define RSu  ((uint32_t)RSx)
#define RDu  ((uint32_t)RDx)

/* registers as printf-able signed values */
#define RTsp  ((long)RTs)
#define RSsp  ((long)RSs)
#define RDsp  ((long)RDs)

/* registers as printf-able unsigned values */
#define RTup  ((unsigned long)RTu)
#define RSup  ((unsigned long)RSu)
#define RDup  ((unsigned long)RDu)

#define STALL { phony_exception(cpu); }
#define WHILO {if (cpu->hiwait>0 || cpu->lowait>0) { STALL; return; }}
#define WHI   {if (cpu->hiwait>0) { STALL; return; }}
#define WLO   {if (cpu->lowait>0) { STALL; return; }}
#define SETHILO(n) (cpu->hiwait = cpu->lowait = (n))
#define SETHI(n)   (cpu->hiwait = (n))
#define SETLO(n)   (cpu->lowait = (n))

#define OVF	  { exception(cpu, EX_OVF, 0, 0); }
#define CHKOVF(v) {if (((int64_t)(int32_t)(v))!=(v)) { OVF; return; }}

#define TRL(...)  CPUTRACEL(tracehow, cpu->cpunum, __VA_ARGS__)
#define TR(...)   CPUTRACE(tracehow, cpu->cpunum, __VA_ARGS__)

#define NEEDRS	 uint32_t rs = (insn & 0x03e00000) >> 21	// register
#define NEEDRT	 uint32_t rt = (insn & 0x001f0000) >> 16	// register
#define NEEDRD	 uint32_t rd = (insn & 0x0000f800) >> 11	// register
#define NEEDTARG uint32_t targ=(insn & 0x03ffffff)           // target of jump
#define NEEDSH	 uint32_t sh = (insn & 0x000007c0) >> 6	// shift count
#define NEEDCN	 uint32_t cn = (insn & 0x0c000000) >> 26	// coproc. no.
#define NEEDSEL	 uint32_t sel= (insn & 0x00000007)	     // register select
#define NEEDIMM	 uint32_t imm= (insn & 0x0000ffff)	     // immediate value
#define NEEDSMM	 NEEDIMM; int32_t smm = (int32_t)(int16_t)imm 
					       // sign-extended immediate value
#define NEEDADDR NEEDRS; NEEDSMM; uint32_t addr = RSu + (uint32_t)smm
                                                     // register+offset address


static
void
domf(struct mipscpu *cpu, int cn, unsigned reg, unsigned sel, int32_t *greg)
{
	unsigned regsel;
	
	if (cn!=0 || IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, cn, 0);
		return;
	}

	regsel = REGSEL(reg, sel);
	switch (regsel) {
	    case C0_INDEX:   *greg = getindex(cpu); break;
	    case C0_RANDOM:  *greg = getrandom(cpu); break;
	    case C0_TLBLO:   *greg = tlbgetlo(&cpu->tlbentry); break;
	    case C0_CONTEXT: *greg = cpu->ex_context; break;
	    case C0_VADDR:   *greg = cpu->ex_vaddr; break;
	    case C0_COUNT:   *greg = cpu->ex_count; break;
	    case C0_TLBHI:   *greg = tlbgethi(&cpu->tlbentry); break;
	    case C0_COMPARE: *greg = cpu->ex_compare; break;
	    case C0_STATUS:  *greg = getstatus(cpu); break;
	    case C0_CAUSE:   *greg = getcause(cpu); break;
	    case C0_EPC:     *greg = cpu->ex_epc; break;
	    case C0_PRID:    *greg = cpu->ex_prid; break;
	    case C0_CFEAT:   *greg = cpu->ex_cfeat; break;
	    case C0_IFEAT:   *greg = cpu->ex_ifeat; break;
	    case C0_CONFIG0: *greg = cpu->ex_config0; break;
	    case C0_CONFIG1: *greg = cpu->ex_config1; break;
#if 0 /* not yet */
	    case C0_CONFIG2: *greg = cpu->ex_config2; break;
	    case C0_CONFIG3: *greg = cpu->ex_config3; break;
	    case C0_CONFIG4: *greg = cpu->ex_config4; break;
	    case C0_CONFIG5: *greg = cpu->ex_config5; break;
	    case C0_CONFIG6: *greg = cpu->ex_config6; break;
	    case C0_CONFIG7: *greg = cpu->ex_config7; break;
#endif
	    default:
		exception(cpu, EX_RI, cn, 0);
		break;
	}
}

static
void
domt(struct mipscpu *cpu, int cn, int reg, int sel, int32_t greg)
{
	unsigned regsel;

	if (cn!=0 || IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, cn, 0);
		return;
	}

	regsel = REGSEL(reg, sel);
	switch (regsel) {
	    case C0_INDEX:   setindex(cpu, greg); break;
	    case C0_RANDOM:  /* read-only register */ break;
	    case C0_TLBLO:   tlbsetlo(&cpu->tlbentry, greg); break;
	    case C0_CONTEXT: cpu->ex_context = greg; break;
	    case C0_VADDR:   cpu->ex_vaddr = greg; break;
	    case C0_COUNT:   cpu->ex_count = greg; break;
	    case C0_TLBHI:   tlbsethi(&cpu->tlbentry, greg); break;
	    case C0_COMPARE:
		cpu->ex_compare = greg;
		cpu->ex_compare_used = 1;
		if (cpu->ex_count > cpu->ex_compare) {
			/* XXX is this right? */
			cpu->ex_count = 0;
		}
		if (cpu->irq_timer) {
			CPUTRACE(DOTRACE_IRQ, cpu->cpunum, "Timer irq OFF");
		}
		cpu->irq_timer = 0;
		break;
	    case C0_STATUS:  setstatus(cpu, greg); break;
	    case C0_CAUSE:   setcause(cpu, greg); break;
	    case C0_EPC:     /* read-only register */ break;
	    case C0_PRID:    /* read-only register */ break;
	    case C0_CFEAT:   /* read-only register */ break;
	    case C0_IFEAT:   /* read-only register */ break;
	    case C0_CONFIG0:
		/*
		 * Silently ignore. Note however that the
		 * CONFIG0_KSEG0_COHERE field to set the coherence
		 * type for kseg0 is theoretically supposed to be
		 * read/write.
		 */
		break;
	    case C0_CONFIG1: /* read-only register */ break;
	    case C0_CONFIG2: /* read-only register */ break;
	    case C0_CONFIG3: /* read-only register */ break;
	    case C0_CONFIG4: /* read-only register */ break;
	    case C0_CONFIG5: /* read-only register */ break;
	    case C0_CONFIG6: /* read-only register */ break;
	    case C0_CONFIG7: /* read-only register */ break;
	    default:
		exception(cpu, EX_RI, cn, 0);
		break;
	}
}

static
void
dolwc(struct mipscpu *cpu, int cn, uint32_t addr, int reg)
{
	(void)addr;
	(void)reg;
	exception(cpu, EX_CPU, cn, 0);
}

static
void
doswc(struct mipscpu *cpu, int cn, uint32_t addr, int reg)
{
	(void)addr;
	(void)reg;
	exception(cpu, EX_CPU, cn, 0);
}

static
inline
void
mx_add(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	int64_t t64;

	TRL("add %s, %s, %s: %ld + %ld -> ",
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	t64 = (int64_t)RSs + (int64_t)RTs;
	CHKOVF(t64);
	RDx = (int32_t)t64;
	TR("%ld", RDsp);
}

static
inline
void
mx_addi(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	int64_t t64;

	TRL("addi %s, %s, %lu: %ld + %ld -> ",
	    regname(rt), regname(rs), (unsigned long)imm, RSsp, (long)smm);
	t64 = (int64_t)RSs + smm;
	CHKOVF(t64);
	RTx = (int32_t)t64;
	TR("%ld", RTsp);
}

static
inline
void
mx_addiu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRS; NEEDSMM;
	TRL("addiu %s, %s, %lu: %ld + %ld -> ", 
	    regname(rt), regname(rs), (unsigned long)imm, RSsp, (long)smm);

	/* must add as unsigned, or overflow behavior is not defined */
	RTx = RSu + (uint32_t)smm;

	TR("%ld", RTsp);
}

static
inline
void
mx_addu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("addu %s, %s, %s: %ld + %ld -> ",
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	RDx = RSu + RTu;
	TR("%ld", RDsp);
}

static
inline
void
mx_and(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("and %s, %s, %s: 0x%lx & 0x%lx -> ", 
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu & RTu;
	TR("0x%lx", RDup);
}

static
inline
void
mx_andi(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDIMM;
	TRL("andi %s, %s, %lu: 0x%lx & 0x%lx -> ", 
	    regname(rt), regname(rs), (unsigned long) imm, RSup, 
	    (unsigned long) imm);
	RTx = RSu & imm;
	TR("0x%lx", RTup);
}

static
inline
void
mx_bcf(struct mipscpu *cpu, uint32_t insn)
{
	NEEDSMM; NEEDCN;
	(void)smm;
	TR("bc%df %ld", cn, (long)smm);
	exception(cpu, EX_CPU, cn, 0);
}

static
inline
void
mx_bct(struct mipscpu *cpu, uint32_t insn)
{
	NEEDSMM; NEEDCN;
	(void)smm;
	TR("bc%dt %ld", cn, (long)smm);
	exception(cpu, EX_CPU, cn, 0);
}

static
inline
void
mx_beq(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRS; NEEDSMM;
	TRL("beq %s, %s, %ld: %lu==%lu? ", 
	    regname(rs), regname(rt), (long)smm, RSup, RTup);
	if (RSu==RTu) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_bgezal(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bgezal %s, %ld: %ld>=0? ", regname(rs), (long)smm, RSsp);
	LINK;
	if (RSs>=0) {
		TR("yes");
		rbranch(cpu, smm<<2); 
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_bgez(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bgez %s, %ld: %ld>=0? ", regname(rs), (long)smm, RSsp);
	if (RSs>=0) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_bltzal(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bltzal %s, %ld: %ld<0? ", regname(rs), (long)smm, RSsp);
	LINK;
	if (RSs<0) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_bltz(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bltz %s, %ld: %ld<0? ", regname(rs), (long)smm, RSsp);
	if (RSs<0) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_bgtz(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bgtz %s, %ld: %ld>0? ", regname(rs), (long)smm, RSsp);
	if (RSs>0) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_blez(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("blez %s, %ld: %ld<=0? ", regname(rs), (long)smm, RSsp);
	if (RSs<=0) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
mx_bne(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	TRL("bne %s, %s, %ld: %lu!=%lu? ", 
	    regname(rs), regname(rt), (long)smm, RSup, RTup);
	if (RSu!=RTu) {
		TR("yes");
		rbranch(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

/*
 * Cache control. This actually does nothing, but that's adequate for
 * the time being.
 */
static
inline
void
mx_cache(struct mipscpu *cpu, uint32_t insn)
{
	NEEDADDR; NEEDRT;
	unsigned cachecode, op;
	enum { L1i, L1d, L2, L3 } cache;
	unsigned cacheway, cacheindex, cacheoffset;
	uint32_t waymask, indexmask, offsetmask;
	unsigned wayshift, indexshift;

	/*
	 * Some documentation says that this instruction is kernel-
	 * only. Other documentation does not say (and even fails to
	 * state that either EX_CPU or EX_RI can be triggered) but
	 * it's clear that at least some of the cache ops cannot be
	 * allowed to unprivileged code. So, better to be safe.
	 */
	if (IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, 0 /*cop0*/, 0);
		return;
	}

	/*
	 * The caches we reported in the config1 register are 4K,
	 * 4-way, with 16-byte cache lines; that is, 64 sets.
	 * Therefore, the index is 6 bits, the way when we need to
	 * specify it explicitly is 2 bits, and the offset is 4 bits.
	 *
	 * XXX we should have the whole thing parameterized by
	 * preprocessor macros, or even configured at runtime.
	 *
	 * XXX also this should come after we pick the cache, inasmuch
	 * as the parameters ought to be different for L2 and L3. If
	 * we have L2 and L3 - a mips of the vintage we're kinda still
	 * pretending to be wouldn't.
	 */
	waymask = 0x00000c00;
	indexmask = 0x000003f0;
	offsetmask = 0x0000000f;
	wayshift = 10;
	indexshift = 4;

	/* the RT field here is a constant rather than a register number */
	cachecode = rt >> 3;
	op = rt & 7;

	/* XXX should have symbolic constants for this */
	switch (cachecode) {
	    case 0: cache = L1i; break; /* L1 icache */
	    case 1: cache = L1d; break; /* L1 dcache or unified L1 */
	    case 2: cache = L3; break; /* L3 */
	    case 3: cache = L2; break; /* L2 */
	}

	switch (op) {
	    case 0:
	    case 1:
	    case 2:
	    case 3:
		/* address the cache by index */
		cacheway = (addr & waymask) >> wayshift;
		cacheindex = (addr & indexmask) >> indexshift;
		cacheoffset = (addr & offsetmask);
		break;
	    case 4:
	    case 5:
	    case 6:
	    case 7:
		/* address the cache by address */
		if (translatemem(cpu, addr, 0, &addr) < 0) {
			return;
		}
		cacheway = 0; /* make compiler happy; need to check all ways */
		cacheindex = (addr & indexmask) >> indexshift;
		cacheoffset = (addr & offsetmask);
		break;
	}

	switch (op) {
	    case 0: /* index writeback & invalidate */
		/*
		 * Look at cache[cacheindex].ways[cacheway]; write it back
		 * if needed, then invalidate it. (For the icache, just
		 * invalidate it.)
		 */
		(void)cache;
		(void)cacheway;
		(void)cacheindex;
		break;
	    case 1: /* index load tag */
		/*
		 * Look at cache[cacheindex].ways[cacheway]; read the
		 * tag into the TagLo and TagHi registers; use the
		 * offset to read the data out of the cache line into
		 * the DataLo and DataHi registers. Note: we don't
		 * have any of these registers, and DataLo/DataHi are
		 * optional.
		 *
		 * The sub-word bits of the offset, if any, should be
		 * ignored rather than kvetched about.
		 */
		(void)cache;
		(void)cacheindex;
		(void)cacheway;
		(void)cacheoffset;
		break;
	    case 2: /* index store tag */
		/*
		 * Look at cache[cacheindex].ways[cacheway]; write the
		 * tag from the TagLo and TagHi registers.
		 */
		(void)cache;
		(void)cacheindex;
		(void)cacheway;
		break;
	    case 3: /* implementation-defined operation */
		break;
	    case 4: /* hit invalidate */
		/*
		 * Look at cache[cacheindex]; check all ways for a
		 * matching tag, and if found invalidate it without
		 * writing back.
		 */
		(void)cache;
		(void)cacheindex;
		break;
	    case 5: /* hit writeback & invalidate */
		if (cache == L1i) {
			/*
			 * For the L1 cache this op is "fill" (!)
			 * Load one of the ways at cache[cacheindex]
			 * from memory using the specified address.
			 */
			(void)cache;
			(void)cacheindex;
			(void)addr;
		}
		else {
			/*
			 * Look at cache[cacheindex]; check all ways for
			 * a matching tag, and if found, write it back if
			 * necessary and then invalidate it.
			 */
			(void)cache;
			(void)cacheindex;
		}
		break;
	    case 6: /* hit writeback */
		/*
		 * Look at cache[cacheindex]; check all ways for a
		 * matching tag, and if found, write it back if
		 * necessary. Leave it valid. A nop for the icache, I
		 * guess.
		 */
		(void)cache;
		(void)cacheindex;
		break;
	    case 7: /* fetch and lock */
		/*
		 * Look at cache[cacheindex]; load one of the ways
		 * from memory using the specified address, and lock
		 * it in. It then won't be kicked out unless
		 * explicitly invalidated, or revoked from another
		 * processor. (Or if the lock bit is cleared by
		 * mucking directly with the tag.)
		 */
		(void)cache;
		(void)cacheindex;
		(void)addr;
		break;
	}
}

static
inline
void
mx_cf(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN;
	(void)rt;
	(void)rd;
	TR("cfc%d %s, $%u", cn, regname(rt), rd);
	exception(cpu, EX_CPU, cn, 0);
}

static
inline
void
mx_ct(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN;
	(void)rt;
	(void)rd;
	TR("ctc%d %s, $%u", cn, regname(rt), rd);
	exception(cpu, EX_CPU, cn, 0);
}

static
inline
void
mx_j(struct mipscpu *cpu, uint32_t insn)
{
	NEEDTARG;
	TR("j 0x%lx", (unsigned long)(targ<<2));
	ibranch(cpu, targ<<2);
}

static
inline
void
mx_jal(struct mipscpu *cpu, uint32_t insn)
{
	NEEDTARG;
	TR("jal 0x%lx", (unsigned long)(targ<<2));
	LINK;
	ibranch(cpu, targ<<2);
#ifdef USE_TRACE
	prof_call(cpu->pc, cpu->nextpc);
#endif
}

static
inline
void
mx_lb(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lb %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	doload(cpu, S_SBYTE, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
mx_lbu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lbu %s, %ld(%s): [0x%lx] -> ",
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	doload(cpu, S_UBYTE, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
mx_lh(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lh %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	doload(cpu, S_SHALF, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
mx_lhu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lhu %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	doload(cpu, S_UHALF, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
mx_ll(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("ll %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	if (domem(cpu, addr, (uint32_t *) &RTx, 0, 0)) {
		/* exception */
		return;
	}

	/* load linked: just save what we did */
	cpu->ll_active = 1;
	cpu->ll_addr = addr;
	cpu->ll_value = RTs;

	g_stats.s_percpu[cpu->cpunum].sp_lls++;

	TR("%ld", RTsp);
}

static
inline
void
mx_lui(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDIMM;
	TR("lui %s, 0x%x", regname(rt), imm);
	RTx = imm << 16;
}

static
inline
void
mx_lw(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lw %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	domem(cpu, addr, (uint32_t *) &RTx, 0, 0);
	TR("%ld", RTsp);
}

static
inline
void
mx_lwc(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR; NEEDCN;
	TR("lwc%d $%u, %ld(%s)", cn, rt, (long)smm, regname(rs));
	dolwc(cpu, cn, addr, rt);
}

static
inline
void
mx_lwl(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lwl %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	doload(cpu, S_WORDL, addr, (uint32_t *) &RTx);
	TR("0x%lx", RTup);
}

static
inline
void
mx_lwr(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lwr %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	doload(cpu, S_WORDR, addr, (uint32_t *) &RTx);
	TR("0x%lx", RTup);
}

static
inline
void
mx_sb(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("sb %s, %ld(%s): %d -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs),
	   (int)(RTu&0xff), (unsigned long)addr);
	dostore(cpu, S_UBYTE, addr, RTu);
}

static
inline
void
mx_sc(struct mipscpu *cpu, uint32_t insn)
{
	uint32_t temp;
	NEEDRT; NEEDADDR;
	TR("sw %s, %ld(%s): %ld -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTsp, (unsigned long)addr);

	/*
	 * Store conditional.
	 *
	 * This implementation uses the observation that if the target
	 * memory address contains the same value we loaded earlier
	 * with LL, then execution is completely equivalent to
	 * performing an atomic operation that reads it now. This is
	 * true even if the target memory has been written to in
	 * between, even though allowing that is formally against the
	 * spec. Other operations that might allow distinguishing
	 * such a case, such as reading other memory regions with or
	 * without using LL, lead to the behavior of the SC being
	 * formally unpredictable or undefined.
	 *
	 * (Since a number of people have missed or misunderstood this
	 * point: it is NOT ALLOWED to do other memory accesses
	 * between the LL and SC. If you do so, the SC no longer
	 * guarantees you an atomic operation; it might fail if
	 * another CPU has accessed the memory you LL'd, but it might
	 * not. Or it might always fail. This is how LL/SC is defined,
	 * on MIPS and other architectures, not a "bug" in System/161.
	 * If you want to make multiple memory accesses atomic, you
	 * need a transactional memory system; good luck with that.)
	 *
	 * The address of the SC must be the same as the LL. That's
	 * supposed to mean both vaddr and paddr, as well as caching
	 * mode and any memory bus consistency mode. If they don't
	 * match, the behavior of the SC is undefined. We check the
	 * vaddr and fail if it doesn't match; if someone does
	 * something reckless to make the vaddr but not the paddr
	 * match, they deserve the consequences. (This can only happen
	 * in kernel mode; changing MMU mappings from user mode
	 * requires a trap, which invalidates the LL.)
	 *
	 * So:
	 *
	 * 1. If we don't have an LL active, SC fails.
	 * 2. If the target vaddr is not the same as the one on file
	 *    from LL, SC fails.
	 * 3. We reread from the address; if the read fails, we
	 *    have taken an exception, so just return.
	 * 4. If the result value is different from the one on file
	 *    from LL, SC fails.
	 * 5. We write to the address; if the write fails, we have
	 *    taken an exception, so just return.
	 * 6. SC succeeds.
	 */

	if (!cpu->ll_active) {
		goto fail;
	}
	if (cpu->ll_addr != addr) {
		goto fail;
	}
	if (domem(cpu, addr, &temp, 0, 1)) {
		/* exception */
		return;
	}
	if (temp != cpu->ll_value) {
		goto fail;
	}
	if (domem(cpu, addr, (uint32_t *) &RTx, 1, 1)) {
		/* exception */
		return;
	}
	/* success */
	RTx = 1;
	g_stats.s_percpu[cpu->cpunum].sp_okscs++;
	return;

 fail:
	/* failure */
	RTx = 0;
	g_stats.s_percpu[cpu->cpunum].sp_badscs++;
}

static
inline
void
mx_sh(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("sh %s, %ld(%s): %d -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs),
	   (int)(RTu&0xffff), (unsigned long)addr);
	dostore(cpu, S_UHALF, addr, RTu);
}

static
inline
void
mx_sw(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("sw %s, %ld(%s): %ld -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTsp, (unsigned long)addr);
	domem(cpu, addr, (uint32_t *) &RTx, 1, 1);
}

static
inline
void
mx_swc(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR; NEEDCN;
	TR("swc%d $%u, %ld(%s)", cn, rt, (long)smm, regname(rs));
	doswc(cpu, cn, addr, rt);
}

static
inline
void
mx_swl(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("swl %s, %ld(%s): 0x%lx -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTup, (unsigned long)addr);
	dostore(cpu, S_WORDL, addr, RTu);
}

static
inline
void
mx_swr(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("swr %s, %ld(%s): 0x%lx -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTup, (unsigned long)addr);
	dostore(cpu, S_WORDR, addr, RTu);
}

static
inline
void
mx_break(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("break");
	exception(cpu, EX_BP, 0, 0);
}

static
inline
void
mx_div(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	TRL("div %s %s: %ld / %ld -> ", 
	    regname(rs), regname(rt), RSsp, RTsp);

	WHILO;
	if (RTs==0) {
		/*
		 * On divide-by-zero the mips doesn't trap.
		 * Instead, the assembler emits an integer check for
		 * zero that (on the real chip) runs in parallel with
		 * the divide unit.
		 *
		 * I don't know what the right values to load in the
		 * result are, if there are any that are specified,
		 * but I'm going to make up what seems like a good
		 * excuse for machine infinity.
		 */
		if (RSs < 0) {
			cpu->lo = 0xffffffff;
		}
		else {
			cpu->lo = 0x7fffffff;
		}
		cpu->hi = 0;
		TR("ERR");
	}
	else {
		cpu->lo = RSs/RTs;
		cpu->hi = RSs%RTs;
		TR("%ld, remainder %ld", 
		   (long)(int32_t)cpu->lo, (long)(int32_t)cpu->hi);
	}
	SETHILO(2);
}

static
inline
void
mx_divu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	TRL("divu %s %s: %lu / %lu -> ", 
	    regname(rs), regname(rt), RSup, RTup);

	WHILO;
	if (RTu==0) {
		/*
		 * See notes under signed divide above.
		 */
		cpu->lo = 0xffffffff;
		cpu->hi = 0;
		TR("ERR");
	}
	else {
		cpu->lo=RSu/RTu;
		cpu->hi=RSu%RTu;
		TR("%lu, remainder %lu", 
		   (unsigned long)(uint32_t)cpu->lo, 
		   (unsigned long)(uint32_t)cpu->hi);
	}
	SETHILO(2);
}

static
inline
void
mx_jr(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS;
	TR("jr %s: 0x%lx", regname(rs), RSup);
	abranch(cpu, RSu);
}

static
inline
void
mx_jalr(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRD;
	TR("jalr %s, %s: 0x%lx", regname(rd), regname(rs), RSup);
	LINK2(rd);
	abranch(cpu, RSu);
#ifdef USE_TRACE
	prof_call(cpu->pc, cpu->nextpc);
#endif
}

static
inline
void
mx_mf(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN; NEEDSEL;
	if (sel) {
		TRL("mfc%d %s, $%u.%u: ... -> ", cn, regname(rt), rd, sel);
	}
	else {
		TRL("mfc%d %s, $%u: ... -> ", cn, regname(rt), rd);
	}
	domf(cpu, cn, rd, sel, &RTx);
	TR("0x%lx", RTup);
}

static
inline
void
mx_mfhi(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD;
	TRL("mfhi %s: ... -> ", regname(rd));
	WHI;
	RDx = cpu->hi;
	SETHI(2);
	TR("0x%lx", RDup);
}

static
inline
void
mx_mflo(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD;
	TRL("mflo %s: ... -> ", regname(rd));
	WLO;
	RDx = cpu->lo;
	SETLO(2);
	TR("0x%lx", RDup);
}

static
inline
void
mx_mt(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN; NEEDSEL;
	if (sel) {
		TR("mtc%d %s, $%u.%u: 0x%lx -> ...", cn, regname(rt), rd, sel,
		   RTup);
	}
	else {		
		TR("mtc%d %s, $%u: 0x%lx -> ...", cn, regname(rt), rd, RTup);
	}
	domt(cpu, cn, rd, sel, RTs);
}

static
inline
void
mx_mthi(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS;
	TR("mthi %s: 0x%lx -> ...", regname(rs), RSup);
	WHI;
	cpu->hi = RSu;
	SETHI(2);
}

static
inline
void
mx_mtlo(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS;
	TR("mtlo %s: 0x%lx -> ...", regname(rs), RSup);
	WLO;
	cpu->lo = RSu;
	SETLO(2);
}

static
inline
void
mx_mult(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	int64_t t64;
	TRL("mult %s, %s: %ld * %ld -> ", 
	    regname(rs), regname(rt), RSsp, RTsp);
	WHILO;
	t64=(int64_t)RSs*(int64_t)RTs;
	cpu->hi = (((uint64_t)t64)&0xffffffff00000000ULL) >> 32;
	cpu->lo = (uint32_t)(((uint64_t)t64)&0x00000000ffffffffULL);
	SETHILO(2);
	TR("%ld %ld", (long)(int32_t)cpu->hi, (long)(int32_t)cpu->lo);
}

static
inline
void
mx_multu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	uint64_t t64;
	TRL("multu %s, %s: %lu * %lu -> ", 
	    regname(rs), regname(rt), RSup, RTup);
	WHILO;
	t64=(uint64_t)RSu*(uint64_t)RTu;
	cpu->hi = (t64&0xffffffff00000000ULL) >> 32;
	cpu->lo = (uint32_t)(t64&0x00000000ffffffffULL);
	SETHILO(2);
	TR("%lu %lu",
	   (unsigned long)(uint32_t)cpu->hi,
	   (unsigned long)(uint32_t)cpu->lo);
}

static
inline
void
mx_nor(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("nor %s, %s, %s: ~(0x%lx | 0x%lx) -> ",
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = ~(RSu | RTu);
	TR("0x%lx", RDup);
}

static
inline
void
mx_or(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("or %s, %s, %s: 0x%lx | 0x%lx -> ", 
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu | RTu;
	TR("0x%lx", RDup);
}

static
inline
void
mx_ori(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDIMM;
	TRL("ori %s, %s, %lu: 0x%lx | 0x%lx -> ", 
	    regname(rt), regname(rs), (unsigned long)imm,
	    RSup, (unsigned long)imm);
	RTx = RSu | imm;
	TR("0x%lx", RTup);
}

static
inline
void
mx_rfe(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("rfe");
	do_rfe(cpu);
}

static
inline
void
mx_sll(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDSH;
	TRL("sll %s, %s, %u: 0x%lx << %u -> ", 
	    regname(rd), regname(rt), (unsigned)sh, RTup, (unsigned)sh);
	RDx = RTu << sh;
	TR("0x%lx", RDup);
}

static
inline
void
mx_sllv(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDRS;
	unsigned vsh = (RSu&31);
	TRL("sllv %s, %s, %s: 0x%lx << %u -> ", 
	    regname(rd), regname(rt), regname(rs), RTup, vsh);
	RDx = RTu << vsh;
	TR("0x%lx", RDup);
}

static
inline
void
mx_slt(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("slt %s, %s, %s: %ld < %ld -> ", 
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	RDx = RSs < RTs;
	TR("%ld", RDsp);
}

static
inline
void
mx_slti(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	TRL("slti %s, %s, %ld: %ld < %ld -> ", 
	    regname(rt), regname(rs), (long)smm, RSsp, (long)smm);
	RTx = RSs < smm;
	TR("%ld", RTsp);
}

static
inline
void
mx_sltiu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	TRL("sltiu %s, %s, %lu: %lu < %lu -> ", 
	    regname(rt), regname(rs), (unsigned long)imm, RSup, 
	    (unsigned long)(uint32_t)smm);
	// Yes, the immediate is sign-extended then treated as
	// unsigned, according to my mips book. Blech.
	RTx = RSu < (uint32_t) smm;
	TR("%ld", RTsp);
}

static
inline
void
mx_sltu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("sltu %s, %s, %s: %lu < %lu -> ", 
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu < RTu;
	TR("%ld", RDsp);
}

static
inline
uint32_t
signedshift(uint32_t val, unsigned amt)
{
	/* There's no way to express a signed shift directly in C. */
	uint32_t result;
	result = val >> amt;
	if (val & 0x80000000) {
		result |= (0xffffffff << (31-amt));
	}
	return result;
}

static
inline
void
mx_sra(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDSH;
	TRL("sra %s, %s, %u: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), (unsigned)sh, RTup, (unsigned)sh);
	RDx = signedshift(RTu, sh);
	TR("0x%lx", RDup);
}

static
inline
void
mx_srav(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	unsigned vsh = (RSu&31);
	TRL("srav %s, %s, %s: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), regname(rs), RTup, vsh);
	RDx = signedshift(RTu, vsh);
	TR("0x%lx", RDup);
}

static
inline
void
mx_srl(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDSH;
	TRL("srl %s, %s, %u: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), (unsigned)sh, RTup, (unsigned)sh);
	RDx = RTu >> sh;
	TR("0x%lx", RDup);
}

static
inline
void
mx_srlv(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	unsigned vsh = (RSu&31);
	TRL("srlv %s, %s, %s: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), regname(rs), RTup, vsh);
	RDx = RTu >> vsh;
	TR("0x%lx", RDup);
}

static
inline
void
mx_sub(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	int64_t t64;
	TRL("sub %s, %s, %s: %ld - %ld -> ", 
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	t64 = (int64_t)RSs - (int64_t)RTs;
	CHKOVF(t64);
	RDx = (int32_t)t64;
	TR("%ld", RDsp);
}

static
inline
void
mx_subu(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("subu %s, %s, %s: %ld - %ld -> ", 
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	RDx = RSu - RTu;
	TR("%ld", RDsp);
}

static
inline
void
mx_sync(struct mipscpu *cpu, uint32_t insn)
{
	/* flush pending memory accesses; for now nothing needed */
	(void)cpu;
	(void)insn;
	TR("sync");
	g_stats.s_percpu[cpu->cpunum].sp_syncs++;
}

static
inline
void
mx_syscall(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("syscall");
	exception(cpu, EX_SYS, 0, 0);
}

static
inline
void
mx_tlbp(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbp");
	probetlb(cpu);
}

static
inline
void
mx_tlbr(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbr");
	cpu->tlbentry = cpu->tlb[cpu->tlbindex];
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, "tlbr:  [%2d] ", cpu->tlbindex);
	TLBTR(&cpu->tlbentry);
	CPUTRACE(DOTRACE_TLB, cpu->cpunum, " ");
}

static
inline
void
mx_tlbwi(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbwi");
	writetlb(cpu, cpu->tlbindex, "tlbwi");
}

static
inline
void
mx_tlbwr(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbwr");
	cpu->tlbrandom %= RANDREG_MAX;
	writetlb(cpu, cpu->tlbrandom+RANDREG_OFFSET, "tlbwr");
}

static
inline
void
mx_wait(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("wait");
	do_wait(cpu);
}

static
inline
void
mx_xor(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("xor %s, %s, %s: 0x%lx ^ 0x%lx -> ",
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu ^ RTu;
	TR("0x%lx", RDup);
}

static
inline
void
mx_xori(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDIMM;
	TRL("xori %s, %s, %lu: 0x%lx ^ 0x%lx -> ",
	    regname(rt), regname(rs), (unsigned long)imm, 
	    RSup, (unsigned long)imm);
	RTx = RSu ^ imm;
	TR("0x%lx", RTup);
}

static
inline
void
mx_ill(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("[illegal instruction %08lx]", (unsigned long) insn);
	exception(cpu, EX_RI, 0, 0);
}

/*
 * Note: OP_WAIT is not defined for mips r2000/r3000 - it's from later
 * MIPS versions. However, we support it here anyway because spinning
 * in an idle loop is just plain stupid.
 */
static
inline
void
mx_copz(struct mipscpu *cpu, uint32_t insn)
{
	NEEDCN;
	uint32_t copop;

	if (cn!=0) {
		exception(cpu, EX_CPU, cn, 0);
		return;
	}
	if (IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, cn, 0);
		return;
	}

	copop = (insn & 0x03e00000) >> 21;	// coprocessor opcode

	if (copop & 0x10) {
		copop = (insn & 0x01ffffff);	// real coprocessor opcode
		switch (copop) {
		    case 1: mx_tlbr(cpu, insn); break;
		    case 2: mx_tlbwi(cpu, insn); break;
		    case 6: mx_tlbwr(cpu, insn); break;
		    case 8: mx_tlbp(cpu, insn); break;
		    case 16: mx_rfe(cpu, insn); break;
		    case 32: mx_wait(cpu, insn); break;
		    default: mx_ill(cpu, insn); break;
		}
	}
	else switch (copop) {
	    case 0: mx_mf(cpu, insn); break;
	    case 2: mx_cf(cpu, insn); break;
	    case 4: mx_mt(cpu, insn); break;
	    case 6: mx_ct(cpu, insn); break;
	    case 8:
	    case 12:
		if (insn & 0x00010000) {
			mx_bcf(cpu, insn);
		}
		else {
			mx_bct(cpu, insn);
		}
		break;
	    default: mx_ill(cpu, insn);
	}
}

static
int
cpu_cycle(void)
{
	uint32_t insn;
	uint32_t op;
	unsigned whichcpu;
	unsigned breakpoints = 0;
	uint32_t retire_pc;
	unsigned retire_usermode;

	for (whichcpu=0; whichcpu < ncpus; whichcpu++) {
		struct mipscpu *cpu = &mycpus[whichcpu];

		if (cpu->state != CPU_RUNNING) {
			// don't check this on the critical path
			//Assert((cpu_running_mask & thiscpumask) == 0);
			g_stats.s_percpu[cpu->cpunum].sp_icycles++;
			continue;
		}

	/* INDENT HORROR BEGIN */

	/*
	 * First, update exception PC.
	 *
	 * Once we've done this, we can take exceptions this cycle and
	 * have them report the correct PC.
	 *
	 * If we're executing the delay slot of a jump, expc remains pointing
	 * to the jump and we clear the flag that tells us to do this.
	 */
	if (cpu->jumping) {
		cpu->jumping = 0;
		cpu->in_jumpdelay = 1;
	}
	else {
		cpu->expc = cpu->pc;
	}

	/*
	 * Check for interrupts.
	 */
	if (cpu->current_irqon) {
		uint32_t soft = cpu->status_softmask & cpu->cause_softirq;
		int lb = cpu->irq_lamebus && cpu->status_hardmask_lb;
		int ipi = cpu->irq_ipi && cpu->status_hardmask_ipi;
		int timer = cpu->irq_timer && cpu->status_hardmask_timer;

		if (lb || ipi || timer || soft) {
			CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
				 "Taking interrupt:%s%s%s%s",
				 lb ? " LAMEbus" : "",
				 ipi ? " IPI" : "",
				 timer ? " timer" : "",
				 soft ? " soft" : "");
			exception(cpu, EX_IRQ, 0, 0);
			/*
			 * Start processing the interrupt this cycle.
			 *
			 * That is, repeat the code above us in this
			 * function.
			 *
			 * At this point we're executing the first 
			 * instruction of the exception handler, which
			 * cannot be a jump delay slot.
			 *
			 * Thus, just set expc.
			 */
			cpu->expc = cpu->pc;
		}
	}

	if (IS_USERMODE(cpu)) {
		g_stats.s_percpu[cpu->cpunum].sp_ucycles++;
#ifdef USE_TRACE
		tracehow = DOTRACE_UINSN;
#endif
	}
	else {
		g_stats.s_percpu[cpu->cpunum].sp_kcycles++;
#ifdef USE_TRACE
		tracehow = DOTRACE_KINSN;
#endif
	}

	/*
	 * If at the end of all the following logic, the PC (which
	 * will hold the address of the next instruction to execute)
	 * is still what's currently nextpc, we haven't taken an
	 * exception and that means we've retired an instruction. We
	 * need to record whether it was user or kernel here, because
	 * otherwise if we switch modes (e.g. with RFE) it'll be
	 * credited incorrectly.
	 */
	retire_pc = cpu->nextpc;
	retire_usermode = IS_USERMODE(cpu);

	/*
	 * Fetch instruction.
	 *
	 * We cache the page translation for the PC.
	 * Use the page part of the precomputed physpc and also the
	 * precomputed page pointer.
	 *
	 * Note that as a result of precomputing everything, exceptions
	 * related to PC mishaps occur at jump time, or possibly when
	 * *nextpc* crosses a page boundary (below) or whatnot, never 
	 * during instruction fetch itself. I believe this is acceptable
	 * behavior to exhibit.
	 */
	insn = bus_use_map(cpu->pcpage, cpu->pcoff);

	// Update PC. 
	cpu->pc = cpu->nextpc;
	cpu->pcoff = cpu->nextpcoff;
	cpu->pcpage = cpu->nextpcpage;
	cpu->nextpc += 4;
	if ((cpu->nextpc & 0xfff)==0) {
		/* crossed page boundary */
		if (insn == FULLOP_RFE) {
			/* defer precompute_nextpc() */
			cpu->nextpcpage = NULL;
			cpu->nextpcoff = 0;
		}
		else if (precompute_nextpc(cpu)) {
			/* exception. on to next cpu. */
			continue;
		}
	}
	else {
		cpu->nextpcoff += 4;
	}

	TRL("at %08x: ", cpu->expc);
	
	/*
	 * Decode instruction.
	 */

	cpu->hit_breakpoint = 0;
	
	op = (insn & 0xfc000000) >> 26;   // opcode

	switch (op) {
	    case OPM_SPECIAL:
		// use function field
		switch (insn & 0x3f) {
		    case OPS_SLL: mx_sll(cpu, insn); break;
		    case OPS_SRL: mx_srl(cpu, insn); break;
		    case OPS_SRA: mx_sra(cpu, insn); break;
		    case OPS_SLLV: mx_sllv(cpu, insn); break;
		    case OPS_SRLV: mx_srlv(cpu, insn); break;
		    case OPS_SRAV: mx_srav(cpu, insn); break;
		    case OPS_JR: mx_jr(cpu, insn); break;
		    case OPS_JALR: mx_jalr(cpu, insn); break;
		    case OPS_SYSCALL: mx_syscall(cpu, insn); break;
		    case OPS_BREAK: 
			/*
			 * If we're in the range that we can debug in (that
			 * is, not the TLB-mapped segments), activate the
			 * kernel debugging hooks.
			 */
			if (gdb_canhandle(cpu->expc)) {
				phony_exception(cpu);
				cpu_stopcycling();
				main_enter_debugger(0 /* not lethal */);
				/*
				 * Don't bill time for hitting the breakpoint.
				 */
				breakpoints++;
				cpu->ex_count--;
				cpu->hit_breakpoint = 1;
				continue;
			}
			mx_break(cpu, insn);
			break;
		    case OPS_SYNC: mx_sync(cpu, insn); break;
		    case OPS_MFHI: mx_mfhi(cpu, insn); break;
		    case OPS_MTHI: mx_mthi(cpu, insn); break;
		    case OPS_MFLO: mx_mflo(cpu, insn); break;
		    case OPS_MTLO: mx_mtlo(cpu, insn); break;
		    case OPS_MULT: mx_mult(cpu, insn); break;
		    case OPS_MULTU: mx_multu(cpu, insn); break;
		    case OPS_DIV: mx_div(cpu, insn); break;
		    case OPS_DIVU: mx_divu(cpu, insn); break;
		    case OPS_ADD: mx_add(cpu, insn); break;
		    case OPS_ADDU: mx_addu(cpu, insn); break;
		    case OPS_SUB: mx_sub(cpu, insn); break;
		    case OPS_SUBU: mx_subu(cpu, insn); break;
		    case OPS_AND: mx_and(cpu, insn); break;
		    case OPS_OR: mx_or(cpu, insn); break;
		    case OPS_XOR: mx_xor(cpu, insn); break;
		    case OPS_NOR: mx_nor(cpu, insn); break;
		    case OPS_SLT: mx_slt(cpu, insn); break;
		    case OPS_SLTU: mx_sltu(cpu, insn); break;
		    default: mx_ill(cpu, insn); break;
		}
		break;
	    case OPM_BCOND:
		// use rt field
		switch ((insn & 0x001f0000) >> 16) {
		    case 0: mx_bltz(cpu, insn); break;
		    case 1: mx_bgez(cpu, insn); break;
		    case 16: mx_bltzal(cpu, insn); break;
		    case 17: mx_bgezal(cpu, insn); break;
		    default: mx_ill(cpu, insn); break;
		}
		break;
	    case OPM_J: mx_j(cpu, insn); break;
	    case OPM_JAL: mx_jal(cpu, insn); break;
	    case OPM_BEQ: mx_beq(cpu, insn); break;
	    case OPM_BNE: mx_bne(cpu, insn); break;
	    case OPM_BLEZ: mx_blez(cpu, insn); break;
	    case OPM_BGTZ: mx_bgtz(cpu, insn); break;
	    case OPM_ADDI: mx_addi(cpu, insn); break;
	    case OPM_ADDIU: mx_addiu(cpu, insn); break;
	    case OPM_SLTI: mx_slti(cpu, insn); break;
	    case OPM_SLTIU: mx_sltiu(cpu, insn); break;
	    case OPM_ANDI: mx_andi(cpu, insn); break;
	    case OPM_ORI: mx_ori(cpu, insn); break;
	    case OPM_XORI: mx_xori(cpu, insn); break;
	    case OPM_LUI: mx_lui(cpu, insn); break;
	    case OPM_COP0:
	    case OPM_COP1:
	    case OPM_COP2:
	    case OPM_COP3: mx_copz(cpu, insn); break;
	    case OPM_LB: mx_lb(cpu, insn); break;
	    case OPM_LH: mx_lh(cpu, insn); break;
	    case OPM_LWL: mx_lwl(cpu, insn); break;
	    case OPM_LW: mx_lw(cpu, insn); break;
	    case OPM_LBU: mx_lbu(cpu, insn); break;
	    case OPM_LHU: mx_lhu(cpu, insn); break;
	    case OPM_LWR: mx_lwr(cpu, insn); break;
	    case OPM_SB: mx_sb(cpu, insn); break;
	    case OPM_SH: mx_sh(cpu, insn); break;
	    case OPM_SWL: mx_swl(cpu, insn); break;
	    case OPM_SW: mx_sw(cpu, insn); break;
	    case OPM_SWR: mx_swr(cpu, insn); break;
	    case OPM_CACHE: mx_cache(cpu, insn); break;
	    case OPM_LWC0: /* LWC0 == LL */ mx_ll(cpu, insn); break;
	    case OPM_LWC1:
	    case OPM_LWC2:
	    case OPM_LWC3: mx_lwc(cpu, insn); break;
	    case OPM_SWC0: /* SWC0 == SC */ mx_sc(cpu, insn); break;
	    case OPM_SWC1:
	    case OPM_SWC2:
	    case OPM_SWC3: mx_swc(cpu, insn); break;
	    default: mx_ill(cpu, insn); break;
	}

	/* Timer. Take interrupt on next cycle; call it a pipeline effect. */
	cpu->ex_count++;
	if (cpu->ex_compare_used && cpu->ex_count == cpu->ex_compare) {
		cpu->ex_count = 0; /* XXX is this right? */
		cpu->irq_timer = 1;
		CPUTRACE(DOTRACE_IRQ, cpu->cpunum, "Timer irq ON");
	}

	if (cpu->lowait > 0) {
		cpu->lowait--;
	}
	if (cpu->hiwait > 0) {
		cpu->hiwait--;
	}

	cpu->in_jumpdelay = 0;
	
	cpu->tlbrandom++;

	/*
	 * If the PC (which is the instruction we're going to execute
	 * on the next cycle) is still what it was saved as above,
	 * meaning we aren't jumping to an exception vector, we've
	 * retired an instruction. If in user mode, we've made
	 * progress.
	 *
	 * Note that it's important to claim progress only when we've
	 * retired an instruction; just spending a cycle in user mode
	 * doesn't count as it's possible to set up livelock
	 * conditions where user-mode instructions are started
	 * regularly but never complete.
	 */
	if (cpu->pc == retire_pc) {
		if (retire_usermode) {
			g_stats.s_percpu[cpu->cpunum].sp_uretired++;
			progress = 1;
		}
		else {
			g_stats.s_percpu[cpu->cpunum].sp_kretired++;
		}
	}

	/* INDENT HORROR END */

	}

	if (breakpoints == 0) {
		return 1;
	}
	if (breakpoints == ncpus) {
		return 1;
	}

	/*
	 * Some CPUs took a cycle, but one or more hit a builtin
	 * breakpoint and didn't.
	 *
	 * Ideally we should roll back the ones that did, or account
	 * for the time properly in some other fashion. Or stop using
	 * a global clock.
	 *
	 * For the time being, slip the clock. This makes builtin
	 * breakpoints not quite noninvasive on multiprocessor
	 * configs. Sigh.
	 */
	return 0;
}

static int cpu_cycling;

uint64_t
cpu_cycles(uint64_t maxcycles)
{
	uint64_t i;

	cpu_cycling = 1;
	i = 0;
	while (i < maxcycles && cpu_cycling) {
		if (cpu_cycle()) {
			i++;
			cpu_cycles_count = i;
		}
		if (cpu_running_mask == 0) {
			/* nothing occurs until we reach maxcycles */
			if (cpu_cycling) {
				i = maxcycles;
			}
		}
	}
	cpu_cycles_count = 0;
	return i;
}

void
cpu_stopcycling(void)
{
	cpu_cycling = 0;
}

/*************************************************************/

void
cpu_init(unsigned numcpus)
{
	unsigned i;

	Assert(numcpus <= 32);

	ncpus = numcpus;
	mycpus = domalloc(ncpus * sizeof(*mycpus));
	for (i=0; i<numcpus; i++) {
		mips_init(&mycpus[i], i);
	}

	mycpus[0].state = CPU_RUNNING;
	cpu_running_mask = 0x1;
}

void
cpu_dumpstate(void)
{
	struct mipscpu *cpu;
	int i;
	unsigned j;

	msg("%u cpus: MIPS r3000", ncpus);
	for (j=0; j<ncpus; j++) {
		cpu = &mycpus[j];
		msg("cpu %d:", j);

	/* BEGIN INDENT HORROR */

	for (i=0; i<NREGS; i++) {
		msgl("r%d:%s 0x%08lx  ", i, i<10 ? " " : "",
		     (unsigned long)(uint32_t) cpu->r[i]);
		if (i%4==3) {
			msg(" ");
		}
	}
	msg("lo:  0x%08lx  hi:  0x%08lx  pc:  0x%08lx  npc: 0x%08lx", 
	    (unsigned long)(uint32_t) cpu->lo,
	    (unsigned long)(uint32_t) cpu->hi,
	    (unsigned long)(uint32_t) cpu->pc,
	    (unsigned long)(uint32_t) cpu->nextpc);

	for (i=0; i<NTLB; i++) {
		tlbmsg("TLB", i, &cpu->tlb[i]);
	}
	tlbmsg("TLB", -1, &cpu->tlbentry);
	msg("tlb index: %d %s", cpu->tlbindex, 
	    cpu->tlbpf ? "[last probe failed]" : "");
	msg("tlb random: %d", (cpu->tlbrandom%RANDREG_MAX)+RANDREG_OFFSET);

	msgl("Status register: ");
	/* coprocessor enable bits */
	msgl("%s%s%s%s",
	     cpu->status_copenable & 0x80000000 ? "3" : "-",
	     cpu->status_copenable & 0x40000000 ? "2" : "-",
	     cpu->status_copenable & 0x20000000 ? "1" : "-",
	     cpu->status_copenable & 0x10000000 ? "0" : "-");
	/* misc control bits */
	msgl("%s%s%s%s%s%s%s%s%s%s%s%s",
	     0 ? "P" : "-",	/* reduced power mode */
	     0 ? "F" : "-",	/* 64-bit extended FPU mode */
	     0 ? "R" : "-",	/* reverse endianness */
	     0 ? "M" : "-",	/* 64-bit MDMX extensions */
	     0 ? "6" : "-",	/* 64-bit mode */
	     cpu->status_bootvectors ? "B" : "-",
	     0 ? "T" : "-",	/* duplicate TLB entries */
	     0 ? "S" : "-",	/* soft reset */
	     0 ? "N" : "-",	/* NMI reset */
	     0 ? "C" : "-",	/* cache parity */
	     0 ? "W" : "-",	/* r3k swap caches */
	     0 ? "I" : "-");	/* r3k isolate cache */
	/* interrupt mask */
	msgl("%s%s%s%s%s%s%s%s",
	     cpu->status_hardmask_timer ? "H" : "-",
	     cpu->status_hardmask_void & 0x00004000 ? "h" : "-",
	     cpu->status_hardmask_void & 0x00002000 ? "h" : "-",
	     cpu->status_hardmask_fpu ? "h" : "-",
	     cpu->status_hardmask_ipi ? "H" : "-",
	     cpu->status_hardmask_lb ? "H" : "-",
	     cpu->status_softmask & 0x0200 ? "S" : "-",
	     cpu->status_softmask & 0x0100 ? "S" : "-");
	/* mode control */
	msg("--%s%s%s%s%s%s",
	    cpu->old_usermode ? "U" : "-",
	    cpu->old_irqon ? "I" : "-",
	    cpu->prev_usermode ? "U" : "-",
	    cpu->prev_irqon ? "I" : "-",
	    cpu->current_usermode ? "U" : "-",
	    cpu->current_irqon ? "I" : "-");

	msg("Cause register: %s %d %s---%s%s%s%s %d [%s]",
	    cpu->cause_bd ? "B" : "-",
	    cpu->cause_ce >> 28,
	    cpu->irq_timer ? "H" : "-",
	    cpu->irq_ipi ? "H" : "-",
	    cpu->irq_lamebus ? "H" : "-",
	    (cpu->cause_softirq & 0x200) ? "S" : "-",
	    (cpu->cause_softirq & 0x100) ? "S" : "-",
	    cpu->cause_code >> 2,
	    exception_name(cpu->cause_code>>2));

	msg("VAddr register: 0x%08lx", (unsigned long)cpu->ex_vaddr);
	msg("Context register: 0x%08lx", (unsigned long)cpu->ex_context);
	msg("EPC register: 0x%08lx", (unsigned long)cpu->ex_epc);

	/* END INDENT HORROR */

	}
}

unsigned
cpu_numcpus(void)
{
	return ncpus;
}

void
cpu_enable(unsigned cpunum)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->state = CPU_RUNNING;
	RUNNING_MASK_ON(cpunum);
}

void
cpu_disable(unsigned cpunum)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->state = CPU_DISABLED;
	RUNNING_MASK_OFF(cpunum);
}

int
cpu_enabled(unsigned cpunum)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	return (cpu->state != CPU_DISABLED);
}

#define BETWEEN(addr, size, base, top) \
          ((addr) >= (base) && (size) <= (top)-(base) && (addr)+(size) < (top))

int
cpu_get_load_paddr(uint32_t vaddr, uint32_t size, uint32_t *paddr)
{
	if (!BETWEEN(vaddr, size, KSEG0, KSEG2)) {
		return -1;
	}

	if (vaddr >= KSEG1) {
		*paddr = vaddr - KSEG1;
	}
	else {
		*paddr = vaddr - KSEG0;
	}
	return 0;
}

int
cpu_get_load_vaddr(uint32_t paddr, uint32_t size, uint32_t *vaddr)
{
	uint32_t zero = 0;  /* suppresses silly gcc warning */
	if (!BETWEEN(paddr, size, zero, KSEG1-KSEG0)) {
		return -1;
	}
	*vaddr = paddr + KSEG0;
	return 0;
}

void
cpu_set_entrypoint(unsigned cpunum, uint32_t addr)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	if ((addr & 0x3) != 0) {
		hang("Kernel entry point is not properly aligned");
		addr &= 0xfffffffc;
	}
	cpu->expc = addr;
	cpu->pc = addr;
	cpu->nextpc = addr+4;
	if (precompute_pc(cpu)) {
		hang("Kernel entry point is an invalid address");
	}
	if (precompute_nextpc(cpu)) {
		hang("Kernel entry point is an invalid address");
	}
}

void
cpu_set_stack(unsigned cpunum, uint32_t stackaddr, uint32_t argument)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->r[29] = stackaddr;   /* register 29: stack pointer */
	cpu->r[4] = argument;     /* register 4: first argument */
	
	/* don't need to set $gp - in the ELF model it's start's problem */
}

uint32_t
cpu_get_secondary_start_stack(uint32_t lboffset)
{
	/* lboffset is the offset from the LAMEbus mapping base. */
	/* XXX why don't we have a constant for 1fe00000? */
	return KSEG0 + 0x1fe00000 + lboffset;
}

void
cpu_set_irqs(unsigned cpunum, int lamebus, int ipi)
{
	Assert(cpunum < ncpus);

	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];
	cpu->irq_lamebus = lamebus;
	cpu->irq_ipi = ipi;

	/* cpu->irq_timer is on-chip, and cannot get set when CPU_IDLE */

	CPUTRACE(DOTRACE_IRQ, cpunum,
		 "cpu_set_irqs: LB %s IPI %s",
		 lamebus ? "ON" : "off",
		 ipi ? "ON" : "off");
	if (cpu->state == CPU_IDLE && (lamebus || ipi)) {
		cpu->state = CPU_RUNNING;
		RUNNING_MASK_ON(cpunum);
	}
}

/*
 * Return which CPU hit a breakpoint. If more than one did, use the
 * first. If none did, use CPU 0.
 */
unsigned
cpudebug_get_break_cpu(void)
{
	unsigned i;

	for (i=0; i<ncpus; i++) {
		if (mycpus[i].hit_breakpoint) {
			return i;
		}
	}
	return 0;
}

void
cpudebug_get_bp_region(uint32_t *start, uint32_t *end)
{
	*start = KSEG0;
	*end = KSEG2;
}

int
cpudebug_fetch_byte(unsigned cpunum, uint32_t va, uint8_t *byte)
{
	uint32_t pa;
	uint32_t aligned_va;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	aligned_va = va & 0xfffffffc;

	/*
	 * For now, only allow KSEG0/1
	 */

	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, aligned_va, 0, &pa)) {
		return -1;
	}

	pa |= (va & 3);

	if (bus_mem_fetchbyte(pa, byte)) {
		return -1;
	}
	return 0;
}

int
cpudebug_fetch_word(unsigned cpunum, uint32_t va, uint32_t *word)
{
	uint32_t pa;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1
	 */
	
	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, va, 0, &pa)) {
		return -1;
	}

	if (bus_mem_fetch(pa, word)) {
		return -1;
	}
	return 0;
}

int
cpudebug_store_byte(unsigned cpunum, uint32_t va, uint8_t byte)
{
	uint32_t pa;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1
	 */

	cpu = &mycpus[cpunum];

	if (debug_translatemem(cpu, va, 1, &pa)) {
		return -1;
	}

	if (bus_mem_storebyte(pa, byte)) {
		return -1;
	}
	return 0;
}

int
cpudebug_store_word(unsigned cpunum, uint32_t va, uint32_t word)
{
	uint32_t pa;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1.
	 */
	
	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, va, 1, &pa)) {
		return -1;
	}

	if (bus_mem_store(pa, word)) {
		return -1;
	}
	return 0;
}

static
inline
void
addreg(uint32_t *regs, int maxregs, int pos, uint32_t val)
{
	if (pos < maxregs) {
		regs[pos] = val;
	}
}

#define GETREG(r) addreg(regs, maxregs, j++, r)

void
cpudebug_getregs(unsigned cpunum, uint32_t *regs, int maxregs, int *nregs)
{
	int i, j=0;
	struct mipscpu *cpu;

	/* choose a CPU */
	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	for (i=0; i<NREGS; i++) {
		GETREG(cpu->r[i]);
	}
	GETREG(getstatus(cpu));
	GETREG(cpu->lo);
	GETREG(cpu->hi);
	GETREG(cpu->ex_vaddr);
	GETREG(getcause(cpu));
	GETREG(cpu->pc);
	GETREG(0); /* fp status? */
	GETREG(0); /* fp something-else? */
	GETREG(0); /* fp ? */
	GETREG(getindex(cpu));
	GETREG(getrandom(cpu));
	GETREG(tlbgetlo(&cpu->tlbentry));
	GETREG(cpu->ex_context);
	GETREG(tlbgethi(&cpu->tlbentry));
	GETREG(cpu->ex_epc);
	GETREG(cpu->ex_prid);
	*nregs = j;
}

uint32_t
cpuprof_sample(void)
{
	/* for now always use CPU 0 (XXX) */
	return mycpus[0].pc;
}
