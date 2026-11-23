# Builds the SoulCore library and the soulrun test runner for Linux/macOS hosts.
#   make            -> native build (Linux x86_64 hosts use the interpreter only; macOS builds
#                      build/macos/soulrun with the Metal backend sources when they exist)
#   make macos      -> the macOS build explicitly (make on Darwin does the same)
#   make arm64      -> aarch64 cross build (run with qemu-aarch64 to exercise the JIT)
UNAME_S := $(shell uname -s)
CC ?= cc
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -Iinclude -D_GNU_SOURCE
SRC := $(wildcard src/*.c)
ASM := $(wildcard src/*.S)
OBJC := $(wildcard src/*.m)
BUILD ?= build/native

ifeq ($(UNAME_S),Darwin)
all: macos
else
all: $(BUILD)/soulrun
endif

$(BUILD)/soulrun: $(SRC) $(ASM) tools/soulrun.c src/*.h include/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ASM) tools/soulrun.c -lm -lpthread

# ---- macOS (Apple Silicon host): clang, one object per file, Objective-C (ARC) for src/*.m ------------------
# -Wno-format-truncation etc. are gcc warnings; clang ignores unknown -Wno-* options silently.
MAC_BUILD := build/macos
MAC_OBJ := $(patsubst src/%.c,$(MAC_BUILD)/obj/%.o,$(SRC)) $(patsubst src/%.S,$(MAC_BUILD)/obj/%.o,$(ASM)) \
           $(patsubst src/%.m,$(MAC_BUILD)/obj/%.om,$(OBJC)) $(MAC_BUILD)/obj/soulrun.o
MAC_HDR := $(wildcard src/*.h include/*.h)
MAC_FRAMEWORKS := -framework Foundation -framework Metal -framework QuartzCore
# only link the frameworks when there is Objective-C code that needs them
MAC_LDFLAGS := $(if $(OBJC),$(MAC_FRAMEWORKS),)

macos: $(MAC_BUILD)/soulrun

$(MAC_BUILD)/soulrun: $(MAC_OBJ)
	$(CC) $(CFLAGS) -o $@ $(MAC_OBJ) $(MAC_LDFLAGS) -lm -lpthread

$(MAC_BUILD)/obj/%.o: src/%.c $(MAC_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(MAC_BUILD)/obj/%.o: src/%.S $(MAC_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(MAC_BUILD)/obj/%.om: src/%.m $(MAC_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -fmodules -c -x objective-c -o $@ $<
$(MAC_BUILD)/obj/soulrun.o: tools/soulrun.c $(MAC_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

arm64:
	$(MAKE) CC=aarch64-linux-gnu-gcc BUILD=build/arm64 CFLAGS="-O2 -g -static"

clean:
	rm -rf build

.PHONY: all macos arm64 clean
