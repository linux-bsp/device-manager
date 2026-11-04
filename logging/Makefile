CC ?= cc
LDLIBS ?= -lcurl
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
.PHONY: all test clean
all: logctl logctld
logctl: logging.c sol-forward.c sol-forward.h startup-forward.c startup-forward.h time-sync.c time-sync.h time-ntp.c time-control.c time-control.h ../ota/shared/time-source.h
	$(CC) $(CPPFLAGS) $(CFLAGS) logging.c sol-forward.c startup-forward.c time-sync.c time-ntp.c time-control.c $(LDFLAGS) $(LDLIBS) -o $@
logctld: logctl
	cp logctl logctld
test: all
	python3 tests/test_logging.py
	python3 tests/test_time_sync.py
	python3 tests/test_time_sources.py
clean:
	rm -f logctl logctld
