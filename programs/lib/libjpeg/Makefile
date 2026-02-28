# Makefile for libjpeg (JPEG decoding via stb_image) on MontaukOS
# Copyright (c) 2026 Daniel Hammer

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
PROG_INC := ../../include
OBJDIR   := obj

# ---- Compiler flags ----

CFLAGS := \
    -std=gnu11 \
    -g -O2 -pipe \
    -Wall \
    -Wno-unused-parameter \
    -Wno-unused-function \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -msse \
    -msse2 \
    -mno-red-zone \
    -mcmodel=small \
    -I $(PROG_INC) \
    -isystem $(LIBC_INC) \
    -isystem $(shell $(CC) -print-file-name=include)

# ---- Target ----

TARGET := libjpeg.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJDIR)/stb_image_impl.o
	$(AR) rcs $@ $^

$(OBJDIR)/stb_image_impl.o: stb_image_impl.c Makefile
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
