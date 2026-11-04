.POSIX:

VERSION = 0.3.14
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

CC = cc
PYTHON = python3
STATIC ?= 1
CPPFLAGS = -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O2
LDFLAGS =
STATIC_LDFLAGS = $(STATIC_LDFLAGS_$(STATIC))
STATIC_LDFLAGS_0 =
STATIC_LDFLAGS_1 = -static
LDLIBS =

ifeq ($(filter $(STATIC),0 1),)
$(error STATIC must be 0 or 1)
endif

all: mwin

config.h:
	cp config.def.h config.h

mwin: mwin.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(STATIC_LDFLAGS) $(LDFLAGS) -o $@ mwin.c $(LDLIBS)

tests/model-test: tests/model.c mwin.c config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(STATIC_LDFLAGS) $(LDFLAGS) -o $@ tests/model.c $(LDLIBS)

debug: CFLAGS = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wconversion -O0 -g3 -fsanitize=address,undefined
debug: LDFLAGS = -fsanitize=address,undefined
debug: STATIC = 0
debug: clean mwin tests/model-test

test: mwin tests/model-test
	./tests/model-test --model-test
	$(PYTHON) tests/integration.py

coverage: clean
	$(CC) $(CPPFLAGS) $(CFLAGS) -O0 --coverage $(STATIC_LDFLAGS) $(LDFLAGS) --coverage -o mwin tests/model.c $(LDLIBS)
	./mwin --model-test
	$(PYTHON) tests/integration.py
	gcov -b -c tests/model.c -o mwin-model.gcno

install: mwin
	mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(MANPREFIX)/man1"
	cp mwin "$(DESTDIR)$(PREFIX)/bin/mwin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/mwin"
	cp mwin.1 "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/mwin" "$(DESTDIR)$(MANPREFIX)/man1/mwin.1"

clean:
	rm -f mwin tests/model-test *.gcda *.gcno *.gcov tests/*.gcda tests/*.gcno

.PHONY: all debug test coverage install uninstall clean
