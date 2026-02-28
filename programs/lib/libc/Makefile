# Makefile for shared libc static library on MontaukOS
# Copyright (c) 2025-2026 Daniel Hammer

MAKEFLAGS += -rR
.SUFFIXES:

# ---- Toolchain ----

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CC := $(TOOLCHAIN_PREFIX)gcc
    AR := $(TOOLCHAIN_PREFIX)ar
else
    CC := gcc
    AR := ar
endif

# ---- Paths ----

LIBC_INC := ../../include/libc
OBJDIR   := obj

# ---- Compiler flags ----

CFLAGS := \
    -std=gnu11 \
    -g -O2 -pipe \
    -Wall \
    -Wno-unused-parameter \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -mcmodel=small \
    -isystem $(LIBC_INC) \
    -isystem $(shell $(CC) -print-file-name=include)

# ---- Target ----

TARGET := liblibc.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJDIR)/libc.o
	$(AR) rcs $@ $^

$(OBJDIR)/libc.o: libc.c Makefile
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
