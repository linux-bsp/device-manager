CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
.DEFAULT_GOAL := all
include ../transfer/consumer.mk
INCLUDES = -I. -Iserver -Iagent -Ishared -I../identity/include
SERVER_SOURCES = $(wildcard server/*.c) shared/ota-common.c shared/ota-report.c
SERVER_HEADERS = $(wildcard server/*.h shared/*.h) ../identity/include/dm/hwid-value.h
AGENT_SOURCES = $(wildcard agent/*.c) shared/ota-common.c shared/ota-report.c
AGENT_HEADERS = $(wildcard agent/*.h shared/*.h)

.PHONY: all test clean
all: device-server fwctld ota-cli

ota-page.h: web/time-format.js web/index.html web/styles.css web/app.js web/hwid.js web/fleet.js web/sol-console.js web/sol-names.js web/sol-bundles.js web/clock.js web/sol-records.js web/incident-logs.js web/embed.py ../config/hwid-registry.json ../identity/registry.py
	python3 web/embed.py $@

device-server: $(SERVER_SOURCES) $(SERVER_HEADERS) ota-page.h $(DM_TRANSFER_SERVER)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(DM_TRANSFER_INCLUDE) $(SERVER_SOURCES) $(DM_TRANSFER_SERVER) $(LDFLAGS) -lcrypto -lz -lsqlite3 -o $@

fwctld: $(AGENT_SOURCES) $(AGENT_HEADERS) $(DM_TRANSFER_CLIENT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(DM_TRANSFER_INCLUDE) $(AGENT_SOURCES) $(DM_TRANSFER_CLIENT) $(LDFLAGS) -lcurl -lcrypto -pthread -o $@

ota-cli: cli/ota-cli.c $(DM_TRANSFER_CLIENT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(DM_TRANSFER_INCLUDE) cli/ota-cli.c $(DM_TRANSFER_CLIENT) $(LDFLAGS) -lcurl -pthread -o $@

test: all
	node --test ./tests/test-time-format.cjs ./tests/test-sol-console.cjs ./tests/test-sol-names.cjs ./tests/test-hwid.cjs
	python3 ./tests/test-hwid-registry.py
	sh ./tests/test-ota.sh
	python3 ./tests/test-admin.py
	python3 ./tests/test-admission.py
	python3 ./tests/test-catalog.py
	python3 ./tests/test-hwid-catalog.py
	python3 ./tests/test-hwid-channels.py
	python3 ./tests/test-auto.py
	python3 ./tests/test-agent-state.py
	python3 ./tests/test-agent-cache.py
	python3 ./tests/test-init.py
	python3 ./tests/test-network.py
	python3 ./tests/test-device-context.py
	python3 ./tests/test-history.py
	python3 ./tests/test-quarantine.py
	python3 ./tests/test-shutdown.py
	python3 ./tests/test-log-stop.py
	python3 ./tests/test-cli.py
	python3 ./tests/test-sol.py
	python3 ./tests/test-sol-bundle.py
	python3 ./tests/test-sol-delete.py
	python3 ./tests/test-startup.py
	python3 ./tests/test-manager-time.py
	python3 ./tests/test-clock-control.py
	python3 ./tests/test-boot-health.py
	python3 ./tests/test-boot-criticality.py
	python3 ./tests/test-incident-evidence.py
	python3 ./tests/test-startup-incident.py
	python3 ./tests/test-diagnostics.py

clean:
	rm -f device-server fwctld ota-cli ota-page.h
