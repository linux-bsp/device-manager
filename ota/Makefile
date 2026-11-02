CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror

.PHONY: all test clean
all: device-server fwctld ota-cli

ota-page.h: admin.html embed-page.awk
	od -An -v -t u1 admin.html | awk -f embed-page.awk > $@

device-server: ota-catalog.c device-server.c ota-quarantine.c ota-identity.c ota-admin.c ota-admin.h ota-publish.c ota-common.c ota-common.h ota-device.c ota-history.c ota-report.c ota-device.h ota-page.h
	$(CC) $(CPPFLAGS) $(CFLAGS) ota-catalog.c device-server.c ota-quarantine.c ota-identity.c ota-admin.c ota-publish.c ota-common.c ota-device.c ota-history.c ota-report.c $(LDFLAGS) -lz -lsqlite3 -o $@

fwctld: fwctld-network.c fwctld-network.h fwctld-log.c fwctld-log.h fwctld.c fwctld-auto.c ota-common.c ota-common.h ota-report.c ota-device.h fwctld-auto.h
	$(CC) $(CPPFLAGS) $(CFLAGS) fwctld.c fwctld-auto.c fwctld-log.c fwctld-network.c ota-common.c ota-report.c $(LDFLAGS) -o $@

ota-cli: ota-cli.c
	$(CC) $(CPPFLAGS) $(CFLAGS) ota-cli.c $(LDFLAGS) -o $@

test: all
	sh ./tests/test-ota.sh
	python3 ./tests/test-admin.py
	python3 ./tests/test-catalog.py
	python3 ./tests/test-auto.py
	python3 ./tests/test-init.py
	python3 ./tests/test-network.py
	python3 ./tests/test-device-context.py
	python3 ./tests/test-history.py
	python3 ./tests/test-quarantine.py
	python3 ./tests/test-shutdown.py
	python3 ./tests/test-cli.py

clean:
	rm -f device-server fwctld ota-cli ota-page.h
