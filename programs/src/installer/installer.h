/*
 * installer.h
 * Shared header for the MontaukOS Installer
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

static constexpr int INIT_W       = 520;
static constexpr int INIT_H       = 440;
static constexpr int TOOLBAR_H    = 36;
static constexpr int STEP_BAR_H   = 32;
static constexpr int CONTENT_TOP  = TOOLBAR_H + STEP_BAR_H;
static constexpr int MAX_DISKS    = 8;
static constexpr int MAX_PARTS    = 32;
static constexpr int STATUS_H     = 26;

// Font sizes — adjusted at runtime by apply_scale()
extern int FONT_SIZE;
extern int FONT_SM;

static constexpr int TB_BTN_Y     = 5;
static constexpr int TB_BTN_H     = 26;
static constexpr int TB_BTN_RAD   = 8;

static constexpr Color BG_COLOR       = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TOOLBAR_BG     = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color BORDER_COLOR   = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color TEXT_COLOR     = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color DIM_TEXT       = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color FAINT_TEXT     = Color::from_rgb(0x88, 0x88, 0x88);
static constexpr Color WHITE          = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color ACCENT         = Color::from_rgb(0x33, 0x66, 0xCC);
static constexpr Color DANGER         = Color::from_rgb(0xCC, 0x33, 0x33);
static constexpr Color SUCCESS_COLOR  = Color::from_rgb(0x33, 0x99, 0x33);
static constexpr Color DISABLED_BG    = Color::from_rgb(0xBB, 0xBB, 0xBB);

// ============================================================================
// Install steps
// ============================================================================

enum InstallerMode {
    MODE_INSTALL = 0,
    MODE_UPDATE,
};

enum InstallStep {
    STEP_MODE_SELECT,
    STEP_SELECT_DISK,
    STEP_PARTITION_SCHEME,
    STEP_CONFIRM,
    STEP_INSTALLING,
    STEP_DONE,
    STEP_ERROR,
    // Update flow
    STEP_UPDATE_SELECT_PART,
    STEP_UPDATE_CONFIRM,
    STEP_UPDATING,
};

// ============================================================================
// Partition schemes
// ============================================================================

enum PartScheme {
    SCHEME_EFI_EXT2 = 0,
    SCHEME_SINGLE_FAT32,
    SCHEME_COUNT,
};

static const char* scheme_names[] = {
    "EFI + ext2 (recommended)",
    "Single FAT32 partition (entire disk)",
};

static const char* scheme_descs[] = {
    "EFI boot partition + ext2 root filesystem.",
    "One partition for boot, OS, and data. Simple and compatible.",
};

// ============================================================================
// State
// ============================================================================

static constexpr int LOG_LINES    = 16;
static constexpr int LOG_LINE_LEN = 64;

struct InstallerState {
    int mode;  // InstallerMode

    // Install flow
    Montauk::DiskInfo disks[MAX_DISKS];
    int disk_count;
    int selected_disk;
    int partition_scheme;

    // Update flow
    Montauk::PartInfo parts[MAX_PARTS];
    int part_count;
    int selected_part;

    InstallStep step;
    char log[LOG_LINES][LOG_LINE_LEN];
    int  log_count;

    char status[80];
    uint64_t status_time;
};

// ============================================================================
// Global state (extern — defined in main.cpp)
// ============================================================================

extern int g_win_w, g_win_h;
extern InstallerState g_state;
extern TrueTypeFont* g_font;
extern uint32_t* g_pixels;
extern uint32_t* g_backbuf;
extern int g_win_id;

// ============================================================================
// Function declarations — helpers (main.cpp)
// ============================================================================

void set_status(const char* msg);
void add_log(const char* msg);
void flush_ui();
void format_disk_size(char* buf, int bufsize, uint64_t sectors, uint16_t sectorSize);
void apply_scale(int scale);

// ============================================================================
// Function declarations — render.cpp
// ============================================================================

void render(uint32_t* pixels);

// ============================================================================
// Function declarations — actions.cpp
// ============================================================================

void installer_refresh_disks();
void installer_refresh_parts();
void do_install();
void do_update();
