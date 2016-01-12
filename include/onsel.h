/*
 * Event hooks for main loop.
 *
 * If func returns a nonzero value, the fd will be removed from the
 * select loop and removefunc will be called.
 *
 * removefunc may be null.
 */
void onselect(int fd, void *data, int (*func)(void *data),
	      void (*removefunc)(void *data));


/*
 * Cancel a prior onselect call. Crashes if the fd isn't there.
 * The removefunc previously installed (if any) will be called.
 */
void notonselect(int fd);

/*
 * Select on the things that have had onselect() called on them.
 *
 * If do_timeout is true, tryselect will return after NSECS, which can
 * be zero. Otherwise it will block indefinitely until something
 * happens.
 *
 * Returns the time spent sleeping in nanoseconds.
 */
uint64_t tryselect(int do_timeout, uint64_t nsecs);

/* Extra time from waiting in select (while dispatching select events) */
uint64_t extra_selecttime;
