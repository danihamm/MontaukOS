# Makefile for desktop (GUI desktop environment) on MontaukOS
# Copyright (c) 2025 Daniel Hammer

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
JPEG_LIB := ../../lib/libjpeg
LIBC_LIB := ../../lib/libc
BEARSSL  := ../../lib/bearssl
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

# ---- C++ source files ----

CORE_SRCS := main.cpp window.cpp panel.cpp compose.cpp input.cpp dialogs.cpp font_data.cpp stb_truetype_impl.cpp
APP_SRCS  := $(wildcard apps/*.cpp)
SRCS      := $(CORE_SRCS) $(APP_SRCS)
OBJS      := $(addprefix $(OBJDIR)/,$(notdir $(SRCS:.cpp=.o)))

# ---- Target ----

TARGET := $(BINDIR)/os/desktop.elf

.PHONY: all clean

all: $(TARGET)

# ---- Libraries ----

LIBS := $(JPEG_LIB)/libjpeg.a $(BEARSSL)/libbearssl.a $(LIBC_LIB)/liblibc.a

$(TARGET): $(OBJS) $(LIBS) $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/os
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

# Core sources
$(OBJDIR)/%.o: %.cpp Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# App sources
$(OBJDIR)/%.o: apps/%.cpp Makefile
	mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OBJDIR) $(TARGET)
