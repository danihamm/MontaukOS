# Makefile for login (graphical login screen) on MontaukOS
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

PROG_INC := ../../include
LIBC_LIB := ../../lib/libc
JPEG_LIB := ../../lib/libjpeg
BEARSSL  := ../../lib/bearssl
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
    -isystem $(PROG_INC)/libc \
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

# ---- Source files ----

SRCS := main.cpp stb_truetype_impl.cpp font_data.cpp
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))
DEPS := $(OBJS:.o=.d)

# ---- Target ----

TARGET := $(BINDIR)/os/login.elf

.PHONY: all clean

all: $(TARGET)

LIBS := $(JPEG_LIB)/libjpeg.a $(BEARSSL)/libbearssl.a $(LIBC_LIB)/liblibc.a

$(TARGET): $(OBJS) $(LIBS) $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/os
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(OBJDIR)/%.o: %.cpp Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(OBJDIR) $(TARGET)
