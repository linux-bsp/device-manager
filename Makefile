CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=gnu11 -Wall -Wextra -Werror
.DEFAULT_GOAL := all
.PHONY: all host device device-server ota-cli fwctld fwctl logctl logctld timectl timectld test test-storage clean
all: host device
host: device-server ota-cli
device: fwctl fwctld logctl logctld timectl timectld
device-server ota-cli fwctld:
	$(MAKE) -C ota $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
fwctl:
	$(MAKE) -C fwctl CC="$(CC)" AR="$(AR)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
logctl:
	$(MAKE) -C logging $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
logctld: logctl
	cp logging/logctl logging/logctld
timectl:
	$(MAKE) -C time $@ CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS) $(WARN)" LDFLAGS="$(LDFLAGS)"
timectld: timectl
	ln -sf timectl time/timectld
test:
	$(MAKE) -C transfer test
	python3 tests/test_hwid.py
	$(MAKE) -C fwctl test
	$(MAKE) -C logging test
	$(MAKE) -C time test
	$(MAKE) -C ota test
test-storage: logctld fwctld
	python3 tests/test_storage_sync.py
clean:
	$(MAKE) -C fwctl clean
	$(MAKE) -C transfer clean
	$(MAKE) -C ota clean
	$(MAKE) -C logging clean
	$(MAKE) -C time clean
	rm -rf bin
