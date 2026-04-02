/*
 * wordprocessor.hpp
 * Shared declarations for the Word Processor app
 * Copyright (c) 2026 Daniel Hammer
 */

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/standalone.hpp>
#include <gui/truetype.hpp>
#include <gui/svg.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
}

using namespace gui;

static constexpr int INIT_W            = 900;
static constexpr int INIT_H            = 600;
static constexpr int WP_TOOLBAR_H      = 36;
static constexpr int WP_PATHBAR_H      = 32;
static constexpr int WP_STATUS_H       = 24;
static constexpr int WP_SCROLLBAR_W    = 12;
static constexpr int WP_MARGIN         = 16;
static constexpr int WP_MAX_RUNS       = 1024;
static constexpr int WP_MAX_TEXT       = 262144;
static constexpr int WP_MAX_WRAP_LINES = 4096;
static constexpr int WP_MAX_PARAGRAPHS = 4096;
static constexpr int WP_DEFAULT_SIZE   = 18;
static constexpr int WP_UNDO_MAX       = 24;
static constexpr int WP_SCREEN_DPI     = 96;
static constexpr int WP_PRINT_DPI      = 150;
static constexpr int WP_PARA_STEP      = 12;
static constexpr int WP_SPACE_STEP     = 6;
static constexpr int WP_LIST_LEFT      = 28;
static constexpr int WP_LIST_HANGING   = -18;
static constexpr int WP_LIST_MARKER_W  = 20;
static constexpr int WP_BTN_OPEN_X     = 4;
static constexpr int WP_BTN_SAVE_X     = 32;
static constexpr int WP_BTN_PRINT_X    = 60;
static constexpr int WP_BTN_UNDO_X     = 94;
static constexpr int WP_BTN_REDO_X     = 122;
static constexpr int WP_BTN_BOLD_X     = 156;
static constexpr int WP_BTN_ITALIC_X   = 184;
static constexpr int WP_FONT_DD_X      = 216;
static constexpr int WP_FONT_DD_W      = 74;
static constexpr int WP_SIZE_DD_X      = 296;
static constexpr int WP_SIZE_DD_W      = 36;
static constexpr int WP_BTN_ALIGN_L_X  = 344;
static constexpr int WP_BTN_ALIGN_C_X  = 372;
static constexpr int WP_BTN_ALIGN_R_X  = 400;
static constexpr int WP_BTN_BULLET_X   = 432;
static constexpr int WP_BTN_NUMBER_X   = 460;
static constexpr int WP_BTN_OUTDENT_X  = 496;
static constexpr int WP_BTN_INDENT_X   = 524;
static constexpr int WP_LINE_DD_X      = 558;
static constexpr int WP_LINE_DD_W      = 48;
static constexpr int WP_BTN_DIVIDER_X  = 614;
static constexpr int WP_BTN_SECTION_X  = 646;
static constexpr int WP_DIVIDER_ROW_H  = 24;
static constexpr int WP_DIVIDER_FLYOUT_W = 216;
static constexpr int WP_SPECIAL_CHAR_ROW_H = 24;
static constexpr int WP_SPECIAL_CHAR_FLYOUT_W = 144;

static constexpr int FONT_ROBOTO    = 0;
static constexpr int FONT_NOTOSERIF = 1;
static constexpr int FONT_C059      = 2;
static constexpr int FONT_COUNT     = 3;

static constexpr uint8_t STYLE_BOLD   = 0x01;
static constexpr uint8_t STYLE_ITALIC = 0x02;

inline constexpr const char* WP_FONT_NAMES[FONT_COUNT] = {
    "Roboto",
    "NotoSerif",
    "C059",
};

inline constexpr int WP_SIZE_OPTIONS[] = { 12, 14, 16, 18, 20, 24, 28, 36 };
static constexpr int WP_SIZE_OPTION_COUNT = 8;
inline constexpr int WP_LINE_SPACING_OPTIONS[] = { 100, 125, 150, 200 };
static constexpr int WP_LINE_SPACING_OPTION_COUNT = 4;

