# Makefile for shared TLS helper library on ZenithOS
# Copyright (c) 2026 Daniel Hammer

MAKEFLAGS += -rR
.SUFFIXES:

# ---- Toolchain ----

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CXX := $(TOOLCHAIN_PREFIX)g++
    AR  := $(TOOLCHAIN_PREFIX)ar
else
    CXX := g++
    AR  := ar
endif

# ---- Paths ----

BEARSSL  := ../bearssl
LIBC_INC := ../../include/libc
PROG_INC := ../../include
OBJDIR   := obj

# ---- Compiler flags ----

CXXFLAGS := \
    -std=gnu++20 \
    -g -O2 -pipe \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-PIC \
    -fno-rtti \
    -fno-exceptions \
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
    -I $(PROG_INC) \
    -isystem $(LIBC_INC) \
    -I $(BEARSSL)/inc \
    -isystem ../../../kernel/freestnd-c-hdrs/x86_64/include \
    -isystem ../../../kernel/freestnd-cxx-hdrs/x86_64/include

# ---- Target ----

TARGET := libtls.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJDIR)/tls.o
	$(AR) rcs $@ $^

$(OBJDIR)/tls.o: tls.cpp Makefile
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
