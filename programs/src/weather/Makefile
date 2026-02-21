# Makefile for weather (standalone Weather GUI client) on ZenithOS
# Copyright (c) 2026 Daniel Hammer

MAKEFLAGS += -rR
.SUFFIXES:

# ---- Toolchain ----

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CXX := $(TOOLCHAIN_PREFIX)g++
else
    CXX := g++
endif

# ---- Paths ----

BEARSSL  := ../../lib/bearssl
TLS_LIB  := ../../lib/tls
LIBC_LIB := ../../lib/libc
LIBC_INC := ../../include/libc
PROG_INC := ../../include
LINK_LD  := ../../link.ld
BINDIR   := ../../bin
OBJDIR   := obj

# ---- Compiler flags ----

CXXFLAGS := \
    -std=gnu++20 \
    -g -O2 -pipe \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -Wno-unused-function \
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
    -msse \
    -msse2 \
    -mno-red-zone \
    -mcmodel=small \
    -I $(PROG_INC) \
    -isystem $(LIBC_INC) \
    -I $(BEARSSL)/inc \
    -isystem ../../../kernel/freestnd-c-hdrs/x86_64/include \
    -isystem ../../../kernel/freestnd-cxx-hdrs/x86_64/include

# ---- Linker flags ----

LDFLAGS := \
    -nostdlib \
    -static \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    -Wl,-m,elf_x86_64 \
    -z max-page-size=0x1000 \
    -T $(LINK_LD)

# ---- Libraries ----

LIBS := $(TLS_LIB)/libtls.a $(BEARSSL)/libbearssl.a $(LIBC_LIB)/liblibc.a

# ---- Source files ----

SRCS := main.cpp stb_truetype_impl.cpp
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))

# ---- Target ----

TARGET := $(BINDIR)/os/weather.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBS) $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/os
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(OBJDIR)/%.o: %.cpp Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
