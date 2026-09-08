.POSIX:

VERSION = 0.1.0
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

CC = cc
CPPFLAGS = -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O2
LDFLAGS =
LDLIBS =

all: build

config.h:
	cp config.def.h config.h

build: mwin.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mwin.c $(LDLIBS)

debug: CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O0 -g3 -fsanitize=address,undefined
debug: LDFLAGS = -fsanitize=address,undefined
debug: clean build

test: build
	./mwin --self-test
	sh tests/integration.sh

install: build
	mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(MANPREFIX)/man1"
	cp mwin "$(DESTDIR)$(PREFIX)/bin/mwin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/mwin"
	cp mwin.1 "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/mwin" "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"

clean:
	rm -f mwin

.PHONY: all debug test install uninstall clean
