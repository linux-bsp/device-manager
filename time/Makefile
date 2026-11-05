CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
LDLIBS ?= -lcurl -lcrypto
.DEFAULT_GOAL := all
include ../transfer/consumer.mk
SOURCES = main.c config.c time-control.c time-sync.c time-ntp.c
.PHONY: all test clean
all: timectl timectld
timectl: $(SOURCES) config.h time-control.h time-sync.h ../ota/shared/time-source.h $(DM_TRANSFER_CLIENT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DM_TRANSFER_INCLUDE) $(SOURCES) $(DM_TRANSFER_CLIENT) $(LDFLAGS) $(LDLIBS) -pthread -o $@
timectld: timectl
	ln -sf timectl $@
test: all
	python3 tests/test_time_sync.py
	python3 tests/test_time_sources.py
	python3 tests/test_service.py
clean:
	rm -f timectl timectld
