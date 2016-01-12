#ifndef GDB_H
#define GDB_H

/*
 * Functions callable in remote gdb support subsystem.
 */

/* Call at bootup to listen on AF_INET or AF_UNIX respectively */
void gdb_inet_init(int listenport);
void gdb_unix_init(const char *socketpath);

/* Call to disable waiting for connections */
void gdb_dontwait(void);

/* Call when stopping for a breakpoint; die if DONTWAIT and LETHAL */
void gdb_startbreak(int dontwait, int lethal);

/* Call to find out if debugging on this address is available */
int gdb_canhandle(uint32_t pcaddr);

/* Call for diagnostic purposes. */
void gdb_dumpstate(void);

#endif /* GDB_H */