inline int wp_points_to_pixels(int points, int dpi) {
    if (points <= 0) points = 1;
    if (dpi <= 0) dpi = WP_SCREEN_DPI;
    int px = (points * dpi + 36) / 72;
    return px > 0 ? px : 1;
}

inline int wp_screen_font_pixels(int points) {
    return wp_points_to_pixels(points, WP_SCREEN_DPI);
}

inline int wp_print_font_pixels(int points) {
    return wp_points_to_pixels(points, WP_PRINT_DPI);
}

inline int wp_scale_layout_units(int units, int dpi) {
    if (dpi <= 0) dpi = WP_SCREEN_DPI;
    int scaled = (units * dpi + (units >= 0 ? WP_SCREEN_DPI / 2 : -WP_SCREEN_DPI / 2)) / WP_SCREEN_DPI;
    return scaled;
}

struct WpSpecialCharOption {
    uint8_t code;
    const char* label;
};

struct WpDividerOption {
    uint8_t type;
    const char* label;
};

inline constexpr WpSpecialCharOption WP_SPECIAL_CHAR_OPTIONS[] = {
    { 0xA7, "Section" },
    { 0xB6, "Pilcrow" },
    { 0x96, "En Dash" },
    { 0x97, "Em Dash" },
    { 0xA9, "Copyright" },
    { 0xAE, "Registered" },
    { 0xB0, "Degree" },
    { 0xB1, "Plus/Minus" },
};
static constexpr int WP_SPECIAL_CHAR_OPTION_COUNT = 8;

enum ParagraphAlign : uint8_t {
    PARA_ALIGN_LEFT = 0,
    PARA_ALIGN_CENTER = 1,
    PARA_ALIGN_RIGHT = 2,
};

enum ParagraphListType : uint8_t {
    PARA_LIST_NONE = 0,
    PARA_LIST_BULLET = 1,
    PARA_LIST_NUMBER = 2,
};

enum ParagraphDividerType : uint8_t {
    PARA_DIVIDER_NONE = 0,
    PARA_DIVIDER_SINGLE = 1,
    PARA_DIVIDER_DOUBLE = 2,
    PARA_DIVIDER_DOTTED = 3,
    PARA_DIVIDER_DASHED = 4,
    PARA_DIVIDER_HEAVY = 5,
    PARA_DIVIDER_THIN_THICK = 6,
    PARA_DIVIDER_THICK_THIN = 7,
};

inline constexpr WpDividerOption WP_DIVIDER_OPTIONS[] = {
    { PARA_DIVIDER_SINGLE, "Single Line" },
    { PARA_DIVIDER_DOUBLE, "Double Line" },
    { PARA_DIVIDER_DOTTED, "Dotted Line" },
    { PARA_DIVIDER_DASHED, "Dashed Line" },
    { PARA_DIVIDER_HEAVY, "Heavy Line" },
    { PARA_DIVIDER_THIN_THICK, "Thin/Thick Rule" },
    { PARA_DIVIDER_THICK_THIN, "Thick/Thin Rule" },
};
static constexpr int WP_DIVIDER_OPTION_COUNT = 7;

struct WPFontTable {
    TrueTypeFont* fonts[FONT_COUNT][4];
    bool loaded;
};

struct StyledRun {
    char* text;
    int len;
    int cap;
    uint8_t font_id;
    uint8_t size;
    uint8_t flags;
};

struct WrapLine {
    int run_idx;
    int run_offset;
    int char_count;
    int y;
    int height;
    int baseline;
    int x;
    int width;
    int paragraph_idx;
    int list_number;
    uint8_t divider_type;
    bool first_in_paragraph;
};

struct ParagraphStyle {
    uint8_t align;
    uint8_t list_type;
    uint8_t line_spacing;
    uint8_t divider_type;
    int16_t left_indent;
    int16_t first_line_indent;
    int16_t space_before;
    int16_t space_after;
};

struct UndoSnapshot {
    uint8_t* data;
    int size;
    int cursor_abs;
    int sel_anchor;
    int sel_end;
    bool has_selection;
    bool modified;
};

inline bool wp_left_held(uint8_t buttons) {
    return (buttons & 0x01) != 0;
}

