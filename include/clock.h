extern unsigned progress;

void clock_init(void);
void clock_cleanup(void);
void clock_setprogresstimeout(uint32_t secs);

uint32_t clock_getrunticks(void);
void clock_ticks(uint64_t ticks);
void schedule_event(uint64_t nsecs, void *data, uint32_t code,
		    void (*func)(void *, uint32_t),
		    const char *desc);
void clock_time(uint32_t *secs, uint32_t *nsecs);
uint64_t clock_monotime(void);

void clock_setsecs(uint32_t secs);
void clock_setnsecs(uint32_t nsecs);


void clock_waitirq(void);

void clock_dumpstate(void);
