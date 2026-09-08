.POSIX:

VERSION = 0.1.0
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

CC = cc
CPPFLAGS = -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O2
LDFLAGS =
LDLIBS =

all: minwin

config.h:
	cp config.def.h config.h

minwin: minwin.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ minwin.c $(LDLIBS)

debug: CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O0 -g3 -fsanitize=address,undefined
debug: LDFLAGS = -fsanitize=address,undefined
debug: clean minwin

test: minwin
	./minwin --self-test
	sh tests/integration.sh

install: minwin
	mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(MANPREFIX)/man1"
	cp minwin "$(DESTDIR)$(PREFIX)/bin/minwin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/minwin"
	cp minwin.1 "$(DESTDIR)$(MANPREFIX)/man1/minwin.1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/minwin.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/minwin" "$(DESTDIR)$(MANPREFIX)/man1/minwin.1"

clean:
	rm -f minwin

.PHONY: all debug test install uninstall clean
