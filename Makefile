CFLAGS ?= -Wall -Wextra -pedantic
CPPFLAGS ?=
LDFLAGS ?=

all: idigna

idigna: idigna.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $<

.PHONY: all clean distclean

clean:
	rm idigna

distclean: clean
