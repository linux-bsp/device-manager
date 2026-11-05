CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror
.DEFAULT_GOAL := all
include ../transfer/consumer.mk

TARGET = ../bin/fwctl
SOURCES = $(wildcard cli/*.c core/*.c storage/*.c recovery/*.c health/*.c)
OBJECTS = $(patsubst %.c,build/%.o,$(SOURCES)) build/identity/hwid.o
INCLUDES = -Iinclude -Icore -Istorage -Ihealth -I../identity/include $(DM_TRANSFER_INCLUDE)
LIBRARIES = -lcurl -lcrypto -lfdt -lubootenv -pthread

.PHONY: all test clean
all: $(TARGET)

$(TARGET): $(OBJECTS) $(DM_TRANSFER_CLIENT) Makefile
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OBJECTS) $(DM_TRANSFER_CLIENT) $(LDFLAGS) $(LIBRARIES) -o $@

build/%.o: %.c Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

build/identity/hwid.o: ../identity/hwid.c Makefile
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

test:
	python3 tests/test_firmware.py
	python3 tests/test_systemd_health.py

clean:
	rm -rf build $(TARGET)