inline bool wp_left_pressed(uint8_t buttons, uint8_t prev_buttons) {
    return (buttons & 0x01) && !(prev_buttons & 0x01);
}

inline bool wp_left_released(uint8_t buttons, uint8_t prev_buttons) {
    return !(buttons & 0x01) && (prev_buttons & 0x01);
}

struct WpScrollbar {
    Rect bounds;
    int content_height;
    int view_height;
    int scroll_offset;
    bool dragging;
    int drag_start_y;
    int drag_start_offset;
    Color bg;
    Color fg;
    Color hover_fg;
    bool hovered;

    void init(int x, int y, int w, int h) {
        bounds = {x, y, w, h};
        content_height = 0;
        view_height = h;
        scroll_offset = 0;
        dragging = false;
        drag_start_y = 0;
        drag_start_offset = 0;
        bg = colors::SCROLLBAR_BG;
        fg = colors::SCROLLBAR_FG;
        hover_fg = Color::from_rgb(0xA0, 0xA0, 0xA0);
        hovered = false;
    }

    int thumb_height() const {
        if (content_height <= view_height) return bounds.h;
        int th = (view_height * bounds.h) / content_height;
        return th < 20 ? 20 : th;
    }

    int thumb_y() const {
        if (content_height <= view_height) return bounds.y;
        int range = bounds.h - thumb_height();
        int max_scroll = content_height - view_height;
        if (max_scroll <= 0) return bounds.y;
        return bounds.y + (scroll_offset * range) / max_scroll;
    }

    int max_scroll() const {
        int ms = content_height - view_height;
        return ms > 0 ? ms : 0;
    }

    void handle_mouse(int x, int y, uint8_t buttons, uint8_t prev_buttons, int scroll) {
        if (content_height <= view_height) return;

        Rect thumb_rect = {bounds.x, thumb_y(), bounds.w, thumb_height()};
        hovered = thumb_rect.contains(x, y);

        if (hovered && wp_left_pressed(buttons, prev_buttons)) {
            dragging = true;
            drag_start_y = y;
            drag_start_offset = scroll_offset;
        }

        if (dragging && wp_left_held(buttons)) {
            int dy = y - drag_start_y;
            int range = bounds.h - thumb_height();
            if (range > 0) {
                int ms = max_scroll();
                scroll_offset = drag_start_offset + (dy * ms) / range;
                if (scroll_offset < 0) scroll_offset = 0;
                if (scroll_offset > ms) scroll_offset = ms;
            }
        }

        if (!wp_left_held(buttons)) {
            dragging = false;
        }

        if (bounds.contains(x, y) && scroll != 0) {
            scroll_offset += scroll * 20;
            int ms = max_scroll();
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > ms) scroll_offset = ms;
        }
    }
};

struct WordProcessorState {
    StyledRun runs[WP_MAX_RUNS];
    int run_count;
    int total_text_len;

    int cursor_run;
    int cursor_offset;

    int sel_anchor;
    int sel_end;
    bool has_selection;
    bool mouse_selecting;

    uint8_t cur_font_id;
    uint8_t cur_size;
    uint8_t cur_flags;

    WpScrollbar scrollbar;
    int content_height;

    WrapLine* wrap_lines;
    int wrap_line_count;
    int wrap_line_cap;
    bool wrap_dirty;
    int last_wrap_width;
    int last_wrap_dpi;

    ParagraphStyle paragraphs[WP_MAX_PARAGRAPHS];
    int paragraph_count;

    bool modified;
    char filepath[256];
    char filename[64];
    char preferred_printer_uri[256];
    char preferred_printer_name[128];
    uint32_t preferred_print_copies;
    char status_msg[160];

    bool show_pathbar;
    bool pathbar_save_mode;
    char pathbar_text[256];
    int pathbar_cursor;
    int pathbar_len;

    bool font_dropdown_open;
    bool size_dropdown_open;
    bool line_spacing_dropdown_open;
    bool divider_flyout_open;
    bool special_char_flyout_open;

    UndoSnapshot undo[WP_UNDO_MAX];
    int undo_count;
    int undo_pos;
};

