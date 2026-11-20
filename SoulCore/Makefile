# Builds the SoulCore library and the soulrun test runner for Linux/macOS hosts.
#   make            -> native build (x86_64 hosts use the interpreter only)
#   make arm64      -> aarch64 cross build (run with qemu-aarch64 to exercise the JIT)
CC ?= cc
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -Iinclude -D_GNU_SOURCE
SRC := $(wildcard src/*.c)
ASM := $(wildcard src/*.S)
BUILD ?= build/native

all: $(BUILD)/soulrun

$(BUILD)/soulrun: $(SRC) $(ASM) tools/soulrun.c src/*.h include/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ASM) tools/soulrun.c -lm -lpthread

arm64:
	$(MAKE) CC=aarch64-linux-gnu-gcc BUILD=build/arm64 CFLAGS="-O2 -g -static"

clean:
	rm -rf build

.PHONY: all arm64 clean
