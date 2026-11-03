CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=gnu11 -Wall -Wextra -Werror
FW_SOURCES = $(wildcard firmware/src/*.c)
FWCTL_SOURCES = $(wildcard fwctl/src/*.c) fwctl/adapter/peripheral_ipc.c recovery/fw-init.c
FW_INCLUDES = -Ifirmware/include -Ifirmware/src -Ifwctl/include -Ifwctl/adapter
.PHONY: all host device device-server ota-cli fwctld fwctl logctl logctld test test-storage clean
all: host device
host: device-server ota-cli
device: fwctl fwctld logctl logctld
device-server ota-cli fwctld:
	$(MAKE) -C ota $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
fwctl:
	mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(FW_INCLUDES) $(FWCTL_SOURCES) $(FW_SOURCES) $(LDFLAGS) -lcurl -lcrypto -lfdt -pthread -o bin/fwctl
logctl:
	$(MAKE) -C logging $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
logctld: logctl
	cp logging/logctl logging/logctld
test:
	python3 tests/test_firmware.py
	$(MAKE) -C logging test
	$(MAKE) -C ota test
test-storage: logctld fwctld
	python3 tests/test_storage_sync.py
clean:
	$(MAKE) -C ota clean
	$(MAKE) -C logging clean
	rm -rf bin
