/*
 * spreadsheet.h
 * Shared header for the MontaukOS spreadsheet app
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

static constexpr int INIT_W        = 900;
static constexpr int INIT_H        = 600;
static constexpr int TOOLBAR_H     = 36;
static constexpr int FORMULA_BAR_H = 36;
static constexpr int COL_HEADER_H  = 28;
static constexpr int ROW_HEADER_W  = 48;
static constexpr int STATUS_BAR_H  = 24;
static constexpr int SCROLL_STEP   = 50;
static constexpr int TB_BTN_SIZE   = 24;
static constexpr int TB_BTN_Y      = 6;
static constexpr int TB_BTN_RAD    = 3;

static constexpr int MAX_COLS      = 26;   // A-Z
static constexpr int MAX_ROWS      = 100;
static constexpr int DEF_COL_W     = 100;
static constexpr int MIN_COL_W     = 30;
static constexpr int ROW_H         = 26;
static constexpr int COL_RESIZE_GRAB = 5;  // pixels from border edge for grab zone

static constexpr int CELL_TEXT_MAX = 128;
static constexpr int FONT_SIZE     = 18;
static constexpr int HEADER_FONT   = 16;

static constexpr Color BG_COLOR       = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color GRID_COLOR     = Color::from_rgb(0xD0, 0xD0, 0xD0);
static constexpr Color HEADER_BG      = Color::from_rgb(0xF0, 0xF0, 0xF0);
static constexpr Color HEADER_TEXT    = Color::from_rgb(0x55, 0x55, 0x55);
static constexpr Color CELL_TEXT      = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color SELECT_BORDER  = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color SELECT_FILL    = Color::from_rgb(0xD8, 0xE8, 0xFD);
static constexpr Color FORMULA_BG     = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color FORMULA_BORDER = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color STATUS_BG      = Color::from_rgb(0x2B, 0x3E, 0x50);
static constexpr Color STATUS_TEXT    = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color NUM_COLOR      = Color::from_rgb(0x10, 0x50, 0x90);
static constexpr Color ERR_COLOR      = Color::from_rgb(0xCC, 0x22, 0x22);
static constexpr Color TOOLBAR_BG    = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color TB_BTN_BG     = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color TB_BTN_ACTIVE = Color::from_rgb(0xC0, 0xD0, 0xE8);
static constexpr Color TB_SEP_COLOR  = Color::from_rgb(0xCC, 0xCC, 0xCC);

// ============================================================================
// Types
// ============================================================================

enum CellAlign : uint8_t { ALIGN_AUTO = 0, ALIGN_LEFT = 1, ALIGN_CENTER = 2, ALIGN_RIGHT = 3 };
enum NumFormat : uint8_t { FMT_AUTO = 0, FMT_DECIMAL = 1, FMT_CURRENCY = 2, FMT_PERCENT = 3 };

enum CellType : uint8_t {
    CT_EMPTY = 0,
    CT_TEXT  = 1,
    CT_NUMBER = 2,
    CT_FORMULA = 3,
    CT_ERROR = 4,
};

struct Cell {
    char input[CELL_TEXT_MAX];   // raw user input
    char display[CELL_TEXT_MAX]; // computed display string
    double value;                // numeric value (for formulas/numbers)
    CellType type;
    CellAlign align;
    NumFormat fmt;
    bool bold;
};

static constexpr int CLIP_MAX_CELLS = 256;
struct ClipCell {
    char text[CELL_TEXT_MAX];
    int  rel_col;
    int  rel_row;
    CellAlign align;
    NumFormat fmt;
    bool bold;
};

static constexpr int UNDO_MAX = 6;
struct UndoCellData {
    char input[CELL_TEXT_MAX];
    CellAlign align;
    NumFormat fmt;
    bool bold;
};
struct UndoEntry {
    UndoCellData cells[MAX_ROWS][MAX_COLS];
};

static constexpr int PATHBAR_H = 32;

// ============================================================================
// Toolbar button positions
// ============================================================================

static constexpr int TB_OPEN_X0 = 4, TB_OPEN_X1 = 40;
static constexpr int TB_SAVE_X0 = 44, TB_SAVE_X1 = 80;
static constexpr int TB_CUT_X0 = 92, TB_CUT_X1 = 120;
static constexpr int TB_COPY_X0 = 124, TB_COPY_X1 = 160;
static constexpr int TB_PASTE_X0 = 164, TB_PASTE_X1 = 200;
static constexpr int TB_BOLD_X0 = 212, TB_BOLD_X1 = 236;
static constexpr int TB_AL_X0 = 248, TB_AL_X1 = 272;
static constexpr int TB_AC_X0 = 276, TB_AC_X1 = 300;
static constexpr int TB_AR_X0 = 304, TB_AR_X1 = 328;
static constexpr int TB_FMT_X0 = 340, TB_FMT_X1 = 404;
static constexpr int TB_UNDO_X0 = 416, TB_UNDO_X1 = 452;
static constexpr int TB_REDO_X0 = 456, TB_REDO_X1 = 492;

// ============================================================================
// Global state (extern — defined in main.cpp)
// ============================================================================

extern int g_win_w, g_win_h;
extern Cell g_cells[MAX_ROWS][MAX_COLS];
extern int g_sel_col, g_sel_row;
extern int g_scroll_x, g_scroll_y;
extern int g_col_widths[MAX_COLS];

extern bool g_editing;
extern char g_edit_buf[CELL_TEXT_MAX];
extern int  g_edit_len;
extern int  g_edit_cursor;

extern bool g_has_selection;
extern int  g_anchor_col, g_anchor_row;

extern ClipCell g_clipboard[CLIP_MAX_CELLS];
extern int  g_clip_count;
extern int  g_clip_cols, g_clip_rows;

extern char g_filepath[256];
extern bool g_modified;

extern bool g_pathbar_open;
extern bool g_pathbar_save;
extern char g_pathbar_text[256];
extern int  g_pathbar_len;
extern int  g_pathbar_cursor;

extern TrueTypeFont* g_font;
extern TrueTypeFont* g_font_bold;

extern bool g_fmt_dropdown_open;

extern bool g_col_resizing;
extern int  g_col_resize_idx;
extern int  g_col_resize_start_x;
extern int  g_col_resize_start_w;

extern UndoEntry* g_undo[UNDO_MAX + 1];
extern int g_undo_count;
extern int g_undo_pos;

// ============================================================================
// Function declarations — helpers.cpp
// ============================================================================

void px_fill(uint32_t* px, int bw, int bh, int x, int y, int w, int h, Color c);
void px_hline(uint32_t* px, int bw, int bh, int x, int y, int w, Color c);
void px_vline(uint32_t* px, int bw, int bh, int x, int y, int h, Color c);
void px_rect(uint32_t* px, int bw, int bh, int x, int y, int w, int h, Color c);
void px_fill_rounded(uint32_t* px, int bw, int bh, int x, int y, int w, int h, int r, Color c);

int  str_len(const char* s);
void str_cpy(char* dst, const char* src, int max);
bool is_digit(char c);
bool is_alpha(char c);
char to_upper(char c);
double str_to_double(const char* s, bool* ok);
void double_to_str(char* buf, int max, double v);

// ============================================================================
// Function declarations — formula.cpp
// ============================================================================

void eval_cell(int col, int row);
void eval_all_cells();
int  col_x(int col);
int  content_width();
int  content_height();
void cell_name(char* buf, int col, int row);
void format_value(char* buf, int max, double val, NumFormat fmt);

// ============================================================================
// Function declarations — fileio.cpp
// ============================================================================

void save_file();
void load_file(const char* path);

// ============================================================================
// Function declarations — edit.cpp
// ============================================================================

void undo_push();
void undo_do();
void redo_do();
void start_editing();
void commit_edit();
void cancel_edit();
void sel_range(int* c0, int* r0, int* c1, int* r1);
void clear_selection();
void copy_selection();
void cut_selection();
void paste_at_cursor();
void apply_bold_toggle();
void apply_align(CellAlign a);
void apply_format(NumFormat f);
void clamp_scroll();
void ensure_sel_visible();

// ============================================================================
// Function declarations — render.cpp
// ============================================================================

void render(uint32_t* pixels);

// ============================================================================
// Function declarations — main.cpp
// ============================================================================

bool hit_cell(int mx, int my, int* out_col, int* out_row);
bool handle_toolbar_click(int mx, int my);
bool handle_fmt_dropdown_click(int mx, int my);
