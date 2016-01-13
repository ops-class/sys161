/*
 * Tell mainloop to begin poweroff.
 * (poweroff takes a few ms; see speed.h)
 */
void main_poweroff(void);

/*
 * Tell mainloop that at least one explicit debugger request was
 * encountered. This typically means the OS panic'd.
 */
void main_note_debugrequest(void);

/*
 * Tell mainloop it should suspend execution.
 */
void main_enter_debugger(int lethal);

/*
 * Tell mainloop it should resume execution.
 */
void main_leave_debugger(void);

/*
 * Have the mainloop code run a single processor cycle.
 * Returns nonzero if we hit a breakpoint instruction in that cycle.
 */
void onecycle(void);

/*
 * Have the mainloop code do a complete dump of its state.
 * This ultimately dumps the entire state of sys161.
 */
void main_dumpstate(void);

/*
 * Hardware counters reported at simulator exit.
 */

struct stats_percpu {
	uint64_t sp_ucycles;  // user mode cycles
	uint64_t sp_kcycles;  // kernel mode cycles
	uint64_t sp_icycles;  // idle cycles
	uint64_t sp_uretired; // user mode instructions retired
	uint64_t sp_kretired; // kernel mode instructions retired
	uint64_t sp_lls;      // LL instructions
	uint64_t sp_okscs;    // successful SC instructions
	uint64_t sp_badscs;   // failed SC instructions
	uint64_t sp_syncs;    // SYNC instructions
};

struct stats {
	uint64_t s_tot_rcycles; // cycles with at least one cpu running
	uint64_t s_tot_icycles; // cycles when fully idle
	struct stats_percpu *s_percpu;
	unsigned s_numcpus;
	uint32_t s_irqs;     // total interrupts
	uint32_t s_exns;     // total exceptions
	uint32_t s_rsects;   // disk sectors read
	uint32_t s_wsects;   // disk sectors written
	uint32_t s_rchars;   // console chars read
	uint32_t s_wchars;   // console chars written
	uint32_t s_remu;     // emufs reads
	uint32_t s_wemu;     // emufs writes
	uint32_t s_memu;     // emufs other ops
	uint32_t s_rpkts;    // network packets read
	uint32_t s_wpkts;    // network packets written
	uint32_t s_dpkts;    // network packets dropped
	uint32_t s_epkts;    // network errors
};

extern struct stats g_stats;
