# Makefile for printers (standalone Window Server app) on MontaukOS
# Copyright (c) 2026 Daniel Hammer

MAKEFLAGS += -rR
.SUFFIXES:

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CXX := $(TOOLCHAIN_PREFIX)g++
else
    CXX := g++
endif

PROG_INC := ../../include
LINK_LD  := ../../link.ld
BINDIR   := ../../bin
OBJDIR   := obj
LIBDIR   := ../../lib

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
    -MMD -MP \
    -I $(PROG_INC) \
    -isystem $(PROG_INC)/libc \
    -I ../../lib/bearssl/inc \
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

SRCS := main.cpp stb_truetype_impl.cpp font_data.cpp
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))

TARGET := $(BINDIR)/apps/printers/printers.elf
LIBS := $(LIBDIR)/tls/libtls.a $(LIBDIR)/bearssl/libbearssl.a $(LIBDIR)/libc/liblibc.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(LINK_LD) Makefile $(LIBS)
	mkdir -p $(BINDIR)/apps/printers
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(OBJDIR)/%.o: %.cpp Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OBJDIR) $(TARGET)
