# Makefile for test_dl

ARCH := x86_64

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
CXX := $(TOOLCHAIN_PREFIX)g++

override CXXFLAGS := \
    -std=gnu++20 \
    -g -O2 -pipe \
    -Wall \
    -Wextra \
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
    -I ../../include \
    -isystem ../../include/libc \
    -isystem ../../include/montauk \
    -isystem ../../../kernel/freestnd-c-hdrs/x86_64/include \
    -isystem ../../../kernel/freestnd-cxx-hdrs/x86_64/include

override LDFLAGS := \
    -nostdlib \
    -static \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    -Wl,-m,elf_x86_64 \
    -z max-page-size=0x1000 \
    -T ../../link.ld

BINDIR := ../../bin/os
LIBDIR := ../../bin

.PHONY: all clean

all: $(BINDIR)/test_dl.elf

$(BINDIR)/test_dl.elf: main.cpp
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -c main.cpp -o main.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) main.o $(LIBDIR)/liblibloader.a ../../lib/libc/liblibc.a -o $@
	rm -f main.o
	rm -f $(LIBDIR)/liblibloader.a

clean:
	rm -f $(BINDIR)/test_dl.elf
