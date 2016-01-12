#
# Makefile fragment for installing the man pages
#

MAN=disk161.1 hub161.1 stat161.1 sys161.1


include defs.mk

all tidy distclean clean rules depend: ;

install:
	(umask 022; \
		[ -d "$(DESTDIR)$(MANDIR)/man1" ] || \
		mkdir -p $(DESTDIR)$(MANDIR)/man1)
	@for m in $(MAN); do \
		echo cp $S/man/$$m $(DESTDIR)$(MANDIR)/man1/$$m; \
		cp $S/man/$$m $(DESTDIR)$(MANDIR)/man1/$$m; \
		chmod 644 $(DESTDIR)$(MANDIR)/man1/$$m; \
	  done
