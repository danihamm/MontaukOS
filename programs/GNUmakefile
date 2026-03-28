# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:

# Target architecture.
ARCH := x86_64

# Auto-detect cross compiler from toolchain/local/.
TOOLCHAIN_PREFIX := $(shell cd .. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CXX := $(TOOLCHAIN_PREFIX)g++
    CC  := $(TOOLCHAIN_PREFIX)gcc
else
    CXX := g++
    CC  := gcc
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

# Programs with custom Makefiles (built separately).
CUSTOM_BUILDS := doom fetch wiki wikipedia weather imageviewer fontpreview spreadsheet wordprocessor pdfviewer disks devexplorer installer volume music video bluetooth terminal klog procmgr calculator desktop login shell rpgdemo paint tcc lua screenshot texteditor mandelbrot
SYSTEM_PROGRAMS := $(filter-out $(CUSTOM_BUILDS),$(PROGRAMS))

# Build targets: system programs go to bin/os/, apps go to bin/apps/<name>/.
TARGETS := $(addprefix $(BINDIR)/os/,$(addsuffix .elf,$(SYSTEM_PROGRAMS)))

# Man pages source directory.
MANDIR := man
MANSRC := $(wildcard $(MANDIR)/*.*)
MANDST := $(patsubst $(MANDIR)/%,$(BINDIR)/man/%,$(MANSRC))

# Web content source directory.
WWWDIR := www
WWWSRC := $(wildcard $(WWWDIR)/*.*)
WWWDST := $(patsubst $(WWWDIR)/%,$(BINDIR)/www/%,$(WWWSRC))

# CA certificate bundle.
CA_CERTS := $(BINDIR)/etc/ca-certificates.crt

# Common shared assets (wallpapers, etc.)
COMMONKEEP := $(BINDIR)/common/.keep

.PHONY: all clean doom fetch wiki wikipedia weather imageviewer fontpreview spreadsheet wordprocessor pdfviewer disks devexplorer installer volume music video bluetooth terminal klog procmgr calculator login desktop shell rpgdemo paint tcc lua screenshot texteditor mandelbrot icons fonts bearssl libc tls libjpeg libjpegwrite install-apps

all: bearssl libc libjpeg libjpegwrite tls $(TARGETS) fetch wiki wikipedia weather imageviewer fontpreview spreadsheet wordprocessor pdfviewer disks devexplorer installer volume music video bluetooth terminal klog procmgr calculator doom rpgdemo paint tcc lua screenshot texteditor mandelbrot login desktop shell icons fonts install-apps $(MANDST) $(WWWDST) $(CA_CERTS) $(COMMONKEEP)

# Build BearSSL static library (cross-compiled for freestanding x86_64).
BEARSSL_INCLUDES := -isystem $(shell cd .. && pwd)/kernel/freestnd-c-hdrs/x86_64/include -isystem $(abspath include/libc)

lib/bearssl/build/libbearssl.a:
	$(MAKE) -C lib/bearssl CONF=MontaukOS \
		BEARSSL_CC="$(CC)" \
		BEARSSL_AR="$(TOOLCHAIN_PREFIX)ar" \
		BEARSSL_INCLUDES="$(BEARSSL_INCLUDES)"

bearssl: lib/bearssl/build/libbearssl.a

# Build shared libc static library.
libc:
	$(MAKE) -C lib/libc

# Build shared TLS helper library.
tls: bearssl libc
	$(MAKE) -C lib/tls

# Build fetch via its own Makefile (depends on bearssl, libc, and tls).
fetch: bearssl libc tls
	$(MAKE) -C src/fetch

# Build wiki via its own Makefile (depends on bearssl, libc, and tls).
wiki: bearssl libc tls
	$(MAKE) -C src/wiki

# Build wikipedia standalone GUI client (depends on bearssl, libc, and tls).
wikipedia: bearssl libc tls
	$(MAKE) -C src/wikipedia

# Build weather standalone GUI client (depends on bearssl, libc, and tls).
weather: bearssl libc tls
	$(MAKE) -C src/weather

# Build libjpeg static library (JPEG decoding via stb_image).
libjpeg: libc
	$(MAKE) -C lib/libjpeg

# Build libjpegwrite static library (JPEG encoding via stb_image_write).
libjpegwrite: libc
	$(MAKE) -C lib/libjpegwrite

# Build imageviewer standalone GUI client (depends on libc and libjpeg).
imageviewer: libc libjpeg
	$(MAKE) -C src/imageviewer

# Build fontpreview standalone GUI client (no library deps).
fontpreview:
	$(MAKE) -C src/fontpreview

# Build spreadsheet standalone GUI client (depends on libc).
spreadsheet: libc
	$(MAKE) -C src/spreadsheet

# Build word processor standalone GUI client (depends on libc).
wordprocessor: libc
	$(MAKE) -C src/wordprocessor

# Build PDF viewer standalone GUI client (depends on libc).
pdfviewer: libc
	$(MAKE) -C src/pdfviewer

# Build disks standalone GUI tool (depends on libc).
disks: libc
	$(MAKE) -C src/disks

# Build devexplorer standalone GUI tool (depends on libc).
devexplorer: libc
	$(MAKE) -C src/devexplorer

# Build installer standalone GUI tool (depends on libc).
installer: libc
	$(MAKE) -C src/installer

# Build volume control standalone GUI tool (depends on libc).
volume: libc
	$(MAKE) -C src/volume

# Build music player standalone GUI tool (depends on libc).
music: libc
	$(MAKE) -C src/music

# Build videos standalone GUI tool (depends on libc).
video: libc
	$(MAKE) -C src/video

# Build bluetooth manager standalone GUI tool (depends on libc).
bluetooth: libc
	$(MAKE) -C src/bluetooth

# Build terminal standalone GUI tool (depends on libc).
terminal: libc
	$(MAKE) -C src/terminal

# Build kernel log standalone GUI tool (depends on libc).
klog: libc
	$(MAKE) -C src/klog

# Build process manager standalone GUI tool (depends on libc).
procmgr: libc
	$(MAKE) -C src/procmgr

# Build calculator standalone GUI tool (depends on libc).
calculator: libc
	$(MAKE) -C src/calculator

# Build login screen via its own Makefile (depends on bearssl and libc).
login: bearssl libc
	$(MAKE) -C src/login

# Build shell via its own Makefile (multi-file build).
shell:
	$(MAKE) -C src/shell

# Build desktop via its own Makefile (depends on libc, libjpeg, and bearssl for user.h).
desktop: libc libjpeg bearssl
	$(MAKE) -C src/desktop

# Copy SVG icons for the desktop into bin/icons/.
icons:
	../scripts/copy_icons.sh

# Copy TTF fonts for the desktop into bin/fonts/.
fonts:
	../scripts/copy_fonts.sh

# Build paint standalone GUI tool (depends on libc).
paint: libc
	$(MAKE) -C src/paint

# Build text editor standalone GUI app (depends on libc).
texteditor: libc
	$(MAKE) -C src/texteditor

# Build Mandelbrot standalone GUI app (depends on libc).
mandelbrot: libc
	$(MAKE) -C src/mandelbrot

# Build RPG demo game via its own Makefile (depends on libc).
rpgdemo: libc
	$(MAKE) -C src/rpgdemo

# Build doom via its own Makefile (depends on libc).
doom: libc
	$(MAKE) -C src/doom

# Build screenshot tool (depends on libc and libjpegwrite).
screenshot: libc libjpegwrite
	$(MAKE) -C src/screenshot

# Build TCC (Tiny C Compiler) via its own Makefile (depends on libc).
tcc: libc
	$(MAKE) -C src/tcc
	mkdir -p $(BINDIR)/lib/tcc/include
	cp -r src/tcc/tcc_include/* $(BINDIR)/lib/tcc/include/
	cp include/libc/*.h $(BINDIR)/lib/tcc/include/
	mkdir -p $(BINDIR)/lib/tcc/include/sys
	cp include/libc/sys/*.h $(BINDIR)/lib/tcc/include/sys/

# Build Lua interpreter and static library (depends on libc).
lua: libc
	$(MAKE) -C src/lua

# Install app bundles (manifests, icons, data files) into bin/apps/<name>/.
install-apps: doom rpgdemo paint spreadsheet wordprocessor weather wikipedia imageviewer fontpreview pdfviewer disks devexplorer installer volume music video bluetooth terminal klog procmgr calculator screenshot texteditor mandelbrot
	../scripts/install_apps.sh

# Copy man pages into bin/man/ so mkramdisk.sh picks them up.
$(BINDIR)/man/%: $(MANDIR)/%
	mkdir -p $(BINDIR)/man
	cp $< $@

# Copy web content into bin/www/ so mkramdisk.sh picks them up.
$(BINDIR)/www/%: $(WWWDIR)/%
	mkdir -p $(BINDIR)/www
	cp $< $@

# Copy CA certificate bundle into bin/etc/.
$(CA_CERTS): data/ca-certificates.crt
	mkdir -p $(BINDIR)/etc
	cp $< $@

# Ensure common assets directory exists.
$(COMMONKEEP):
	mkdir -p $(BINDIR)/common
	touch $@

# Build each system program from its source files.
$(BINDIR)/os/%.elf: src/%/main.cpp link.ld GNUmakefile
	mkdir -p $(BINDIR)/os obj/$*
	$(CXX) $(CXXFLAGS) -c src/$*/main.cpp -o obj/$*/main.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) obj/$*/main.o -o $@

clean:
	rm -rf $(BINDIR) obj
	$(MAKE) -C lib/bearssl clean
	$(MAKE) -C lib/libc clean
	$(MAKE) -C lib/libjpeg clean
	$(MAKE) -C lib/libjpegwrite clean
	$(MAKE) -C lib/tls clean
	$(MAKE) -C src/fetch clean
	$(MAKE) -C src/doom clean
	$(MAKE) -C src/login clean
	$(MAKE) -C src/shell clean
	$(MAKE) -C src/desktop clean
	$(MAKE) -C src/wikipedia clean
	$(MAKE) -C src/weather clean
	$(MAKE) -C src/imageviewer clean
	$(MAKE) -C src/fontpreview clean
	$(MAKE) -C src/spreadsheet clean
	$(MAKE) -C src/wordprocessor clean
	$(MAKE) -C src/pdfviewer clean
	$(MAKE) -C src/disks clean
	$(MAKE) -C src/devexplorer clean
	$(MAKE) -C src/installer clean
	$(MAKE) -C src/volume clean
	$(MAKE) -C src/music clean
	$(MAKE) -C src/video clean
	$(MAKE) -C src/bluetooth clean
	$(MAKE) -C src/terminal clean
	$(MAKE) -C src/klog clean
	$(MAKE) -C src/procmgr clean
	$(MAKE) -C src/calculator clean
	$(MAKE) -C src/paint clean
	$(MAKE) -C src/rpgdemo clean
	$(MAKE) -C src/screenshot clean
	$(MAKE) -C src/mandelbrot clean
	$(MAKE) -C src/tcc clean
