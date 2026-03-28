# Makefile for video (standalone Videos app) on MontaukOS
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
    -I $(PROG_INC) \
    -I . \
    -isystem $(PROG_INC)/libc \
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

SRCS := main.cpp avi.cpp stb_image_impl.cpp stb_truetype_impl.cpp
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))

TARGET := $(BINDIR)/apps/video/video.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/apps/video
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LIBDIR)/libc/liblibc.a -o $@

HEADERS := avi.hpp

$(OBJDIR)/%.o: %.cpp $(HEADERS) Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
