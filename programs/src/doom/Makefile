# Makefile for DOOM (doomgeneric) on ZenithOS
# Copyright (c) 2025 Daniel Hammer

MAKEFLAGS += -rR
.SUFFIXES:

# ---- Toolchain ----

TOOLCHAIN_PREFIX := $(shell cd ../../.. && pwd)/toolchain/local/bin/x86_64-elf-
ifneq ($(wildcard $(TOOLCHAIN_PREFIX)gcc),)
    CC := $(TOOLCHAIN_PREFIX)gcc
    LD := $(TOOLCHAIN_PREFIX)gcc
else
    CC := gcc
    LD := gcc
endif

# ---- Paths ----

DOOM_SRC   := ../../../doomgeneric/doomgeneric
LIBC_INC   := ../../include/libc
PROG_INC   := ../../include
LINK_LD    := ../../link.ld
BINDIR     := ../../bin
OBJDIR     := obj

# ---- Compiler flags ----

CFLAGS := \
    -std=gnu11 \
    -g -O2 -pipe \
    -Wall \
    -Wno-unused-parameter \
    -Wno-unused-variable \
    -Wno-missing-field-initializers \
    -Wno-sign-compare \
    -Wno-pointer-sign \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -msse \
    -msse2 \
    -mno-red-zone \
    -mcmodel=small \
    -DNORMALUNIX \
    -DLINUX \
    -isystem $(LIBC_INC) \
    -I $(PROG_INC) \
    -I $(DOOM_SRC) \
    -isystem $(shell $(CC) -print-file-name=include)

# ---- Linker flags ----

LDFLAGS := \
    -nostdlib \
    -static \
    -Wl,--build-id=none \
    -Wl,--gc-sections \
    -Wl,-m,elf_x86_64 \
    -z max-page-size=0x1000 \
    -T $(LINK_LD)

# ---- DOOM source files (excluding platform-specific ports and sound backends) ----

DOOM_SRCS := \
    am_map.c \
    d_event.c \
    d_items.c \
    d_iwad.c \
    d_loop.c \
    d_main.c \
    d_mode.c \
    d_net.c \
    doomdef.c \
    doomgeneric.c \
    doomstat.c \
    dstrings.c \
    dummy.c \
    f_finale.c \
    f_wipe.c \
    g_game.c \
    gusconf.c \
    hu_lib.c \
    hu_stuff.c \
    i_cdmus.c \
    i_endoom.c \
    i_input.c \
    i_joystick.c \
    i_scale.c \
    i_sound.c \
    i_system.c \
    i_timer.c \
    i_video.c \
    info.c \
    m_argv.c \
    m_bbox.c \
    m_cheat.c \
    m_config.c \
    m_controls.c \
    m_fixed.c \
    m_menu.c \
    m_misc.c \
    m_random.c \
    memio.c \
    mus2mid.c \
    p_ceilng.c \
    p_doors.c \
    p_enemy.c \
    p_floor.c \
    p_inter.c \
    p_lights.c \
    p_map.c \
    p_maputl.c \
    p_mobj.c \
    p_plats.c \
    p_pspr.c \
    p_saveg.c \
    p_setup.c \
    p_sight.c \
    p_spec.c \
    p_switch.c \
    p_telept.c \
    p_tick.c \
    p_user.c \
    r_bsp.c \
    r_data.c \
    r_draw.c \
    r_main.c \
    r_plane.c \
    r_segs.c \
    r_sky.c \
    r_things.c \
    s_sound.c \
    sha1.c \
    sounds.c \
    st_lib.c \
    st_stuff.c \
    statdump.c \
    tables.c \
    v_video.c \
    w_checksum.c \
    w_file.c \
    w_file_stdc.c \
    w_main.c \
    w_wad.c \
    wi_stuff.c \
    z_zone.c

# Local source files
LOCAL_SRCS := doomgeneric_zenith.c libc.c

# ---- Object files ----

DOOM_OBJS  := $(addprefix $(OBJDIR)/,$(DOOM_SRCS:.c=.o))
LOCAL_OBJS := $(addprefix $(OBJDIR)/,$(LOCAL_SRCS:.c=.o))
ALL_OBJS   := $(DOOM_OBJS) $(LOCAL_OBJS)

# ---- Target ----

TARGET := $(BINDIR)/games/doom.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(ALL_OBJS) $(LINK_LD) Makefile
	mkdir -p $(BINDIR)/games
	$(LD) $(CFLAGS) $(LDFLAGS) $(ALL_OBJS) -o $@

# DOOM source files (from doomgeneric directory)
$(OBJDIR)/%.o: $(DOOM_SRC)/%.c Makefile
	mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Local source files
$(OBJDIR)/%.o: %.c Makefile
	mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(TARGET)
