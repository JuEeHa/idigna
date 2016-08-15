DESTDIR ?=
PREFIX ?= /usr/local
EXEC_PREFIX ?= $(PREFIX)
BINDIR ?= $(DESTDIR)$(EXEC_PREFIX)/bin

CFLAGS += -Os -g -Wall -Wextra -pedantic
CPPFLAGS +=
LDFLAGS +=

all: idigna

install: all
	install idigna $(BINDIR)

idigna: idigna.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $<

.PHONY: all install clean distclean

clean:
	rm idigna

distclean: clean
