CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
.PHONY: all test clean
all: logctl logctld
logctl: logging.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
logctld: logctl
	cp logctl logctld
test: all
	python3 tests/test_logging.py
clean:
	rm -f logctl logctld
