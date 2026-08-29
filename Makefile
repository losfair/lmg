CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lcurl -ljson-c

.PHONY: all clean test

all: lmg

lmg: lmg.c skill.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

skill.inc: SKILL.md
	LC_ALL=C od -An -v -tu1 $< | awk '{ for (i = 1; i <= NF; i++) printf "%s,", $$i; print "" } END { print "0" }' > $@

clean:
	rm -f lmg skill.inc

test: lmg
	LMG_BIN="$(CURDIR)/lmg" python3 -m unittest -v tests/test_lmg.py
