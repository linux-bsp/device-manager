CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=gnu11 -Wall -Wextra -Werror
.DEFAULT_GOAL := all
include transfer/consumer.mk
FW_SOURCES = $(wildcard firmware/src/*.c)
FWCTL_SOURCES = $(wildcard fwctl/src/*.c) fwctl/adapter/peripheral_ipc.c recovery/fw-init.c identity/hwid.c
FW_INCLUDES = -Ifirmware/include -Ifirmware/src -Ifwctl/include -Ifwctl/adapter -Iidentity/include
.PHONY: all host device device-server ota-cli fwctld fwctl logctl logctld test test-storage clean
all: host device
host: device-server ota-cli
device: fwctl fwctld logctl logctld
device-server ota-cli fwctld:
	$(MAKE) -C ota $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
fwctl: $(DM_TRANSFER_CLIENT)
	mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(FW_INCLUDES) $(DM_TRANSFER_INCLUDE) $(FWCTL_SOURCES) $(FW_SOURCES) $(DM_TRANSFER_CLIENT) $(LDFLAGS) -lcurl -lcrypto -lfdt -lubootenv -pthread -o bin/fwctl
logctl:
	$(MAKE) -C logging $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
logctld: logctl
	cp logging/logctl logging/logctld
test:
	$(MAKE) -C transfer test
	python3 tests/test_hwid.py
	python3 tests/test_firmware.py
	python3 tests/test_systemd_health.py
	$(MAKE) -C logging test
	$(MAKE) -C ota test
test-storage: logctld fwctld
	python3 tests/test_storage_sync.py
clean:
	$(MAKE) -C transfer clean
	$(MAKE) -C ota clean
	$(MAKE) -C logging clean
	rm -rf bin