extern int g_win_w;
extern int g_win_h;
extern WsWindow g_win;
extern WordProcessorState g_wp;
extern WPFontTable g_wp_fonts;
extern SvgIcon g_icon_folder;
extern SvgIcon g_icon_save;
extern SvgIcon g_icon_print;
extern SvgIcon g_icon_undo;
extern SvgIcon g_icon_redo;
extern SvgIcon g_icon_align_left;
extern SvgIcon g_icon_align_center;
extern SvgIcon g_icon_align_right;
extern SvgIcon g_icon_list_bullet;
extern SvgIcon g_icon_list_number;
extern SvgIcon g_icon_indent_less;
extern SvgIcon g_icon_indent_more;
extern TrueTypeFont* g_ui_font;
extern TrueTypeFont* g_ui_bold;

void wp_load_fonts();
void wp_load_icons();
void wp_init_paragraph_style(ParagraphStyle* para);
void wp_init_empty_document(WordProcessorState* wp);
void wp_free_document(WordProcessorState* wp);
void wp_cleanup_state();

TrueTypeFont* wp_get_font(int font_id, uint8_t flags);

int  wp_abs_pos(WordProcessorState* wp, int run, int offset);
void wp_pos_to_run(WordProcessorState* wp, int abs_pos, int* out_run, int* out_offset);
char wp_char_at(WordProcessorState* wp, int abs_pos);
int  wp_find_paragraph_at(WordProcessorState* wp, int abs_pos);
void wp_selected_paragraph_range(WordProcessorState* wp, int* out_start_para, int* out_end_para);

void wp_insert_char(WordProcessorState* wp, char c);
void wp_delete_char(WordProcessorState* wp);
void wp_backspace(WordProcessorState* wp);

void wp_cursor_left(WordProcessorState* wp);
void wp_cursor_right(WordProcessorState* wp);
void wp_cursor_up(WordProcessorState* wp);
void wp_cursor_down(WordProcessorState* wp);

void wp_clear_selection(WordProcessorState* wp);
void wp_sel_range(WordProcessorState* wp, int* out_start, int* out_end);
void wp_start_selection(WordProcessorState* wp);
void wp_update_selection_to_cursor(WordProcessorState* wp);
void wp_apply_style_to_selection(WordProcessorState* wp, int mode, int value);
void wp_apply_alignment(WordProcessorState* wp, uint8_t align);
void wp_adjust_paragraph_indent(WordProcessorState* wp, int delta);
void wp_adjust_paragraph_first_line_indent(WordProcessorState* wp, int delta);
void wp_adjust_paragraph_spacing_before(WordProcessorState* wp, int delta);
void wp_adjust_paragraph_spacing_after(WordProcessorState* wp, int delta);
void wp_cycle_line_spacing(WordProcessorState* wp);
void wp_set_line_spacing(WordProcessorState* wp, int value);
void wp_toggle_list(WordProcessorState* wp, uint8_t list_type);
void wp_insert_divider(WordProcessorState* wp, uint8_t divider_type);
void wp_delete_selection(WordProcessorState* wp);

void wp_recompute_wrap(WordProcessorState* wp, int content_w, int layout_dpi);
int  wp_find_wrap_line(WordProcessorState* wp, int abs_pos);
int  wp_wrap_line_start(WordProcessorState* wp, int line_idx);
void wp_ensure_cursor_visible(WordProcessorState* wp, int view_h);

void wp_set_filepath(WordProcessorState* wp, const char* path);
void wp_start_pathbar(WordProcessorState* wp, bool save_mode, const char* initial_text);
void wp_open_save_pathbar(WordProcessorState* wp);
void wp_save_file(WordProcessorState* wp);
void wp_load_file(WordProcessorState* wp, const char* path);

void wp_history_reset(WordProcessorState* wp);
void wp_history_checkpoint(WordProcessorState* wp);
void wp_history_mark_saved(WordProcessorState* wp);
bool wp_undo(WordProcessorState* wp);
bool wp_redo(WordProcessorState* wp);

bool wp_print_document(WordProcessorState* wp, char* out_status, int out_status_len);

void wp_render();
void wp_handle_mouse(const Montauk::WinEvent& ev);
void wp_handle_key(const Montauk::KeyEvent& key);
