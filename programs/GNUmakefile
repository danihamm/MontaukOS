# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:

# Target architecture.
ARCH := x86_64

# Auto-detect cross compiler from toolchain/local/.
TOOLCHAIN_PREFIX := $(shell cd .. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CXX := $(TOOLCHAIN_PREFIX)g++
else
    CXX := g++
endif

# Compiler flags: freestanding, no stdlib, kernel-mode compatible.
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
    -I include \
    -isystem ../kernel/freestnd-c-hdrs/x86_64/include \
    -isystem ../kernel/freestnd-cxx-hdrs/x86_64/include

# Linker flags: freestanding static ELF.
override LDFLAGS := \
    -nostdlib \
    -static \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    -Wl,-m,elf_x86_64 \
    -z max-page-size=0x1000 \
    -T link.ld

# Output directory.
BINDIR := bin

# Discover all programs (each subdirectory under src/ is a program).
PROGRAMS := $(notdir $(wildcard src/*))

# Games are built separately (doom has its own Makefile).
GAMES := doom
SYSTEM_PROGRAMS := $(filter-out $(GAMES),$(PROGRAMS))

# Build targets: system programs go to bin/os/, games are handled separately.
TARGETS := $(addprefix $(BINDIR)/os/,$(addsuffix .elf,$(SYSTEM_PROGRAMS)))

# Game data files to copy into bin/games/.
GAME_DATA := $(BINDIR)/games/doom1.wad

# Man pages source directory.
MANDIR := man
MANSRC := $(wildcard $(MANDIR)/*.*)
MANDST := $(patsubst $(MANDIR)/%,$(BINDIR)/man/%,$(MANSRC))

# Web content source directory.
WWWDIR := www
WWWSRC := $(wildcard $(WWWDIR)/*.*)
WWWDST := $(patsubst $(WWWDIR)/%,$(BINDIR)/www/%,$(WWWSRC))

# Home directory placeholder.
HOMEKEEP := $(BINDIR)/home/.keep

.PHONY: all clean doom

all: $(TARGETS) doom $(GAME_DATA) $(MANDST) $(WWWDST) $(HOMEKEEP)

# Build doom via its own Makefile.
doom:
	$(MAKE) -C src/doom

# Copy doom1.wad from data/games/ into bin/games/.
$(BINDIR)/games/doom1.wad: data/games/doom1.wad
	mkdir -p $(BINDIR)/games
	cp $< $@

# Copy man pages into bin/man/ so mkramdisk.sh picks them up.
$(BINDIR)/man/%: $(MANDIR)/%
	mkdir -p $(BINDIR)/man
	cp $< $@

# Copy web content into bin/www/ so mkramdisk.sh picks them up.
$(BINDIR)/www/%: $(WWWDIR)/%
	mkdir -p $(BINDIR)/www
	cp $< $@

# Create empty home directory with a keep file.
$(HOMEKEEP):
	mkdir -p $(BINDIR)/home
	touch $@

# Build each system program from its source files.
$(BINDIR)/os/%.elf: src/%/main.cpp link.ld GNUmakefile
	mkdir -p $(BINDIR)/os obj/$*
	$(CXX) $(CXXFLAGS) -c src/$*/main.cpp -o obj/$*/main.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) obj/$*/main.o -o $@

clean:
	rm -rf $(BINDIR) obj
	$(MAKE) -C src/doom clean
