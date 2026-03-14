# Makefile for shell (interactive shell) on MontaukOS
# Copyright (c) 2025-2026 Daniel Hammer

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
LINK_LD  := ../../link.ld
BINDIR   := ../../bin
OBJDIR   := obj

# ---- C++ compiler flags ----

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
    -MMD -MP \
    -I . \
    -I $(PROG_INC) \
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

SRCS := main.cpp vars.cpp builtins.cpp exec.cpp
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))

# ---- Target ----

TARGET := $(BINDIR)/os/shell.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/os
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(OBJDIR)/%.o: %.cpp shell.h Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OBJDIR) $(TARGET)
