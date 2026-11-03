CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
INCLUDES = -I. -Iserver -Iagent -Ishared
SERVER_SOURCES = $(wildcard server/*.c) shared/ota-common.c shared/ota-report.c
SERVER_HEADERS = $(wildcard server/*.h shared/*.h)
AGENT_SOURCES = $(wildcard agent/*.c) shared/ota-common.c shared/ota-report.c
AGENT_HEADERS = $(wildcard agent/*.h shared/*.h)

.PHONY: all test clean
all: device-server fwctld ota-cli

ota-page.h: web/index.html web/styles.css web/app.js web/fleet.js web/sol-console.js web/embed.py
	python3 web/embed.py $@

device-server: $(SERVER_SOURCES) $(SERVER_HEADERS) ota-page.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(SERVER_SOURCES) $(LDFLAGS) -lz -lsqlite3 -o $@

fwctld: $(AGENT_SOURCES) $(AGENT_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(AGENT_SOURCES) $(LDFLAGS) -o $@

ota-cli: cli/ota-cli.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) cli/ota-cli.c $(LDFLAGS) -o $@

test: all
	node --test ./tests/test-sol-console.cjs
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
	python3 ./tests/test-log-stop.py
	python3 ./tests/test-cli.py
	python3 ./tests/test-sol.py

clean:
	rm -f device-server fwctld ota-cli ota-page.h
