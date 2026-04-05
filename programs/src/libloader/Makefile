# Makefile for libloader

ARCH := x86_64

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
CXX := $(TOOLCHAIN_PREFIX)g++
AR := $(TOOLCHAIN_PREFIX)ar

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

BINDIR := ../../bin

.PHONY: all clean

all: $(BINDIR)/liblibloader.a

$(BINDIR)/liblibloader.a: libloader.cpp
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -c libloader.cpp -o libloader.o
	$(AR) rcs $(BINDIR)/liblibloader.a libloader.o
	rm -f libloader.o

clean:
	rm -f $(BINDIR)/liblibloader.a
