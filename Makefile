PREFIX?=/usr
PKG_CONFIG?=pkg-config

CFLAGS?=-Wall -O3

CFLAGS+=`$(PKG_CONFIG) --cflags alsa` -pthread
LOADLIBES=`$(PKG_CONFIG) --libs alsa` -lm

all: mod-alsa-test

mod-alsa-test: mod-alsa-test.c

clean:
	rm -f mod-alsa-test

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 mod-alsa-test $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mod-alsa-test
	-rmdir $(DESTDIR)$(PREFIX)/bin

.PHONY: clean all install uninstall
