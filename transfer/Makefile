CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wextra -Werror
INCLUDES = -Iinclude -Isrc
CLIENT_SOURCES = src/http-client.c src/file-transfer.c src/checkpoint.c src/io.c src/hash.c
SERVER_SOURCES = src/http-server.c src/io.c src/hash.c
HEADERS = $(wildcard include/dm/*.h src/*.h)
.PHONY: all client server clean test
all: client server
client: build/libdm-transfer.a
server: build/libdm-transfer-server.a

# Each recursive consumer may request the same archive concurrently. Compile in
# a private directory and atomically publish the complete archive.
build/libdm-transfer.a: $(CLIENT_SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	@set -eu; transfer_tmp=$$(mktemp -d build/.client-XXXXXX); trap 'rm -rf "$$transfer_tmp"' EXIT HUP INT TERM; \
	for source in $(CLIENT_SOURCES); do \
	  $(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -c "$$source" -o "$$transfer_tmp/$$(basename "$$source" .c).o"; \
	done; $(AR) rcs "$$transfer_tmp/library.a" "$$transfer_tmp/"*.o; mv "$$transfer_tmp/library.a" $@
build/libdm-transfer-server.a: $(SERVER_SOURCES) $(HEADERS) Makefile
	@mkdir -p build
	@set -eu; transfer_tmp=$$(mktemp -d build/.server-XXXXXX); trap 'rm -rf "$$transfer_tmp"' EXIT HUP INT TERM; \
	for source in $(SERVER_SOURCES); do \
	  $(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -c "$$source" -o "$$transfer_tmp/$$(basename "$$source" .c).o"; \
	done; $(AR) rcs "$$transfer_tmp/library.a" "$$transfer_tmp/"*.o; mv "$$transfer_tmp/library.a" $@
test: all
	python3 tests/test_transfer.py
clean:
	rm -rf build
