all depend install tidy clean:
	(cd build-sys161 && $(MAKE) $@)
	(cd build-trace161 && $(MAKE) $@)
	(cd build-stat161 && $(MAKE) $@)
	(cd build-hub161 && $(MAKE) $@)
	(cd build-disk161 && $(MAKE) $@)
	(cd build-doc && $(MAKE) $@)
	(cd build-man && $(MAKE) $@)

distclean:
	rm -rf build-sys161 build-trace161
	rm -rf build-stat161 build-hub161 build-disk161
	rm -rf build-doc build-man
	rm -rf test-cpu
	rm -f Makefile defs.mk

test:
	(cd test-cpu && $(MAKE))
