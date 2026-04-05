# Makefile for libmath shared library
# Builds libmath.lib at fixed address 0x400000 (relocated at load time)

# Target architecture.
ARCH := x86_64

# Toolchain
TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
CXX := $(TOOLCHAIN_PREFIX)g++
NM := $(TOOLCHAIN_PREFIX)nm
STRIP := $(TOOLCHAIN_PREFIX)strip

# Compiler flags: freestanding, no stdlib
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
    -isystem ../../include/montauk

# Linker flags: freestanding static ELF at fixed address
# Note: NOT using --gc-sections so all functions are kept
override LDFLAGS := \
    -nostdlib \
    -static \
    -Wl,--build-id=none \
    -Wl,-m,elf_x86_64 \
    -T link.ld

# Output
OUTDIR := ../../bin/os
LIBNAME := libmath
LIBSRC := src/libmath.cpp

.PHONY: all clean

all: $(OUTDIR)/$(LIBNAME).lib $(OUTDIR)/$(LIBNAME).lib.sym

$(OUTDIR)/$(LIBNAME).lib: $(LIBSRC)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) -c $(LIBSRC) -o src/libmath.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) src/libmath.o -o $@

# Generate symbol file from nm output
# Format: "symbol_name,offset_in_hex"
$(OUTDIR)/$(LIBNAME).lib.sym: $(OUTDIR)/$(LIBNAME).lib
	@mkdir -p $(OUTDIR)
	@echo "# Symbol table for $(LIBNAME)" > $@
	@echo "# This file maps symbol names to offsets from library base" >> $@
	@$(NM) $(OUTDIR)/$(LIBNAME).lib | grep ' T ' | awk '{printf "%s,0x%x\n", $$3, strtonum("0x" $$1) - 0x400000}' >> $@
	@echo "Generated symbol file: $@"

clean:
	rm -rf ../../bin/os/$(LIBNAME).lib ../../bin/os/$(LIBNAME).lib.sym src/libmath.o
