CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lcurl -ljson-c

.PHONY: all clean test

all: lmg

lmg: lmg.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f lmg

test: lmg
	LMG_BIN="$(CURDIR)/lmg" python3 -m unittest -v tests/test_lmg.py
