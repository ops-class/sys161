#
# Makefile fragment for building hub161
#

all: $(PROG)

include rules.mk
include depend.mk

CFLAGS+=-I.
SRCFILES+=nethub  array.c nethub.c

distclean clean:
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
		[ -d "$(DESTDIR)$(BINDIR)" ] || mkdir -p $(DESTDIR)$(BINDIR))
	$S/installit.sh "$(DESTDIR)$(BINDIR)" "$(PROG)" "$(VERSION)"

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -lm -o $(PROG)
