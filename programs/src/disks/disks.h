/*
 * disks.h
 * Shared header for the MontaukOS Disk Tool
 * Copyright (c) 2026 Daniel Hammer
 */

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
}

using namespace gui;

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W       = 640;
static constexpr int INIT_H       = 460;
static constexpr int TOOLBAR_H    = 40;
static constexpr int HEADER_H     = 26;
static constexpr int ITEM_H       = 32;
static constexpr int MAP_H        = 48;
static constexpr int MAP_PAD      = 16;
static constexpr int MAX_PARTS    = 32;
static constexpr int MAX_DISKS    = 8;
static constexpr int STATUS_H     = 26;
static constexpr int FONT_SIZE    = 16;

static constexpr int TB_BTN_Y     = 7;
static constexpr int TB_BTN_H     = 26;
static constexpr int TB_BTN_RAD   = 6;

static constexpr Color BG_COLOR       = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TOOLBAR_BG     = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color HEADER_BG      = Color::from_rgb(0xF0, 0xF0, 0xF0);
static constexpr Color BORDER_COLOR   = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color TEXT_COLOR     = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color DIM_TEXT       = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color FAINT_TEXT     = Color::from_rgb(0x88, 0x88, 0x88);
static constexpr Color HOVER_BG       = Color::from_rgb(0xE8, 0xF0, 0xF8);
static constexpr Color STATUS_BG_COL  = Color::from_rgb(0xF0, 0xF0, 0xF0);
static constexpr Color WHITE          = Color::from_rgb(0xFF, 0xFF, 0xFF);

static constexpr int NUM_PART_COLORS = 8;

// ============================================================================
// Filesystem types
// ============================================================================

struct FsTypeEntry {
    const char* name;
    int         id;
};

static const FsTypeEntry g_fsTypes[] = {
    { "FAT32", Montauk::FS_TYPE_FAT32 },
};
static constexpr int NUM_FS_TYPES = 1;

// ============================================================================
// State
// ============================================================================

static constexpr int FMT_DLG_W = 280;
static constexpr int FMT_DLG_H = 220;
static constexpr int NP_DLG_W  = 340;
static constexpr int NP_DLG_H  = 200;

struct FormatDialog {
    bool open;
    int  global_part_index;
    int  selected_fs;
    bool hover_format;
    bool hover_cancel;
    char part_desc[80];
    int  win_id;
    uint32_t* pixels;
};

struct NewPartDialog {
    bool open;
    bool will_init_gpt;     // true if GPT init is needed (most destructive)
    bool hover_confirm;
    bool hover_cancel;
    char disk_desc[80];
    char warn_line1[96];
    char warn_line2[96];
    int  win_id;
    uint32_t* pixels;
};

struct DiskToolState {
    Montauk::DiskInfo disks[MAX_DISKS];
    int disk_count;
    Montauk::PartInfo parts[MAX_PARTS];
    int part_count;
    int selected_disk;
    int selected_part;
    int scroll_y;
    char status[80];
    uint64_t status_time;
    FormatDialog fmt_dlg;
    NewPartDialog np_dlg;
};

// ============================================================================
// Global state (extern — defined in main.cpp)
// ============================================================================

extern int g_win_w, g_win_h;
extern DiskToolState g_state;
extern TrueTypeFont* g_font;

// ============================================================================
// Partition colors
// ============================================================================

extern const Color part_colors[NUM_PART_COLORS];

// ============================================================================
// Function declarations — helpers (main.cpp)
// ============================================================================

void set_status(const char* msg);
int  get_disk_parts(int* indices, int max);
void format_disk_size(char* buf, int bufsize, uint64_t sectors, uint16_t sectorSize);

// ============================================================================
// Function declarations — render.cpp
// ============================================================================

void render(uint32_t* pixels);
void render_format_window();
void render_newpart_window();

// ============================================================================
// Function declarations — actions.cpp
// ============================================================================

void disktool_refresh();
void do_create_partition();
void do_mount_partition();
void open_format_dialog();
void close_format_dialog();
void format_dialog_do_format();
void open_newpart_dialog();
void close_newpart_dialog();
void newpart_dialog_confirm();
