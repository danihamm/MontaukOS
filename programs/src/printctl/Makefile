# Makefile for printctl on MontaukOS

MAKEFLAGS += -rR
.SUFFIXES:

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CXX := $(TOOLCHAIN_PREFIX)g++
else
    CXX := g++
endif

PROG_INC := ../../include
LIBC_LIB := ../../lib/libc
BEARSSL := ../../lib/bearssl
TLS_LIB := ../../lib/tls
LINK_LD  := ../../link.ld
BINDIR   := ../../bin
OBJDIR   := obj

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
    -isystem $(PROG_INC)/libc \
    -I $(BEARSSL)/inc \
    -isystem ../../../kernel/freestnd-c-hdrs/x86_64/include \
    -isystem ../../../kernel/freestnd-cxx-hdrs/x86_64/include

LDFLAGS := \
    -nostdlib \
    -static \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    -Wl,-m,elf_x86_64 \
    -z max-page-size=0x1000 \
    -T $(LINK_LD)

TARGET := $(BINDIR)/os/printctl.elf
DEPS := $(OBJDIR)/main.d

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJDIR)/main.o $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/os
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJDIR)/main.o $(TLS_LIB)/libtls.a $(BEARSSL)/libbearssl.a $(LIBC_LIB)/liblibc.a -o $@

$(OBJDIR)/main.o: main.cpp Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(OBJDIR) $(TARGET)
