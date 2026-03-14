# Makefile for standalone MontaukOS app (template)
# Usage:
#   make              Build the app
#   make clean        Remove build artifacts
#   make install      Copy ELF to MontaukOS ramdisk bin directory
#
# Copy this entire directory, rename it, and edit APP_NAME/SRCS below.

MAKEFLAGS += -rR
.SUFFIXES:

# ---- App configuration ----
# Change these for your app:

APP_NAME := myapp
SRCS     := src/main.cpp src/stb_truetype_impl.cpp src/cxxrt.cpp

# Optional features — set to 1 to enable:
#   USE_TLS=1    HTTPS/TLS support (libtls + libbearssl)
#   USE_JPEG=1   JPEG image decoding (libjpeg / stb_image)
USE_TLS  ?= 0
USE_JPEG ?= 0

# ---- Toolchain ----
# Looks for the cross-compiler via MONTAUKOS, then falls back to system g++.

MONTAUKOS   ?= $(HOME)/dev/MontaukOS
TOOLCHAIN   := $(MONTAUKOS)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN)gcc),)
    CXX := $(TOOLCHAIN)g++
else
    CXX := g++
endif

# ---- Paths ----

SYSROOT  := sysroot
OBJDIR   := obj
BINDIR   := bin
LINK_LD  := link.ld

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
    -I $(SYSROOT)/include \
    -I src \
    -isystem $(SYSROOT)/include/libc

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
# Order matters: libtls depends on libbearssl, everything depends on liblibc.

LIBS :=
ifeq ($(USE_TLS),1)
    LIBS += $(SYSROOT)/lib/libtls.a $(SYSROOT)/lib/libbearssl.a
endif
ifeq ($(USE_JPEG),1)
    LIBS += $(SYSROOT)/lib/libjpeg.a
endif
LIBS += $(SYSROOT)/lib/liblibc.a

# ---- Build rules ----

OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.cpp=.o))
TARGET := $(BINDIR)/$(APP_NAME).elf

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS) $(LINK_LD) Makefile
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	@echo "Built: $@"

$(OBJDIR)/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(BINDIR)

# Copy the built ELF into the MontaukOS ramdisk tree for testing.
install: $(TARGET)
	@mkdir -p $(MONTAUKOS)/programs/bin/apps/$(APP_NAME)
	cp $(TARGET) $(MONTAUKOS)/programs/bin/apps/$(APP_NAME)/$(APP_NAME).elf
	@echo "Installed to $(MONTAUKOS)/programs/bin/apps/$(APP_NAME)/"
