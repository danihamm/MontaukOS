/*
 * pdfviewer.h
 * Shared header for the MontaukOS PDF viewer app
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

static constexpr int INIT_W        = 800;
static constexpr int INIT_H        = 700;
static constexpr int TOOLBAR_H     = 36;
static constexpr int STATUS_BAR_H  = 24;
static constexpr int TB_BTN_SIZE   = 24;
static constexpr int TB_BTN_Y      = 6;
static constexpr int TB_BTN_RAD    = 3;
static constexpr int PATHBAR_H     = 32;
static constexpr int PAGE_MARGIN   = 20;
static constexpr int PAGE_PAD      = 8;
static constexpr int FONT_SIZE     = 18;
static constexpr int HEADER_FONT   = 16;
static constexpr int SCROLL_STEP   = 40;
static constexpr int MAX_TEXT_LEN  = 128;
static constexpr int MAX_OPERANDS  = 16;

// Colors
static constexpr Color BG_COLOR      = Color::from_rgb(0x80, 0x80, 0x80);
static constexpr Color PAGE_COLOR    = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color PAGE_SHADOW   = Color::from_rgb(0x60, 0x60, 0x60);
static constexpr Color TEXT_COLOR    = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color TOOLBAR_BG   = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color HEADER_TEXT   = Color::from_rgb(0x55, 0x55, 0x55);
static constexpr Color TB_BTN_BG    = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color TB_BTN_ACTIVE = Color::from_rgb(0xC0, 0xD0, 0xE8);
static constexpr Color TB_SEP_COLOR  = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color GRID_COLOR    = Color::from_rgb(0xD0, 0xD0, 0xD0);
static constexpr Color STATUS_BG     = Color::from_rgb(0x2B, 0x3E, 0x50);
static constexpr Color STATUS_TEXT   = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color HEADER_BG     = Color::from_rgb(0xF0, 0xF0, 0xF0);

// ============================================================================
// Types
// ============================================================================

struct TextItem {
    float x, y;         // PDF user-space coordinates (origin bottom-left)
    float font_size;    // effective font size in points
    char text[MAX_TEXT_LEN];
    uint8_t flags;      // bit 0: bold, bit 1: italic, bit 2: monospace
    TrueTypeFont* font; // embedded font, or nullptr for system font
};

struct PdfPage {
    TextItem* items;
    int item_count;
    int item_cap;
    float width, height; // page dimensions in points (from MediaBox)
};

struct EmbeddedFontEntry {
    int stream_obj;         // FontFile stream object number
    TrueTypeFont* font;    // loaded font
    uint8_t* font_data;    // heap-allocated font data (must persist for stb_truetype)
};

struct PdfDoc {
    uint8_t* data;
    int data_len;

    int* xref;          // byte offset for each object number
    int xref_count;

    PdfPage* pages;
    int* page_objs;     // object number for each page
    int page_count;
    int page_cap;

    EmbeddedFontEntry* emb_fonts; // cached embedded fonts
    int emb_font_count;

    bool valid;
};

struct FontInfo {
    char name[32];      // PDF font name (e.g., "F1")
    uint8_t flags;      // bit 0: bold, bit 1: italic, bit 2: mono
    uint16_t* tounicode; // glyph ID -> Unicode codepoint (256 entries), heap-allocated
    TrueTypeFont* embedded_font; // loaded from PDF font stream, or nullptr
};

struct FontMap {
    FontInfo fonts[32];
    int count;
};

// ============================================================================
// Global state (extern -- defined in main.cpp)
// ============================================================================

extern int g_win_w, g_win_h;
extern PdfDoc g_doc;
extern int g_current_page;
extern int g_scroll_y;
extern float g_zoom;

extern char g_filepath[256];

extern bool g_pathbar_open;
extern char g_pathbar_text[256];
extern int  g_pathbar_len;
extern int  g_pathbar_cursor;

extern TrueTypeFont* g_font;
extern TrueTypeFont* g_font_bold;
extern TrueTypeFont* g_font_mono;

extern char g_status_msg[128];

// ============================================================================
// helpers.cpp
// ============================================================================

void px_fill(uint32_t* px, int bw, int bh, int x, int y, int w, int h, Color c);
void px_hline(uint32_t* px, int bw, int bh, int x, int y, int w, Color c);
void px_vline(uint32_t* px, int bw, int bh, int x, int y, int h, Color c);
void px_rect(uint32_t* px, int bw, int bh, int x, int y, int w, int h, Color c);
void px_fill_rounded(uint32_t* px, int bw, int bh, int x, int y, int w, int h, int r, Color c);
int  str_len(const char* s);
void str_cpy(char* dst, const char* src, int max);

// ============================================================================
// pdf_parser.cpp
// ============================================================================

bool load_pdf(const char* path);
void free_pdf();

// Internal utilities used by pdf_page.cpp
int  skip_ws(const uint8_t* d, int len, int p);
int  parse_int_at(const uint8_t* d, int len, int p, int* val);
int  parse_real_at(const uint8_t* d, int len, int p, float* val);
bool starts_with(const uint8_t* d, int len, int p, const char* s);
int  dict_lookup(const uint8_t* d, int len, int pos, const char* key);
int  parse_ref_at(const uint8_t* d, int len, int p, int* obj_num);
int  find_obj_content(int obj_num, int* start, int* end);
uint8_t* get_stream_data(int obj_num, int* out_len);
void build_font_map(int page_obj_num, FontMap* out);

// ============================================================================
// pdf_page.cpp
// ============================================================================

void parse_page(int page_idx, int page_obj_num);

// ============================================================================
// render.cpp
// ============================================================================

void render(uint32_t* pixels);
