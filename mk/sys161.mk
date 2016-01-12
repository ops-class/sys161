#
# Makefile fragment for compiling sys161.
#

all: $(PROG)

include rules.mk
include depend.mk

CFLAGS+=-I$S/include -I.
SRCFILES+=bus     lamebus.c boot.c \
                  dev_disk.c dev_emufs.c dev_net.c dev_random.c \
                  dev_screen.c dev_serial.c dev_timer.c dev_trace.c \
          gdb     gdb_fe.c gdb_be.c \
          main    main.c onsel.c clock.c console.c \
                  prof.c meter.c trace.c util.c

tidy:
	(find $S -name '*~' -print | xargs rm -f)

distclean clean: #tidy
	rm -f *.o $(PROG)

rules:
	@echo Making rules...
	@echo $(SRCFILES) | $S/makerules.sh > rules.mk

depend:
	$(MAKE) rules
	$(MAKE) realdepend

realdepend:
	$(CC) $(CFLAGS) $(DEPINCLUDES) -MM $(SRCS) > depend.mk

install:
	(umask 022; \
		[ -d "$(DESTDIR)$(BINDIR)" ] || mkdir -p "$(DESTDIR)$(BINDIR)")
	$S/installit.sh "$(DESTDIR)$(BINDIR)" "$(PROG)" "$(VERSION)"

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -lm -o $(PROG)
