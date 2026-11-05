CC ?= cc
LDLIBS ?= -lcurl -lcrypto
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
.DEFAULT_GOAL := all
include ../transfer/consumer.mk
.PHONY: all test clean
all: logctl logctld
logctl: bundle-forward.c bundle-forward.h logging.c archive.c archive.h retention.c retention.h sol-forward.c sol-forward.h startup-forward.c startup-forward.h time-sync.c time-sync.h time-ntp.c time-control.c time-control.h ../ota/shared/time-source.h $(DM_TRANSFER_CLIENT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DM_TRANSFER_INCLUDE) logging.c archive.c bundle-forward.c retention.c sol-forward.c startup-forward.c time-sync.c time-ntp.c time-control.c $(DM_TRANSFER_CLIENT) $(LDFLAGS) $(LDLIBS) -pthread -o $@
logctld: logctl
	cp logctl logctld
test: all
	python3 tests/test_logging.py
	python3 tests/test_time_sync.py
	python3 tests/test_time_sources.py
clean:
	rm -f logctl logctld
