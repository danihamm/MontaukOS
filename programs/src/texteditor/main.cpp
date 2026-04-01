/*
 * main.cpp
 * MontaukOS Text Editor - standalone Window Server app
 * Single-buffer text editor with line numbers, cursor, scrolling, file I/O,
 * syntax highlighting for C and Lua files
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>
#include <gui/svg.hpp>
#include <gui/dialogs.hpp>
#include <gui/stb_math.h>

extern "C" {
#include <string.h>
#include <stdio.h>
}

using namespace gui;

#include "syntax_highlight.hpp"

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W         = 600;
static constexpr int INIT_H         = 450;
static constexpr int TOOLBAR_H      = 36;
static constexpr int PATHBAR_H      = 32;
static constexpr int STATUS_H       = 24;
static constexpr int LINE_NUM_W     = 48;
static constexpr int SCROLLBAR_W    = 12;
static constexpr int INIT_CAP       = 4096;
static constexpr int MAX_CAP        = 262144;  // 256KB
static constexpr int MAX_LINES      = 16384;
static constexpr int TAB_WIDTH      = 4;
static constexpr int FONT_SIZE      = 16;
static constexpr int SYN_LINE_BUF_CAP = 1024;

// Colors
static constexpr Color BG_COLOR         = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TOOLBAR_BG       = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color BTN_BG           = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color BORDER_COLOR     = Color::from_rgb(0xD0, 0xD0, 0xD0);
static constexpr Color LINENUM_COLOR    = Color::from_rgb(0x99, 0x99, 0x99);
static constexpr Color LINENUM_BG       = Color::from_rgb(0xF0, 0xF0, 0xF0);
static constexpr Color CURSOR_LINE_BG   = Color::from_rgb(0xFF, 0xFD, 0xE8);
static constexpr Color TEXT_COLOR       = Color::from_rgb(0x1E, 0x1E, 0x1E);
static constexpr Color SEL_BG           = Color::from_rgb(0xB0, 0xD0, 0xF0);
static constexpr Color ACCENT           = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color STATUS_BG        = Color::from_rgb(0x2B, 0x3E, 0x50);
static constexpr Color STATUS_TEXT      = Color::from_rgb(0xE0, 0xE0, 0xE0);
static constexpr Color PATHBAR_BG       = Color::from_rgb(0xF0, 0xF0, 0xF0);
static constexpr Color WHITE            = Color::from_rgb(0xFF, 0xFF, 0xFF);

// ============================================================================
// Global state
// ============================================================================

int g_win_w = INIT_W;
int g_win_h = INIT_H;

TrueTypeFont* g_font = nullptr;      // UI font (Roboto)
TrueTypeFont* g_font_mono = nullptr; // Editor font (JetBrains Mono)

SvgIcon g_icon_folder = {};
SvgIcon g_icon_save = {};

// Buffer
char* g_buffer = nullptr;
int g_buf_len = 0;
int g_buf_cap = 0;

// Line index
int* g_line_offsets = nullptr;
int g_line_count = 0;

// Cursor
int g_cursor_pos = 0;    // byte position
int g_cursor_line = 0;
int g_cursor_col = 0;
int g_scroll_y = 0;      // first visible line
int g_scroll_x = 0;      // horizontal scroll in pixels
bool g_modified = false;
bool g_cursor_moved = true;
char g_status_msg[128] = {};

// File
char g_filepath[256] = {};
char g_filename[64] = {};

// Selection
int  g_sel_anchor = 0;
int  g_sel_end = 0;
bool g_has_selection = false;
bool g_mouse_selecting = false;

// Pathbar
bool g_pathbar_open = false;
bool g_pathbar_save = false;
char g_pathbar_text[256] = {};
int  g_pathbar_len = 0;
int  g_pathbar_cursor = 0;

// Syntax highlighting
SynLanguage g_syntax_language = SYN_LANG_NONE;

// ============================================================================
// Pixel drawing helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

static void px_hline(uint32_t* px, int bw, int bh,
                     int x, int y, int w, Color c) {
    if (y < 0 || y >= bh) return;
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > bw ? bw : x + w;
    for (int col = x0; col < x1; col++)
        px[y * bw + col] = v;
}

static void px_vline(uint32_t* px, int bw, int bh,
                     int x, int y, int h, Color c) {
    if (x < 0 || x >= bw) return;
    uint32_t v = c.to_pixel();
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        px[row * bw + x] = v;
}

static void px_fill_rounded(uint32_t* px, int bw, int bh,
                            int x, int y, int w, int h, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= bh) continue;
        int inset = 0;
        if (row < r) {
            int dy = r - 1 - row;
            if (r == 3) {
                if (dy >= 2) inset = 2;
                else if (dy >= 1) inset = 1;
            }
        } else if (row >= h - r) {
            int dy = row - (h - r);
            if (r == 3) {
                if (dy >= 2) inset = 2;
                else if (dy >= 1) inset = 1;
            }
        }
        int x0 = x + inset;
        int x1 = x + w - inset;
        if (x0 < 0) x0 = 0;
        if (x1 > bw) x1 = bw;
        for (int col = x0; col < x1; col++)
            px[py * bw + col] = v;
    }
}

static void px_icon(uint32_t* px, int bw, int bh,
                    int x, int y, const SvgIcon& ic) {
    if (!ic.pixels) return;
    for (int row = 0; row < ic.height; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        for (int col = 0; col < ic.width; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            uint32_t src = ic.pixels[row * ic.width + col];
            uint8_t sa = (src >> 24) & 0xFF;
            if (sa == 0) continue;
            if (sa == 255) {
                px[dy * bw + dx] = src;
            } else {
                uint32_t dst = px[dy * bw + dx];
                uint8_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
                uint8_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
                uint32_t a = sa, inv = 255 - sa;
                uint32_t rr = (a * sr + inv * dr + 128) / 255;
                uint32_t gg = (a * sg + inv * dg + 128) / 255;
                uint32_t bb = (a * sb + inv * db + 128) / 255;
                px[dy * bw + dx] = 0xFF000000 | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
}

// ============================================================================
// Font metrics
// ============================================================================

static constexpr int MONO_SIZE = 18;

static int font_cell_w() {
    if (g_font_mono && g_font_mono->valid) {
        GlyphCache* gc = g_font_mono->get_cache(MONO_SIZE);
        CachedGlyph* g = g_font_mono->get_glyph(gc, 'M');
        if (g) return g->advance;
    }
    return 8;
}

static int font_cell_h() {
    if (g_font_mono && g_font_mono->valid)
        return g_font_mono->get_line_height(MONO_SIZE);
    return 16;
}

// ============================================================================
// Line index management
// ============================================================================

static void recompute_lines() {
    if (!g_line_offsets)
        g_line_offsets = (int*)montauk::malloc(MAX_LINES * sizeof(int));

    g_line_count = 0;
    g_line_offsets[g_line_count++] = 0;
    for (int i = 0; i < g_buf_len; i++) {
        if (g_buffer[i] == '\n' && g_line_count < MAX_LINES)
            g_line_offsets[g_line_count++] = i + 1;
    }
}

static void update_cursor_pos() {
    g_cursor_line = 0;
    for (int i = g_line_count - 1; i >= 0; i--) {
        if (g_cursor_pos >= g_line_offsets[i]) {
            g_cursor_line = i;
            break;
        }
    }
    g_cursor_col = g_cursor_pos - g_line_offsets[g_cursor_line];
    g_cursor_moved = true;
}

static int line_length(int line) {
    if (line < 0 || line >= g_line_count) return 0;
    int start = g_line_offsets[line];
    int end = (line + 1 < g_line_count) ? g_line_offsets[line + 1] - 1 : g_buf_len;
    return end - start;
}

// ============================================================================
// Buffer operations
// ============================================================================

static void ensure_capacity(int needed) {
    if (g_buf_len + needed <= g_buf_cap) return;
    int new_cap = g_buf_cap * 2;
    if (new_cap > MAX_CAP) new_cap = MAX_CAP;
    if (new_cap < g_buf_len + needed) new_cap = g_buf_len + needed;
    g_buffer = (char*)montauk::realloc(g_buffer, new_cap);
    g_buf_cap = new_cap;
}

static void delete_range(int start, int count) {
    for (int i = start; i < g_buf_len - count; i++)
        g_buffer[i] = g_buffer[i + count];
    g_buf_len -= count;
}

static void insert_char(char c) {
    if (g_buf_len >= MAX_CAP - 1) return;
    ensure_capacity(1);
    for (int i = g_buf_len; i > g_cursor_pos; i--)
        g_buffer[i] = g_buffer[i - 1];
    g_buffer[g_cursor_pos] = c;
    g_buf_len++;
    g_cursor_pos++;
    g_modified = true;
    recompute_lines();
    update_cursor_pos();
}

static void insert_string(const char* s, int len) {
    if (g_buf_len + len >= MAX_CAP) return;
    ensure_capacity(len);
    for (int i = g_buf_len - 1; i >= g_cursor_pos; i--)
        g_buffer[i + len] = g_buffer[i];
    for (int i = 0; i < len; i++)
        g_buffer[g_cursor_pos + i] = s[i];
    g_buf_len += len;
    g_cursor_pos += len;
    g_modified = true;
    recompute_lines();
    update_cursor_pos();
}

static void backspace() {
    if (g_cursor_pos <= 0) return;
    g_cursor_pos--;
    delete_range(g_cursor_pos, 1);
    g_modified = true;
    recompute_lines();
    update_cursor_pos();
}

static void delete_char() {
    if (g_cursor_pos >= g_buf_len) return;
    delete_range(g_cursor_pos, 1);
    g_modified = true;
    recompute_lines();
    update_cursor_pos();
}

// ============================================================================
// Cursor movement
// ============================================================================

static void move_up() {
    if (g_cursor_line <= 0) return;
    int col = g_cursor_col;
    int prev = g_cursor_line - 1;
    int len = line_length(prev);
    if (col > len) col = len;
    g_cursor_pos = g_line_offsets[prev] + col;
    update_cursor_pos();
}

static void move_down() {
    if (g_cursor_line >= g_line_count - 1) return;
    int col = g_cursor_col;
    int next = g_cursor_line + 1;
    int len = line_length(next);
    if (col > len) col = len;
    g_cursor_pos = g_line_offsets[next] + col;
    update_cursor_pos();
}

static void move_left() {
    if (g_cursor_pos > 0) { g_cursor_pos--; update_cursor_pos(); }
}

static void move_right() {
    if (g_cursor_pos < g_buf_len) { g_cursor_pos++; update_cursor_pos(); }
}

static void move_home() {
    g_cursor_pos = g_line_offsets[g_cursor_line];
    update_cursor_pos();
}

static void move_end() {
    g_cursor_pos = g_line_offsets[g_cursor_line] + line_length(g_cursor_line);
    update_cursor_pos();
}

// ============================================================================
// Selection
// ============================================================================

static void clear_selection() {
    g_has_selection = false;
    g_sel_anchor = 0;
    g_sel_end = 0;
}

static void sel_range(int* out_s, int* out_e) {
    if (g_sel_anchor < g_sel_end) { *out_s = g_sel_anchor; *out_e = g_sel_end; }
    else { *out_s = g_sel_end; *out_e = g_sel_anchor; }
}

static void start_selection() {
    if (!g_has_selection) {
        g_sel_anchor = g_cursor_pos;
        g_sel_end = g_cursor_pos;
        g_has_selection = true;
    }
}

static void update_selection() {
    g_sel_end = g_cursor_pos;
    if (g_sel_anchor == g_sel_end)
        g_has_selection = false;
}

static void delete_selection() {
    if (!g_has_selection) return;
    int ss, se;
    sel_range(&ss, &se);
    int count = se - ss;
    if (count <= 0) { clear_selection(); return; }
    delete_range(ss, count);
    g_cursor_pos = ss;
    g_modified = true;
    clear_selection();
    recompute_lines();
    update_cursor_pos();
}

// ============================================================================
// Scrolling
// ============================================================================

static void ensure_cursor_visible(int visible_lines, int text_area_w) {
    if (g_cursor_line < g_scroll_y)
        g_scroll_y = g_cursor_line;
    if (g_cursor_line >= g_scroll_y + visible_lines)
        g_scroll_y = g_cursor_line - visible_lines + 1;

    int max_scroll_y = g_line_count - visible_lines;
    if (max_scroll_y < 0) max_scroll_y = 0;
    if (g_scroll_y > max_scroll_y) g_scroll_y = max_scroll_y;
    if (g_scroll_y < 0) g_scroll_y = 0;

    int cell_w = font_cell_w();
    int cursor_px = g_cursor_col * cell_w;
    int view_w = text_area_w - LINE_NUM_W - SCROLLBAR_W;
    if (cursor_px - g_scroll_x > view_w - cell_w * 2)
        g_scroll_x = cursor_px - view_w + cell_w * 4;
    if (cursor_px < g_scroll_x) {
        g_scroll_x = cursor_px - cell_w * 2;
        if (g_scroll_x < 0) g_scroll_x = 0;
    }
}

// ============================================================================
// File I/O
// ============================================================================

static void set_status(const char* msg) {
    if (!msg) msg = "";
    montauk::strncpy(g_status_msg, msg, sizeof(g_status_msg) - 1);
    g_status_msg[sizeof(g_status_msg) - 1] = '\0';
}

static bool looks_binary(const char* data, int len) {
    if (!data || len <= 0) return false;

    int control_count = 0;
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == 0) return true;
        if (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t' && ch != '\f')
            control_count++;
    }

    return control_count * 20 > len;
}

static bool load_file(const char* path) {
    int fd = montauk::open(path);
    if (fd < 0) {
        set_status("Failed to open file");
        return false;
    }

    uint64_t size = montauk::getsize(fd);
    if (size > (uint64_t)MAX_CAP) {
        montauk::close(fd);
        set_status("File is too large for Text Editor");
        return false;
    }

    int read_len = (int)size;
    int temp_cap = read_len + 1;
    if (temp_cap < 1024) temp_cap = 1024;
    char* temp = (char*)montauk::malloc(temp_cap);
    if (!temp) {
        montauk::close(fd);
        set_status("Out of memory while opening file");
        return false;
    }

    int got = montauk::read(fd, (uint8_t*)temp, 0, size);
    montauk::close(fd);
    if (got < 0) {
        montauk::mfree(temp);
        set_status("Failed to read file");
        return false;
    }

    temp[got] = '\0';
    if (looks_binary(temp, got)) {
        montauk::mfree(temp);
        set_status("Binary files cannot be opened in Text Editor");
        return false;
    }

    if (got >= g_buf_cap) {
        int new_cap = got + 1024;
        if (new_cap > MAX_CAP) new_cap = MAX_CAP;
        g_buffer = (char*)montauk::realloc(g_buffer, new_cap);
        g_buf_cap = new_cap;
    }

    montauk::memcpy(g_buffer, temp, (uint64_t)(got + 1));
    montauk::mfree(temp);

    g_buf_len = got;
    g_cursor_pos = 0;
    g_scroll_y = 0;
    g_scroll_x = 0;
    g_modified = false;

    montauk::strncpy(g_filepath, path, 255);

    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;
    if (last_slash >= 0)
        montauk::strncpy(g_filename, path + last_slash + 1, 63);
    else
        montauk::strncpy(g_filename, path, 63);

    g_syntax_language = syn_detect_language(g_filepath);
    clear_selection();
    set_status("");

    recompute_lines();
    update_cursor_pos();
    return true;
}

static void save_file() {
    if (g_filepath[0] == '\0') return;

    int fd = montauk::fcreate(g_filepath);
    if (fd < 0) {
        set_status("Failed to save file");
        return;
    }

    montauk::fwrite(fd, (const uint8_t*)g_buffer, 0, g_buf_len);
    montauk::close(fd);

    g_modified = false;
    set_status("");
}

// ============================================================================
// Hit testing
// ============================================================================

static int hit_test(int mx, int my, int editor_y) {
    int cell_w = font_cell_w();
    int cell_h = font_cell_h();

    int clicked_line = g_scroll_y + (my - editor_y) / cell_h;
    if (clicked_line >= g_line_count) clicked_line = g_line_count - 1;
    if (clicked_line < 0) clicked_line = 0;

    int clicked_col = (mx - LINE_NUM_W - 4 + g_scroll_x + cell_w / 2) / cell_w;
    if (clicked_col < 0) clicked_col = 0;
    int ll = line_length(clicked_line);
    if (clicked_col > ll) clicked_col = ll;

    return g_line_offsets[clicked_line] + clicked_col;
}

// ============================================================================
// Rendering
// ============================================================================

static void render(uint32_t* pixels) {
    int W = g_win_w, H = g_win_h;
    int cell_w = font_cell_w();
    int cell_h = font_cell_h();

    // Clear
    px_fill(pixels, W, H, 0, 0, W, H, BG_COLOR);

    // ---- Toolbar ----
    px_fill(pixels, W, H, 0, 0, W, TOOLBAR_H, TOOLBAR_BG);

    // Open button
    px_fill_rounded(pixels, W, H, 4, 6, 24, 24, 3, BTN_BG);
    px_icon(pixels, W, H, 8, 10, g_icon_folder);

    // Save button
    px_fill_rounded(pixels, W, H, 32, 6, 24, 24, 3, BTN_BG);
    px_icon(pixels, W, H, 36, 10, g_icon_save);

    // Separator
    px_vline(pixels, W, H, 60, 4, 28, BORDER_COLOR);

    // Filename + modified
    char label[128];
    if (g_filename[0])
        snprintf(label, 128, "%s%s", g_filename, g_modified ? " [modified]" : "");
    else
        snprintf(label, 128, "Untitled%s", g_modified ? " [modified]" : "");
    if (g_font)
        g_font->draw_to_buffer(pixels, W, H, 68, (TOOLBAR_H - FONT_SIZE) / 2, label, TEXT_COLOR, FONT_SIZE);

    // Toolbar bottom line
    px_hline(pixels, W, H, 0, TOOLBAR_H - 1, W, BORDER_COLOR);

    // ---- Pathbar (conditional) ----
    int editor_y = TOOLBAR_H;

    if (g_pathbar_open) {
        int pb_y = TOOLBAR_H;
        px_fill(pixels, W, H, 0, pb_y, W, PATHBAR_H, PATHBAR_BG);

        int btn_w = 56;
        int inp_x = 8;
        int inp_y = pb_y + 4;
        int inp_w = W - inp_x - btn_w - 12;
        int inp_h = 24;

        // Input box
        px_fill(pixels, W, H, inp_x, inp_y, inp_w, inp_h, WHITE);
        // Input border (top, bottom, left, right)
        px_hline(pixels, W, H, inp_x, inp_y, inp_w, ACCENT);
        px_hline(pixels, W, H, inp_x, inp_y + inp_h - 1, inp_w, ACCENT);
        px_vline(pixels, W, H, inp_x, inp_y, inp_h, ACCENT);
        px_vline(pixels, W, H, inp_x + inp_w - 1, inp_y, inp_h, ACCENT);

        // Path text
        if (g_font && g_pathbar_text[0])
            g_font->draw_to_buffer(pixels, W, H, inp_x + 4, inp_y + (inp_h - FONT_SIZE) / 2,
                                   g_pathbar_text, TEXT_COLOR, FONT_SIZE);

        // Cursor in pathbar
        char prefix[256];
        int plen = g_pathbar_cursor > 255 ? 255 : g_pathbar_cursor;
        for (int i = 0; i < plen; i++) prefix[i] = g_pathbar_text[i];
        prefix[plen] = '\0';
        int cx = inp_x + 4 + (g_font ? g_font->measure_text(prefix, FONT_SIZE) : 0);
        px_fill(pixels, W, H, cx, inp_y + 3, 2, inp_h - 6, ACCENT);

        // Button
        int ob_x = inp_x + inp_w + 6;
        px_fill_rounded(pixels, W, H, ob_x, inp_y, btn_w, inp_h, 3, ACCENT);
        const char* btn_label = g_pathbar_save ? "Save" : "Open";
        if (g_font) {
            int tw = g_font->measure_text(btn_label, FONT_SIZE);
            g_font->draw_to_buffer(pixels, W, H, ob_x + (btn_w - tw) / 2,
                                   inp_y + (inp_h - FONT_SIZE) / 2, btn_label, WHITE, FONT_SIZE);
        }

        // Pathbar bottom line
        px_hline(pixels, W, H, 0, pb_y + PATHBAR_H - 1, W, BORDER_COLOR);

        editor_y = TOOLBAR_H + PATHBAR_H;
    }

    // ---- Editor area ----
    int text_area_h = H - editor_y - STATUS_H;
    int visible_lines = text_area_h / cell_h;
    if (visible_lines < 1) visible_lines = 1;
    int text_area_w = W - SCROLLBAR_W;

    if (g_cursor_moved) {
        ensure_cursor_visible(visible_lines, W);
        g_cursor_moved = false;
    }

    // Line number gutter
    px_fill(pixels, W, H, 0, editor_y, LINE_NUM_W, text_area_h, LINENUM_BG);
    px_vline(pixels, W, H, LINE_NUM_W, editor_y, text_area_h, BORDER_COLOR);

    // Selection range
    int sel_s = 0, sel_e = 0;
    if (g_has_selection) sel_range(&sel_s, &sel_e);

    int text_start_x = LINE_NUM_W + 4;

    // Syntax highlighting: compute multi-line syntax state up to the first visible line
    SynState syn_state = syn_make_state();
    SynToken syn_line_buf[SYN_LINE_BUF_CAP];
    if (g_syntax_language != SYN_LANG_NONE) {
        for (int i = 0; i < g_scroll_y && i < g_line_count; i++) {
            int ls = g_line_offsets[i];
            int ll = line_length(i);
            syn_highlight_line(g_buffer + ls, ll, nullptr, 0, g_syntax_language, syn_state);
        }
    }

    GlyphCache* gc = (g_font_mono && g_font_mono->valid) ? g_font_mono->get_cache(MONO_SIZE) : nullptr;

    for (int vis = 0; vis < visible_lines + 1; vis++) {
        int line = g_scroll_y + vis;
        if (line >= g_line_count) break;

        int py = editor_y + vis * cell_h;
        if (py >= editor_y + text_area_h) break;

        // Cursor line highlight
        if (line == g_cursor_line && !g_has_selection) {
            int hl_h = cell_h;
            if (py + hl_h > editor_y + text_area_h) hl_h = editor_y + text_area_h - py;
            if (hl_h > 0)
                px_fill(pixels, W, H, LINE_NUM_W + 1, py, text_area_w - LINE_NUM_W - 1, hl_h, CURSOR_LINE_BG);
        }

        // Line number
        char num_str[8];
        snprintf(num_str, 8, "%4d", line + 1);
        if (g_font_mono)
            g_font_mono->draw_to_buffer(pixels, W, H, 4, py, num_str, LINENUM_COLOR, MONO_SIZE);

        // Line text
        int line_start = g_line_offsets[line];
        int line_len = line_length(line);

        // Syntax highlight this line
        int syn_len = line_len > SYN_LINE_BUF_CAP ? SYN_LINE_BUF_CAP : line_len;
        if (g_syntax_language != SYN_LANG_NONE)
            syn_highlight_line(g_buffer + line_start, line_len, syn_line_buf,
                               SYN_LINE_BUF_CAP, g_syntax_language, syn_state);

        for (int ci = 0; ci < line_len; ci++) {
            int px_x = text_start_x + ci * cell_w - g_scroll_x;
            if (px_x + cell_w <= LINE_NUM_W + 1) continue;
            if (px_x >= text_area_w) break;

            int byte_pos = line_start + ci;
            char ch = g_buffer[byte_pos];

            // Selection highlight
            if (g_has_selection && byte_pos >= sel_s && byte_pos < sel_e) {
                int hl_h = cell_h;
                if (py + hl_h > editor_y + text_area_h) hl_h = editor_y + text_area_h - py;
                if (hl_h > 0)
                    px_fill(pixels, W, H, px_x, py, cell_w, hl_h, SEL_BG);
            }

            Color char_color = TEXT_COLOR;
            if (g_syntax_language != SYN_LANG_NONE && ci < syn_len)
                char_color = syn_color(syn_line_buf[ci]);

            if (ch >= 32 || ch < 0) {
                if (gc) {
                    g_font_mono->draw_char_to_buffer(pixels, W, H, px_x, py + gc->ascent,
                                                     ch, char_color, gc);
                }
            }
        }

        // Selection highlight for newline at end of line
        if (g_has_selection && line + 1 < g_line_count) {
            int nl_pos = line_start + line_len;
            if (nl_pos >= sel_s && nl_pos < sel_e) {
                int px_x = text_start_x + line_len * cell_w - g_scroll_x;
                if (px_x > LINE_NUM_W && px_x < text_area_w) {
                    int hl_h = cell_h;
                    if (py + hl_h > editor_y + text_area_h) hl_h = editor_y + text_area_h - py;
                    if (hl_h > 0)
                        px_fill(pixels, W, H, px_x, py, cell_w / 2, hl_h, SEL_BG);
                }
            }
        }

        // Cursor
        if (line == g_cursor_line) {
            int cx = text_start_x + g_cursor_col * cell_w - g_scroll_x;
            if (cx > LINE_NUM_W && cx + 2 <= text_area_w) {
                int cur_h = cell_h;
                if (py + cur_h > editor_y + text_area_h) cur_h = editor_y + text_area_h - py;
                if (cur_h > 0)
                    px_fill(pixels, W, H, cx, py, 2, cur_h, ACCENT);
            }
        }
    }

    // ---- Scrollbar ----
    int content_h = g_line_count * cell_h;
    if (content_h > text_area_h) {
        int sb_x = W - SCROLLBAR_W;
        px_fill(pixels, W, H, sb_x, editor_y, SCROLLBAR_W, text_area_h, Color::from_rgb(0xF0, 0xF0, 0xF0));
        int max_scr = g_line_count - visible_lines;
        if (max_scr < 1) max_scr = 1;
        int thumb_h = (text_area_h * text_area_h) / content_h;
        if (thumb_h < 20) thumb_h = 20;
        int range = text_area_h - thumb_h;
        int thumb_y = editor_y + (g_scroll_y * range) / max_scr;
        px_fill(pixels, W, H, sb_x + 1, thumb_y, SCROLLBAR_W - 2, thumb_h, Color::from_rgb(0xAA, 0xAA, 0xAA));
    }

    // ---- Status bar ----
    int status_y = H - STATUS_H;
    px_fill(pixels, W, H, 0, status_y, W, STATUS_H, STATUS_BG);

    int sty = status_y + (STATUS_H - FONT_SIZE) / 2;

    // Left: filename
    char status_left[128];
    if (g_status_msg[0]) {
        if (g_filename[0])
            snprintf(status_left, 128, " %s%s | %s", g_filename, g_modified ? " [modified]" : "", g_status_msg);
        else
            snprintf(status_left, 128, " Untitled%s | %s", g_modified ? " [modified]" : "", g_status_msg);
    } else if (g_filename[0]) {
        snprintf(status_left, 128, " %s%s", g_filename, g_modified ? " [modified]" : "");
    } else {
        snprintf(status_left, 128, " Untitled%s", g_modified ? " [modified]" : "");
    }
    if (g_font)
        g_font->draw_to_buffer(pixels, W, H, 4, sty, status_left, STATUS_TEXT, FONT_SIZE);

    // Right: cursor position
    char status_right[48];
    if (g_has_selection) {
        int ss, se;
        sel_range(&ss, &se);
        snprintf(status_right, 48, "%d selected  Ln %d, Col %d ", se - ss, g_cursor_line + 1, g_cursor_col + 1);
    } else {
        snprintf(status_right, 48, "Ln %d, Col %d ", g_cursor_line + 1, g_cursor_col + 1);
    }
    if (g_font) {
        int rw = g_font->measure_text(status_right, FONT_SIZE);
        g_font->draw_to_buffer(pixels, W, H, W - rw - 4, sty, status_right, STATUS_TEXT, FONT_SIZE);
    }
}

// ============================================================================
// Pathbar actions
// ============================================================================

static void pathbar_confirm(int win_id) {
    if (g_pathbar_text[0] == '\0') return;

    if (g_pathbar_save) {
        montauk::strncpy(g_filepath, g_pathbar_text, 255);
        const char* name = g_pathbar_text;
        for (int i = 0; g_pathbar_text[i]; i++)
            if (g_pathbar_text[i] == '/') name = g_pathbar_text + i + 1;
        montauk::strncpy(g_filename, name, 63);
        g_syntax_language = syn_detect_language(g_filepath);
        save_file();
    } else {
        load_file(g_pathbar_text);
    }

    g_pathbar_open = false;
}

static void open_inline_pathbar(bool save_mode, const char* initial_text) {
    g_pathbar_open = true;
    g_pathbar_save = save_mode;
    if (!initial_text) initial_text = "";
    montauk::strncpy(g_pathbar_text, initial_text, 255);
    g_pathbar_len = montauk::slen(g_pathbar_text);
    g_pathbar_cursor = g_pathbar_len;
}

static void open_pathbar_open() {
    char path[256] = {};
    char msg[128] = {};
    if (dialogs::open_file("Open File", g_filepath, path, sizeof(path), msg, sizeof(msg))) {
        load_file(path);
    } else if (msg[0]) {
        open_inline_pathbar(false, g_filepath);
    }
}

static void open_pathbar_save() {
    char path[256] = {};
    const char* initial_path = g_filepath[0] ? g_filepath : "";
    const char* suggested_name = g_filename[0] ? g_filename : "untitled.txt";
    char msg[128] = {};
    if (dialogs::save_file("Save File", initial_path, suggested_name, path, sizeof(path), msg, sizeof(msg))) {
        montauk::strncpy(g_filepath, path, 255);
        const char* name = path;
        for (int i = 0; path[i]; i++)
            if (path[i] == '/') name = path + i + 1;
        montauk::strncpy(g_filename, name, 63);
        g_syntax_language = syn_detect_language(g_filepath);
        save_file();
    } else if (msg[0]) {
        open_inline_pathbar(true, initial_path[0] ? initial_path : suggested_name);
    }
}

// ============================================================================
// Keyboard handler
// ============================================================================

static bool handle_key(Montauk::KeyEvent& key, int win_id) {
    if (!key.pressed) return false;

    // ---- Pathbar mode ----
    if (g_pathbar_open) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            pathbar_confirm(win_id);
            return true;
        }
        if (key.scancode == 0x01) { // Escape
            g_pathbar_open = false;
            return true;
        }
        if (key.ascii == '\b' || key.scancode == 0x0E) {
            if (g_pathbar_cursor > 0) {
                for (int i = g_pathbar_cursor - 1; i < g_pathbar_len - 1; i++)
                    g_pathbar_text[i] = g_pathbar_text[i + 1];
                g_pathbar_len--;
                g_pathbar_cursor--;
                g_pathbar_text[g_pathbar_len] = '\0';
            }
            return true;
        }
        if (key.scancode == 0x4B) { if (g_pathbar_cursor > 0) g_pathbar_cursor--; return true; }
        if (key.scancode == 0x4D) { if (g_pathbar_cursor < g_pathbar_len) g_pathbar_cursor++; return true; }
        if (key.ascii >= 32 && key.ascii < 127 && g_pathbar_len < 254) {
            for (int i = g_pathbar_len; i > g_pathbar_cursor; i--)
                g_pathbar_text[i] = g_pathbar_text[i - 1];
            g_pathbar_text[g_pathbar_cursor] = key.ascii;
            g_pathbar_cursor++;
            g_pathbar_len++;
            g_pathbar_text[g_pathbar_len] = '\0';
            return true;
        }
        return false;
    }

    // ---- Editor mode ----

    // Ctrl+S: save
    if (key.ctrl && (key.ascii == 's' || key.ascii == 'S')) {
        if (g_filepath[0] == '\0')
            open_pathbar_save();
        else
            save_file();
        return true;
    }

    // Ctrl+O: open
    if (key.ctrl && (key.ascii == 'o' || key.ascii == 'O')) {
        if (g_pathbar_open) g_pathbar_open = false;
        else open_pathbar_open();
        return true;
    }

    // Ctrl+A: select all
    if (key.ctrl && (key.ascii == 'a' || key.ascii == 'A')) {
        g_sel_anchor = 0;
        g_sel_end = g_buf_len;
        g_has_selection = (g_buf_len > 0);
        g_cursor_pos = g_buf_len;
        update_cursor_pos();
        return true;
    }

    // Arrow keys
    if (key.scancode == 0x48) { // Up
        if (key.shift) start_selection(); else if (g_has_selection) clear_selection();
        move_up();
        if (key.shift) update_selection();
        return true;
    }
    if (key.scancode == 0x50) { // Down
        if (key.shift) start_selection(); else if (g_has_selection) clear_selection();
        move_down();
        if (key.shift) update_selection();
        return true;
    }
    if (key.scancode == 0x4B) { // Left
        if (key.shift) {
            start_selection(); move_left(); update_selection();
        } else if (g_has_selection) {
            int s, e; sel_range(&s, &e);
            g_cursor_pos = s; update_cursor_pos(); clear_selection();
        } else move_left();
        return true;
    }
    if (key.scancode == 0x4D) { // Right
        if (key.shift) {
            start_selection(); move_right(); update_selection();
        } else if (g_has_selection) {
            int s, e; sel_range(&s, &e);
            g_cursor_pos = e; update_cursor_pos(); clear_selection();
        } else move_right();
        return true;
    }

    // Home
    if (key.scancode == 0x47) {
        if (key.shift) start_selection(); else if (g_has_selection) clear_selection();
        move_home();
        if (key.shift) update_selection();
        return true;
    }
    // End
    if (key.scancode == 0x4F) {
        if (key.shift) start_selection(); else if (g_has_selection) clear_selection();
        move_end();
        if (key.shift) update_selection();
        return true;
    }

    // Page Up
    if (key.scancode == 0x49) {
        if (key.shift) start_selection(); else if (g_has_selection) clear_selection();
        int cell_h = font_cell_h();
        int vis = cell_h > 0 ? (g_win_h - TOOLBAR_H - STATUS_H) / cell_h : 20;
        for (int i = 0; i < vis; i++) move_up();
        if (key.shift) update_selection();
        return true;
    }
    // Page Down
    if (key.scancode == 0x51) {
        if (key.shift) start_selection(); else if (g_has_selection) clear_selection();
        int cell_h = font_cell_h();
        int vis = cell_h > 0 ? (g_win_h - TOOLBAR_H - STATUS_H) / cell_h : 20;
        for (int i = 0; i < vis; i++) move_down();
        if (key.shift) update_selection();
        return true;
    }

    // Delete
    if (key.scancode == 0x53) {
        if (g_has_selection) delete_selection(); else delete_char();
        return true;
    }

    // Backspace
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        if (g_has_selection) delete_selection(); else backspace();
        return true;
    }

    // Enter
    if (key.ascii == '\n' || key.ascii == '\r') {
        if (g_has_selection) delete_selection();
        insert_char('\n');
        return true;
    }

    // Tab
    if (key.ascii == '\t') {
        if (g_has_selection) delete_selection();
        for (int i = 0; i < TAB_WIDTH; i++) insert_char(' ');
        return true;
    }

    // Printable
    if (key.ascii >= 32 && key.ascii < 127) {
        if (g_has_selection) delete_selection();
        insert_char(key.ascii);
        return true;
    }

    return false;
}

// ============================================================================
// Mouse handler
// ============================================================================

static bool handle_mouse(Montauk::WinEvent& ev, int win_id) {
    int mx = ev.mouse.x;
    int my = ev.mouse.y;
    uint8_t btns = ev.mouse.buttons;
    uint8_t prev = ev.mouse.prev_buttons;
    bool clicked = (btns & 1) && !(prev & 1);
    bool held = (btns & 1) != 0;
    bool released = !(btns & 1) && (prev & 1);
    int scroll = ev.mouse.scroll;
    int cell_h = font_cell_h();

    int editor_y = TOOLBAR_H + (g_pathbar_open ? PATHBAR_H : 0);
    int text_area_h = g_win_h - editor_y - STATUS_H;

    // ---- Toolbar clicks ----
    if (clicked && my < TOOLBAR_H) {
        // Open button
        if (mx >= 4 && mx < 28 && my >= 6 && my < 30) {
            if (g_pathbar_open) g_pathbar_open = false;
            else open_pathbar_open();
            return true;
        }
        // Save button
        if (mx >= 32 && mx < 56 && my >= 6 && my < 30) {
            if (g_filepath[0] == '\0')
                open_pathbar_save();
            else
                save_file();
            return true;
        }
        return false;
    }

    // ---- Pathbar clicks ----
    if (g_pathbar_open && my >= TOOLBAR_H && my < TOOLBAR_H + PATHBAR_H) {
        if (clicked) {
            int btn_w = 56;
            int inp_w = g_win_w - 8 - btn_w - 12;
            int ob_x = 8 + inp_w + 6;
            if (mx >= ob_x && mx < ob_x + btn_w)
                pathbar_confirm(win_id);
        }
        return true;
    }

    // ---- Editor click ----
    if (clicked && my >= editor_y && my < editor_y + text_area_h
        && mx > LINE_NUM_W && mx < g_win_w - SCROLLBAR_W) {
        int pos = hit_test(mx, my, editor_y);
        g_cursor_pos = pos;
        update_cursor_pos();
        g_sel_anchor = pos;
        g_sel_end = pos;
        g_has_selection = false;
        g_mouse_selecting = true;
        return true;
    }

    // ---- Editor drag ----
    if (g_mouse_selecting && held && my >= editor_y - 20) {
        int pos = hit_test(mx, my, editor_y);
        g_sel_end = pos;
        g_cursor_pos = pos;
        update_cursor_pos();
        g_has_selection = (g_sel_anchor != g_sel_end);
        return true;
    }

    // ---- Mouse release ----
    if (g_mouse_selecting && released) {
        g_mouse_selecting = false;
        return true;
    }

    // ---- Scroll ----
    if (scroll != 0 && my >= editor_y && my < editor_y + text_area_h) {
        int old_scroll = g_scroll_y;
        g_scroll_y -= scroll * 3;
        if (g_scroll_y < 0) g_scroll_y = 0;
        int visible_lines = text_area_h / cell_h;
        int max_scroll = g_line_count - visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;

        // Move cursor with scroll to keep it visible
        int delta = g_scroll_y - old_scroll;
        if (delta != 0) {
            int new_line = g_cursor_line + delta;
            if (new_line < g_scroll_y) new_line = g_scroll_y;
            if (new_line >= g_scroll_y + visible_lines) new_line = g_scroll_y + visible_lines - 1;
            if (new_line < 0) new_line = 0;
            if (new_line >= g_line_count) new_line = g_line_count - 1;
            int col = g_cursor_col;
            int ll = line_length(new_line);
            if (col > ll) col = ll;
            g_cursor_pos = g_line_offsets[new_line] + col;
            g_cursor_line = new_line;
            g_cursor_col = col;
        }
        return true;
    }

    return false;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Initialize buffer
    g_buffer = (char*)montauk::malloc(INIT_CAP);
    g_buf_cap = INIT_CAP;
    g_buf_len = 0;

    recompute_lines();
    update_cursor_pos();

    // Load fonts
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_font = load_font("0:/fonts/Roboto-Medium.ttf");
    g_font_mono = load_font("0:/fonts/JetBrainsMono-Regular.ttf");

    // Load toolbar icons
    Color icon_color = Color::from_rgb(0x44, 0x44, 0x44);
    g_icon_folder = svg_load("0:/icons/folder.svg", 16, 16, icon_color);
    g_icon_save = svg_load("0:/icons/document-save-symbolic.svg", 16, 16, icon_color);

    // Check for file argument
    char args[512] = {};
    int arglen = montauk::getargs(args, sizeof(args));
    if (arglen > 0 && args[0])
        load_file(args);

    // Build window title
    char title[64] = "Text Editor";
    if (g_filename[0]) {
        snprintf(title, 64, "%s - Editor", g_filename);
    }

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create(title, INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    render(pixels);
    montauk::win_present(win_id);

    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;
        if (r == 0) { montauk::sleep_ms(16); continue; }

        // Close
        if (ev.type == 3) break;

        // Resize
        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        bool redraw = false;

        // Keyboard
        if (ev.type == 0)
            redraw = handle_key(ev.key, win_id);

        // Mouse
        if (ev.type == 1)
            redraw = handle_mouse(ev, win_id);

        if (redraw) {
            render(pixels);
            montauk::win_present(win_id);
        }
    }

    // Cleanup
    if (g_buffer) montauk::mfree(g_buffer);
    if (g_line_offsets) montauk::mfree(g_line_offsets);
    if (g_font) montauk::mfree(g_font);
    if (g_font_mono) montauk::mfree(g_font_mono);
    montauk::win_destroy(win_id);
    montauk::exit(0);
}
