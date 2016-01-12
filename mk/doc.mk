#
# Makefile fragment for installing the docs
#

HTML=devices.html gdb.html index.html install.html lamebus.html \
     mips.html networking.html prof.html system.html


include defs.mk

all tidy distclean clean rules depend: ;

install:
	(umask 022; \
	  [ -d "$(DESTDIR)$(EXAMPLEDIR)" ] || mkdir -p $(DESTDIR)$(EXAMPLEDIR))
	(umask 022; \
	  [ -d "$(DESTDIR)$(DOCDIR)" ] || mkdir -p $(DESTDIR)$(DOCDIR))
	@for h in $(HTML); do \
		echo cp $S/doc/$$h $(DESTDIR)$(DOCDIR)/$$h; \
		cp $S/doc/$$h $(DESTDIR)$(DOCDIR)/$$h; \
		chmod 644 $(DESTDIR)$(DOCDIR)/$$h; \
	  done
	cp $S/COPYING $(DESTDIR)$(DOCDIR)/copying.txt
	chmod 644 $(DESTDIR)$(DOCDIR)/copying.txt
	cp $S/sys161.conf.sample $(DESTDIR)$(EXAMPLEDIR)/sys161.conf.sample
	chmod 644 $(DESTDIR)$(EXAMPLEDIR)/sys161.conf.sample
