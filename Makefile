.POSIX:

VERSION = 0.3.3
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

CC = cc
PYTHON = python3
CPPFLAGS = -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O2
LDFLAGS =
LDLIBS =

all: mwin

config.h:
	cp config.def.h config.h

mwin: mwin.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mwin.c $(LDLIBS)

debug: CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O0 -g3 -fsanitize=address,undefined
debug: LDFLAGS = -fsanitize=address,undefined
debug: clean mwin

test: mwin
	./mwin --self-test
	$(PYTHON) tests/integration.py

coverage: clean
	$(MAKE) CFLAGS='$(CFLAGS) -O0 --coverage' LDFLAGS='$(LDFLAGS) --coverage' mwin
	./mwin --self-test
	$(PYTHON) tests/integration.py
	gcov -b -c mwin.c

install: mwin
	mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(MANPREFIX)/man1"
	cp mwin "$(DESTDIR)$(PREFIX)/bin/mwin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/mwin"
	cp mwin.1 "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/mwin" "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"

clean:
	rm -f mwin *.gcda *.gcno *.gcov

.PHONY: all debug test coverage install uninstall clean
