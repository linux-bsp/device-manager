CC ?= cc
LDLIBS ?= -lcurl -lcrypto -lz
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
.DEFAULT_GOAL := all
include ../transfer/consumer.mk
.PHONY: all test clean
all: logctl logctld
logctl: boot-worker.c boot-worker.h boot-record.c boot-record.h bundle-forward.c bundle-forward.h logging.c archive.c archive.h retention.c retention.h sol-forward.c sol-forward.h startup-forward.c startup-forward.h $(DM_TRANSFER_CLIENT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DM_TRANSFER_INCLUDE) logging.c archive.c boot-record.c boot-worker.c bundle-forward.c retention.c sol-forward.c startup-forward.c $(DM_TRANSFER_CLIENT) $(LDFLAGS) $(LDLIBS) -pthread -o $@
logctld: logctl
	cp logctl logctld
test: all
	python3 tests/test_logging.py
	python3 tests/test_boot_record.py
clean:
	rm -f logctl logctld
