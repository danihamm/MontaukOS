/*
 * main.cpp
 * MontaukOS Videos app -- AVI playback with PCM audio
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/config.h>
#include <gui/gui.hpp>
#include <gui/standalone.hpp>
#include <gui/truetype.hpp>
#include <gui/svg.hpp>

extern "C" {
#include <stdio.h>
#include <string.h>
}

#include "avi.hpp"

using namespace gui;

static constexpr int INIT_W = 1040;
static constexpr int INIT_H = 720;

static constexpr int PROGRESS_H = 40;
static constexpr int TRANSPORT_H = 72;
static constexpr int TOOL_ICON_SIZE = 16;

static constexpr int FONT_SIZE = 18;
static constexpr int FONT_SIZE_SM = 16;
static constexpr int FONT_SIZE_LG = 22;

static constexpr int MAX_FILES = 128;
static constexpr int MAX_PATH = 256;
static constexpr int MAX_FILTER = 64;
static constexpr uint64_t MAX_FILE_SIZE = 1024ull * 1024ull * 1024ull;
static constexpr int AUDIO_OUT_CHANNELS = 2;
static constexpr int AUDIO_OUT_BITS = 16;
static constexpr int AUDIO_RING_BYTES = 0x8000;
static constexpr int AUDIO_CHUNK_BYTES = 4096;

static constexpr Color BG_TOP = Color::from_rgb(0x07, 0x09, 0x0C);
static constexpr Color BG_BOTTOM = Color::from_rgb(0x12, 0x15, 0x1A);
static constexpr Color TOOLBAR_BG = Color::from_rgb(0x10, 0x13, 0x18);
static constexpr Color PANEL_BG = Color::from_rgb(0x14, 0x18, 0x1F);
static constexpr Color PANEL_BG_ALT = Color::from_rgb(0x0E, 0x12, 0x17);
static constexpr Color VIDEO_BG = Color::from_rgb(0x03, 0x04, 0x06);
static constexpr Color BORDER = Color::from_rgb(0x2A, 0x31, 0x3A);
static constexpr Color TEXT = Color::from_rgb(0xF4, 0xF0, 0xE8);
static constexpr Color TEXT_DIM = Color::from_rgb(0xA2, 0xA8, 0xB3);
static constexpr Color TEXT_MUTED = Color::from_rgb(0x73, 0x7A, 0x84);
static constexpr Color ACCENT = Color::from_rgb(0xD9, 0x4F, 0x32);
static constexpr Color ACCENT_DARK = Color::from_rgb(0xA9, 0x35, 0x20);
static constexpr Color ACCENT_SOFT = Color::from_rgb(0x3A, 0x1C, 0x18);
static constexpr Color TRACK_BG = Color::from_rgb(0x29, 0x31, 0x3B);
static constexpr Color BTN_BG = Color::from_rgb(0x1C, 0x23, 0x2C);
static constexpr Color BTN_ACTIVE = Color::from_rgb(0xD9, 0x4F, 0x32);
static constexpr Color BTN_STOP = Color::from_rgb(0xC9, 0x3A, 0x31);
static constexpr Color WHITE = Color::from_rgb(0xFF, 0xFF, 0xFF);

enum class PlayState {
    Stopped,
    Playing,
    Paused,
};

struct VideoEntry {
    char name[128];
};

struct AppState {
    int win_w;
    int win_h;

    TrueTypeFont* font;

    char dir_path[MAX_PATH];
    VideoEntry files[MAX_FILES];
    int file_count;

    int scroll_y;
    int hovered_item;
    int selected_item;
    int filtered_indices[MAX_FILES];
    int filtered_count;
    char filter_text[MAX_FILTER];
    bool filter_active;

    bool loop_enabled;

    int current_video;
    avi::File movie;
    uint32_t* frame_pixels;
    int displayed_frame_index;
    char status_text[128];

    PlayState play_state;
    uint64_t base_time_us;
    uint64_t play_start_ms;

    int audio_handle;
    uint64_t audio_hw_total_bytes;
    uint64_t audio_output_written_bytes;
    uint64_t audio_output_total_bytes;
    int audio_hw_prev_pos;
    bool audio_hw_valid;

    int audio_chunk_index;
    uint32_t audio_chunk_pos;
    uint8_t audio_buf[AUDIO_CHUNK_BYTES];
    int audio_buf_len;
    int audio_buf_pos;

    bool dragging_progress;
    uint64_t drag_time_us;

    SvgIcon ico_rewind;
    SvgIcon ico_play;
    SvgIcon ico_pause;
    SvgIcon ico_stop;
    SvgIcon ico_forward;
    SvgIcon ico_play_w;
    SvgIcon ico_pause_w;
    SvgIcon ico_stop_w;
    SvgIcon ico_repeat;
    SvgIcon ico_repeat_w;
};

static AppState g = {};

static void transport_layout(int* out_x, int* out_y, int* out_w, int* out_h, int* out_gap);

static int text_w(const char* text, int size = FONT_SIZE) {
    return (g.font && text) ? g.font->measure_text(text, size) : 0;
}

static int font_h(int size = FONT_SIZE) {
    if (!g.font) return size;
    GlyphCache* cache = g.font->get_cache(size);
    return cache ? (cache->ascent - cache->descent) : size;
}

static void px_fill(uint32_t* px, int bw, int bh, int x, int y, int w, int h, Color c) {
    if (!px || w <= 0 || h <= 0) return;
    int x0 = gui_max(0, x);
    int y0 = gui_max(0, y);
    int x1 = gui_min(bw, x + w);
    int y1 = gui_min(bh, y + h);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t v = c.to_pixel();
    for (int row = y0; row < y1; row++) {
        for (int col = x0; col < x1; col++) {
            px[row * bw + col] = v;
        }
    }
}

static void px_vgradient(uint32_t* px, int bw, int bh, int x, int y, int w, int h,
                         Color top, Color bottom) {
    if (!px || w <= 0 || h <= 0) return;
    int x0 = gui_max(0, x);
    int y0 = gui_max(0, y);
    int x1 = gui_min(bw, x + w);
    int y1 = gui_min(bh, y + h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int row = y0; row < y1; row++) {
        int t = (h <= 1) ? 255 : ((row - y) * 255) / (h - 1);
        int inv = 255 - t;
        Color c = {
            (uint8_t)((top.r * inv + bottom.r * t + 127) / 255),
            (uint8_t)((top.g * inv + bottom.g * t + 127) / 255),
            (uint8_t)((top.b * inv + bottom.b * t + 127) / 255),
            255
        };
        uint32_t v = c.to_pixel();
        for (int col = x0; col < x1; col++) {
            px[row * bw + col] = v;
        }
    }
}

static void px_hline(uint32_t* px, int bw, int bh, int x, int y, int w, Color c) {
    if (!px || y < 0 || y >= bh || w <= 0) return;
    int x0 = gui_max(0, x);
    int x1 = gui_min(bw, x + w);
    uint32_t v = c.to_pixel();
    for (int col = x0; col < x1; col++) {
        px[y * bw + col] = v;
    }
}

static void px_vline(uint32_t* px, int bw, int bh, int x, int y, int h, Color c) {
    if (!px || x < 0 || x >= bw || h <= 0) return;
    int y0 = gui_max(0, y);
    int y1 = gui_min(bh, y + h);
    uint32_t v = c.to_pixel();
    for (int row = y0; row < y1; row++) {
        px[row * bw + x] = v;
    }
}

static void px_rect(uint32_t* px, int bw, int bh, int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    px_hline(px, bw, bh, x, y, w, c);
    px_hline(px, bw, bh, x, y + h - 1, w, c);
    px_vline(px, bw, bh, x, y, h, c);
    px_vline(px, bw, bh, x + w - 1, y, h, c);
}

static void px_fill_rounded(uint32_t* px, int bw, int bh,
                            int x, int y, int w, int h, int r, Color c) {
    if (!px || w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    int max_r = gui_min(w / 2, h / 2);
    if (r > max_r) r = max_r;
    uint32_t v = c.to_pixel();

    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= bh) continue;
        for (int col = 0; col < w; col++) {
            int px_x = x + col;
            if (px_x < 0 || px_x >= bw) continue;

            bool skip = false;
            int cx = 0, cy = 0;
            if      (col < r      && row < r)      { cx = r - col - 1; cy = r - row - 1; skip = (cx * cx + cy * cy >= r * r); }
            else if (col >= w - r && row < r)      { cx = col - (w - r); cy = r - row - 1; skip = (cx * cx + cy * cy >= r * r); }
            else if (col < r      && row >= h - r) { cx = r - col - 1; cy = row - (h - r); skip = (cx * cx + cy * cy >= r * r); }
            else if (col >= w - r && row >= h - r) { cx = col - (w - r); cy = row - (h - r); skip = (cx * cx + cy * cy >= r * r); }

            if (!skip) px[py * bw + px_x] = v;
        }
    }
}

static void px_circle(uint32_t* px, int bw, int bh, int cx, int cy, int r, Color c) {
    if (!px || r <= 0) return;
    uint32_t v = c.to_pixel();
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= bh) continue;
        for (int dx = -r; dx <= r; dx++) {
            int px_x = cx + dx;
            if (px_x < 0 || px_x >= bw) continue;
            if (dx * dx + dy * dy <= r * r)
                px[py * bw + px_x] = v;
        }
    }
}

static void px_icon(uint32_t* px, int bw, int bh, int x, int y, const SvgIcon& icon) {
    if (!px || !icon.pixels) return;
    for (int row = 0; row < icon.height; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        for (int col = 0; col < icon.width; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            uint32_t src = icon.pixels[row * icon.width + col];
            uint8_t sa = (src >> 24) & 0xFF;
            if (sa == 0) continue;
            if (sa == 255) {
                px[dy * bw + dx] = src;
            } else {
                uint32_t dst = px[dy * bw + dx];
                uint32_t inv = 255 - sa;
                uint32_t r = (sa * ((src >> 16) & 0xFF) + inv * ((dst >> 16) & 0xFF) + 127) / 255;
                uint32_t g0 = (sa * ((src >> 8) & 0xFF) + inv * ((dst >> 8) & 0xFF) + 127) / 255;
                uint32_t b = (sa * (src & 0xFF) + inv * (dst & 0xFF) + 127) / 255;
                px[dy * bw + dx] = 0xFF000000u | (r << 16) | (g0 << 8) | b;
            }
        }
    }
}

static void px_text(uint32_t* px, int bw, int bh, int x, int y,
                    const char* text, Color c, int size = FONT_SIZE) {
    if (g.font && text && text[0])
        g.font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}

static void copy_text_fit(char* out, int out_cap, const char* text, int max_w, int size) {
    if (!out || out_cap <= 0) return;
    if (!text) {
        out[0] = '\0';
        return;
    }

    montauk::strncpy(out, text, out_cap);
    if (!g.font || text_w(out, size) <= max_w) return;

    int len = montauk::slen(out);
    const char* ellipsis = "...";
    while (len > 0) {
        len--;
        out[len] = '\0';
        char trial[160];
        montauk::strncpy(trial, out, (int)sizeof(trial));
        int tlen = montauk::slen(trial);
        if (tlen < (int)sizeof(trial) - 4) {
            trial[tlen + 0] = '.';
            trial[tlen + 1] = '.';
            trial[tlen + 2] = '.';
            trial[tlen + 3] = '\0';
        }
        if (text_w(trial, size) <= max_w || len == 0) {
            montauk::strncpy(out, trial, out_cap);
            return;
        }
    }

    montauk::strncpy(out, ellipsis, out_cap);
}

static void px_text_fit(uint32_t* px, int bw, int bh, int x, int y, int max_w,
                        const char* text, Color c, int size = FONT_SIZE) {
    char fitted[160];
    copy_text_fit(fitted, (int)sizeof(fitted), text, max_w, size);
    px_text(px, bw, bh, x, y, fitted, c, size);
}

static void px_icon_button(uint32_t* px, int bw, int bh, int x, int y, int w, int h,
                           const SvgIcon& icon, Color bg, int r) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    px_icon(px, bw, bh, x + (w - icon.width) / 2, y + (h - icon.height) / 2, icon);
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool is_printable_ascii(char c) {
    return c >= 32 && c < 127;
}

static bool str_ends_with(const char* s, const char* suffix) {
    int sl = montauk::slen(s);
    int xl = montauk::slen(suffix);
    if (xl > sl) return false;
    for (int i = 0; i < xl; i++) {
        if (ascii_lower(s[sl - xl + i]) != ascii_lower(suffix[i]))
            return false;
    }
    return true;
}

static bool str_eq_case_insensitive(const char* a, const char* b) {
    int i = 0;
    while (a[i] || b[i]) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
        i++;
    }
    return true;
}

static bool str_contains_case_insensitive(const char* text, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!text || !text[0]) return false;

    int tlen = montauk::slen(text);
    int nlen = montauk::slen(needle);
    if (nlen > tlen) return false;

    for (int i = 0; i <= tlen - nlen; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            if (ascii_lower(text[i + j]) != ascii_lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }

    return false;
}

static void set_status(const char* text) {
    montauk::strncpy(g.status_text, text ? text : "", (int)sizeof(g.status_text));
}

static uint64_t media_duration_us() {
    return g.movie.valid ? avi::duration_us(g.movie) : 0;
}

static Rect content_rect() {
    int bottom = g.win_h - PROGRESS_H - TRANSPORT_H;
    if (bottom < 160) bottom = 160;
    return {0, 0, g.win_w, bottom};
}

static Rect video_rect() {
    return content_rect();
}

static Rect repeat_button_rect() {
    int bx, by, bw, bh, gap;
    transport_layout(&bx, &by, &bw, &bh, &gap);
    (void)bx;
    (void)gap;
    return {g.win_w - bw - 24, by, bw, bh};
}

static Rect progress_rect() {
    int y = g.win_h - TRANSPORT_H - PROGRESS_H;
    return {16, y + 28, g.win_w - 32, 6};
}

static void transport_layout(int* out_x, int* out_y, int* out_w, int* out_h, int* out_gap) {
    int btn_w = 48;
    int btn_h = 32;
    int gap = 10;
    int total_w = btn_w * 4 + gap * 3;
    int x = (g.win_w - total_w) / 2;
    int y = g.win_h - TRANSPORT_H + (TRANSPORT_H - btn_h) / 2;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = btn_w;
    if (out_h) *out_h = btn_h;
    if (out_gap) *out_gap = gap;
}

static int visible_items() { return 0; }

static int filtered_raw_at(int visible_index) {
    if (visible_index < 0 || visible_index >= g.filtered_count) return -1;
    return g.filtered_indices[visible_index];
}

static bool filtered_contains(int raw_index) {
    if (raw_index < 0) return false;
    for (int i = 0; i < g.filtered_count; i++) {
        if (g.filtered_indices[i] == raw_index) return true;
    }
    return false;
}

static int selected_visible_index() {
    for (int i = 0; i < g.filtered_count; i++) {
        if (g.filtered_indices[i] == g.selected_item) return i;
    }
    return -1;
}

static void clamp_list_scroll() {
    int max_scroll = g.filtered_count;
    if (max_scroll < 0) max_scroll = 0;
    if (g.scroll_y < 0) g.scroll_y = 0;
    if (g.scroll_y > max_scroll) g.scroll_y = max_scroll;
}

static void ensure_selected_visible() {
    int sel = selected_visible_index();
    if (sel < 0) {
        g.scroll_y = 0;
        return;
    }
    g.scroll_y = sel;
}

static void rebuild_filtered_view() {
    int prev_selected = g.selected_item;
    g.filtered_count = 0;

    for (int i = 0; i < g.file_count && g.filtered_count < MAX_FILES; i++) {
        if (str_contains_case_insensitive(g.files[i].name, g.filter_text))
            g.filtered_indices[g.filtered_count++] = i;
    }

    if (!filtered_contains(g.hovered_item)) g.hovered_item = -1;

    if (g.filtered_count == 0) {
        g.selected_item = -1;
        g.scroll_y = 0;
        return;
    }

    if (filtered_contains(prev_selected)) g.selected_item = prev_selected;
    else if (filtered_contains(g.current_video)) g.selected_item = g.current_video;
    else g.selected_item = g.filtered_indices[0];

    clamp_list_scroll();
    ensure_selected_visible();
}

static void append_filter_char(char c) {
    int len = montauk::slen(g.filter_text);
    if (len >= MAX_FILTER - 1) return;
    g.filter_text[len] = c;
    g.filter_text[len + 1] = '\0';
    rebuild_filtered_view();
}

static void pop_filter_char() {
    int len = montauk::slen(g.filter_text);
    if (len <= 0) return;
    g.filter_text[len - 1] = '\0';
    rebuild_filtered_view();
}

static void clear_filter() {
    if (!g.filter_text[0]) return;
    g.filter_text[0] = '\0';
    rebuild_filtered_view();
}

static void move_selection(int delta) {
    if (g.filtered_count <= 0) {
        g.selected_item = -1;
        g.scroll_y = 0;
        return;
    }

    int sel = selected_visible_index();
    if (sel < 0) sel = (delta >= 0) ? 0 : (g.filtered_count - 1);
    else {
        sel += delta;
        if (sel < 0) sel = 0;
        if (sel >= g.filtered_count) sel = g.filtered_count - 1;
    }

    g.selected_item = g.filtered_indices[sel];
    ensure_selected_visible();
}

static void scan_directory() {
    g.file_count = 0;

    const char* names[256];
    int count = montauk::readdir(g.dir_path, names, 256);
    if (count < 0) {
        rebuild_filtered_view();
        return;
    }

    for (int i = 0; i < count && g.file_count < MAX_FILES; i++) {
        if (!str_ends_with(names[i], ".avi")) continue;
        montauk::strncpy(g.files[g.file_count].name, names[i], (int)sizeof(g.files[g.file_count].name));
        g.file_count++;
    }

    rebuild_filtered_view();
}

static void close_audio_session() {
    if (g.audio_handle >= 0) {
        montauk::audio_close(g.audio_handle);
        g.audio_handle = -1;
    }
    g.audio_hw_total_bytes = 0;
    g.audio_output_written_bytes = 0;
    g.audio_output_total_bytes = 0;
    g.audio_hw_prev_pos = 0;
    g.audio_hw_valid = false;
    g.audio_chunk_index = 0;
    g.audio_chunk_pos = 0;
    g.audio_buf_len = 0;
    g.audio_buf_pos = 0;
}

static void unload_movie() {
    close_audio_session();
    avi::reset(g.movie);
    if (g.frame_pixels) {
        montauk::mfree(g.frame_pixels);
        g.frame_pixels = nullptr;
    }
    g.current_video = -1;
    g.displayed_frame_index = -1;
    g.base_time_us = 0;
    g.play_start_ms = 0;
    g.play_state = PlayState::Stopped;
    g.dragging_progress = false;
}

static void format_time(char* buf, int buf_size, uint64_t time_us) {
    uint64_t secs = time_us / 1000000ull;
    int h = (int)(secs / 3600ull);
    int m = (int)((secs / 60ull) % 60ull);
    int s = (int)(secs % 60ull);
    if (h > 0) snprintf(buf, buf_size, "%d:%02d:%02d", h, m, s);
    else snprintf(buf, buf_size, "%d:%02d", m, s);
}

static void format_fps(char* buf, int buf_size) {
    if (!g.movie.valid) {
        snprintf(buf, buf_size, "-- fps");
        return;
    }

    if (g.movie.stream_rate > 0 && g.movie.stream_scale > 0) {
        uint64_t fps10 = ((uint64_t)g.movie.stream_rate * 10ull + (uint64_t)g.movie.stream_scale / 2ull) /
                         (uint64_t)g.movie.stream_scale;
        if ((fps10 % 10ull) == 0) snprintf(buf, buf_size, "%llu fps", (unsigned long long)(fps10 / 10ull));
        else snprintf(buf, buf_size, "%llu.%llu fps",
                      (unsigned long long)(fps10 / 10ull),
                      (unsigned long long)(fps10 % 10ull));
        return;
    }

    if (g.movie.microsec_per_frame > 0) {
        uint64_t fps10 = (10000000ull + (uint64_t)g.movie.microsec_per_frame / 2ull) /
                         (uint64_t)g.movie.microsec_per_frame;
        if ((fps10 % 10ull) == 0) snprintf(buf, buf_size, "%llu fps", (unsigned long long)(fps10 / 10ull));
        else snprintf(buf, buf_size, "%llu.%llu fps",
                      (unsigned long long)(fps10 / 10ull),
                      (unsigned long long)(fps10 % 10ull));
        return;
    }

    snprintf(buf, buf_size, "-- fps");
}

static bool movie_has_supported_audio() {
    return avi::has_supported_audio(g.movie);
}

static void update_audio_hw_position() {
    if (g.audio_handle < 0) return;
    int hw = montauk::audio_get_pos(g.audio_handle);
    if (hw < 0) return;

    int pos = hw % AUDIO_RING_BYTES;
    if (!g.audio_hw_valid) {
        g.audio_hw_prev_pos = pos;
        g.audio_hw_valid = true;
        return;
    }

    int delta = pos - g.audio_hw_prev_pos;
    if (delta < 0) delta += AUDIO_RING_BYTES;
    g.audio_hw_total_bytes += (uint64_t)delta;
    if (g.audio_hw_total_bytes > g.audio_output_written_bytes)
        g.audio_hw_total_bytes = g.audio_output_written_bytes;
    g.audio_hw_prev_pos = pos;
}

static uint64_t current_time_us() {
    uint64_t total = media_duration_us();
    if (!g.movie.valid) return 0;
    if (g.dragging_progress) return g.drag_time_us > total ? total : g.drag_time_us;

    if (g.play_state == PlayState::Playing) {
        if (g.audio_handle >= 0) {
            update_audio_hw_position();
            uint64_t bytes_per_sec = (uint64_t)g.movie.audio_rate * 4ull;
            if (bytes_per_sec > 0) {
                uint64_t delta_us = (g.audio_hw_total_bytes * 1000000ull) / bytes_per_sec;
                uint64_t t = g.base_time_us + delta_us;
                return t > total ? total : t;
            }
        } else {
            uint64_t elapsed_us = (montauk::get_milliseconds() - g.play_start_ms) * 1000ull;
            uint64_t t = g.base_time_us + elapsed_us;
            return t > total ? total : t;
        }
    }

    return g.base_time_us > total ? total : g.base_time_us;
}

static bool decode_frame_index(int index) {
    if (!g.movie.valid || !g.frame_pixels) return false;
    if (index < 0 || index >= g.movie.video_chunk_count) return false;
    if (index == g.displayed_frame_index) return true;
    if (!avi::decode_frame(g.movie, index, g.frame_pixels)) {
        set_status("Frame decode failed");
        return false;
    }
    g.displayed_frame_index = index;
    return true;
}

static void sync_frame_to_time(uint64_t time_us) {
    if (!g.movie.valid) return;
    int index = avi::frame_index_for_time(g.movie, time_us);
    if (index >= 0) decode_frame_index(index);
}

static void seek_audio_to_time(uint64_t time_us) {
    g.audio_chunk_index = 0;
    g.audio_chunk_pos = 0;
    g.audio_buf_len = 0;
    g.audio_buf_pos = 0;

    if (!movie_has_supported_audio()) {
        g.audio_output_total_bytes = 0;
        return;
    }

    uint64_t target_frames = (time_us * (uint64_t)g.movie.audio_rate) / 1000000ull;
    if (target_frames > g.movie.total_audio_frames)
        target_frames = g.movie.total_audio_frames;

    uint64_t remaining_frames = g.movie.total_audio_frames - target_frames;
    g.audio_output_total_bytes = remaining_frames * 4ull;

    uint64_t skip_frames = target_frames;
    for (int i = 0; i < g.movie.audio_chunk_count; i++) {
        uint64_t chunk_frames = g.movie.audio_chunks[i].size / g.movie.audio_block_align;
        if (skip_frames >= chunk_frames) {
            skip_frames -= chunk_frames;
            continue;
        }

        g.audio_chunk_index = i;
        g.audio_chunk_pos = (uint32_t)(skip_frames * (uint64_t)g.movie.audio_block_align);
        return;
    }

    g.audio_chunk_index = g.movie.audio_chunk_count;
    g.audio_chunk_pos = 0;
}

static void fill_audio_buffer() {
    g.audio_buf_pos = 0;
    g.audio_buf_len = 0;

    if (!movie_has_supported_audio()) return;

    uint8_t src_buf[AUDIO_CHUNK_BYTES];
    while (g.audio_buf_len + 4 <= AUDIO_CHUNK_BYTES && g.audio_chunk_index < g.movie.audio_chunk_count) {
        const avi::ChunkRef& chunk = g.movie.audio_chunks[g.audio_chunk_index];
        if (g.audio_chunk_pos >= chunk.size) {
            g.audio_chunk_index++;
            g.audio_chunk_pos = 0;
            continue;
        }

        if (chunk.size - g.audio_chunk_pos < g.movie.audio_block_align) {
            g.audio_chunk_index++;
            g.audio_chunk_pos = 0;
            continue;
        }

        uint32_t out_space = (uint32_t)(AUDIO_CHUNK_BYTES - g.audio_buf_len);
        uint32_t max_frames = out_space / 4u;
        uint32_t src_space = chunk.size - g.audio_chunk_pos;
        src_space -= src_space % g.movie.audio_block_align;
        uint32_t read_size = max_frames * (uint32_t)g.movie.audio_block_align;
        if (read_size > src_space) read_size = src_space;
        if (read_size > AUDIO_CHUNK_BYTES) {
            read_size = AUDIO_CHUNK_BYTES;
            read_size -= read_size % g.movie.audio_block_align;
        }
        if (read_size < g.movie.audio_block_align) return;

        if (!avi::read_bytes(g.movie, (uint64_t)chunk.offset + (uint64_t)g.audio_chunk_pos, src_buf, read_size)) {
            set_status("Audio read failed");
            g.audio_chunk_index = g.movie.audio_chunk_count;
            g.audio_chunk_pos = 0;
            return;
        }

        uint32_t frames = read_size / g.movie.audio_block_align;
        for (uint32_t i = 0; i < frames; i++) {
            const uint8_t* src = src_buf + (uint64_t)i * (uint64_t)g.movie.audio_block_align;
            int16_t left = 0;
            int16_t right = 0;

            if (g.movie.audio_bits == 8) {
                int s0 = ((int)src[0] - 128) << 8;
                left = (int16_t)s0;
                if (g.movie.audio_channels >= 2) {
                    int s1 = ((int)src[1] - 128) << 8;
                    right = (int16_t)s1;
                } else {
                    right = left;
                }
            } else {
                left = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                if (g.movie.audio_channels >= 2) {
                    right = (int16_t)((uint16_t)src[2] | ((uint16_t)src[3] << 8));
                } else {
                    right = left;
                }
            }

            g.audio_buf[g.audio_buf_len + 0] = (uint8_t)(left & 0xFF);
            g.audio_buf[g.audio_buf_len + 1] = (uint8_t)((left >> 8) & 0xFF);
            g.audio_buf[g.audio_buf_len + 2] = (uint8_t)(right & 0xFF);
            g.audio_buf[g.audio_buf_len + 3] = (uint8_t)((right >> 8) & 0xFF);
            g.audio_buf_len += 4;
        }

        g.audio_chunk_pos += read_size;
    }
}

static void feed_audio() {
    if (g.play_state != PlayState::Playing || g.audio_handle < 0) return;
    update_audio_hw_position();

    for (int iter = 0; iter < 4; iter++) {
        if (g.audio_buf_len > 0) {
            int written = montauk::audio_write(g.audio_handle, g.audio_buf + g.audio_buf_pos, g.audio_buf_len);
            if (written <= 0) return;
            g.audio_buf_pos += written;
            g.audio_buf_len -= written;
            g.audio_output_written_bytes += (uint64_t)written;
            continue;
        }

        fill_audio_buffer();
        if (g.audio_buf_len <= 0) return;
    }
}

static bool begin_playback_from_time(uint64_t time_us) {
    if (!g.movie.valid) return false;

    uint64_t duration = media_duration_us();
    if (duration == 0) time_us = 0;
    else if (time_us > duration) time_us = duration;

    close_audio_session();
    g.base_time_us = time_us;
    g.play_state = PlayState::Playing;
    sync_frame_to_time(time_us);

    if (movie_has_supported_audio()) {
        seek_audio_to_time(time_us);
        g.audio_handle = montauk::audio_open(g.movie.audio_rate, AUDIO_OUT_CHANNELS, AUDIO_OUT_BITS);
        if (g.audio_handle >= 0) {
            g.audio_hw_prev_pos = montauk::audio_get_pos(g.audio_handle);
            if (g.audio_hw_prev_pos < 0) g.audio_hw_prev_pos = 0;
            g.audio_hw_prev_pos %= AUDIO_RING_BYTES;
            g.audio_hw_valid = true;
        } else {
            set_status("Audio device unavailable; continuing without sound");
            g.play_start_ms = montauk::get_milliseconds();
        }
    } else {
        g.play_start_ms = montauk::get_milliseconds();
    }

    return true;
}

static void pause_playback() {
    if (g.play_state != PlayState::Playing || !g.movie.valid) return;
    g.base_time_us = current_time_us();
    close_audio_session();
    g.play_state = PlayState::Paused;
    sync_frame_to_time(g.base_time_us);
}

static void stop_playback(bool keep_last_frame) {
    if (!g.movie.valid) {
        close_audio_session();
        g.play_state = PlayState::Stopped;
        g.base_time_us = 0;
        return;
    }

    close_audio_session();
    g.play_state = PlayState::Stopped;
    g.base_time_us = keep_last_frame ? media_duration_us() : 0;
    sync_frame_to_time(g.base_time_us);
}

static void seek_to_time(uint64_t time_us) {
    if (!g.movie.valid) return;
    uint64_t duration = media_duration_us();
    if (duration > 0 && time_us > duration) time_us = duration;

    if (g.play_state == PlayState::Playing) {
        begin_playback_from_time(time_us);
    } else {
        close_audio_session();
        g.base_time_us = time_us;
        sync_frame_to_time(time_us);
    }
}

static bool load_video(int index) {
    if (index < 0 || index >= g.file_count) return false;

    unload_movie();

    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s/%s", g.dir_path, g.files[index].name);

    int fd = montauk::open(path);
    if (fd < 0) {
        set_status("Could not open AVI file");
        return false;
    }

    uint64_t file_size = montauk::getsize(fd);
    if (file_size == 0 || file_size > MAX_FILE_SIZE) {
        montauk::close(fd);
        set_status("AVI file is empty or too large");
        return false;
    }

    if (!avi::parse(g.movie, fd, file_size)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "AVI parse failed: %s", g.movie.error[0] ? g.movie.error : "unsupported file");
        unload_movie();
        set_status(msg);
        return false;
    }

    uint64_t pixels_size = (uint64_t)g.movie.width * (uint64_t)g.movie.height * 4ull;
    g.frame_pixels = (uint32_t*)montauk::malloc(pixels_size);
    if (!g.frame_pixels) {
        unload_movie();
        set_status("Out of memory for video frame buffer");
        return false;
    }
    montauk::memset(g.frame_pixels, 0, pixels_size);

    g.current_video = index;
    g.selected_item = index;
    g.displayed_frame_index = -1;
    g.base_time_us = 0;
    g.play_state = PlayState::Stopped;
    sync_frame_to_time(0);

    if (movie_has_supported_audio()) {
        char fps[32];
        format_fps(fps, sizeof(fps));
        snprintf(g.status_text, sizeof(g.status_text), "%s  %dx%d  %s  PCM %u Hz",
                 avi::codec_name(g.movie), g.movie.width, g.movie.height, fps, g.movie.audio_rate);
    } else if (g.movie.audio_chunk_count > 0) {
        char fps[32];
        format_fps(fps, sizeof(fps));
        snprintf(g.status_text, sizeof(g.status_text), "%s  %dx%d  %s  audio unsupported",
                 avi::codec_name(g.movie), g.movie.width, g.movie.height, fps);
    } else {
        char fps[32];
        format_fps(fps, sizeof(fps));
        snprintf(g.status_text, sizeof(g.status_text), "%s  %dx%d  %s  silent",
                 avi::codec_name(g.movie), g.movie.width, g.movie.height, fps);
    }

    return true;
}

static bool start_video(int index) {
    if (g.current_video != index || !g.movie.valid) {
        if (!load_video(index)) return false;
    }
    return begin_playback_from_time(0);
}

static bool play_selected_or_current() {
    if (g.selected_item >= 0 && filtered_contains(g.selected_item)) {
        if (g.current_video == g.selected_item && g.movie.valid) {
            uint64_t duration = media_duration_us();
            uint64_t start = g.base_time_us;
            if (duration > 0 && start >= duration) start = 0;
            return begin_playback_from_time(start);
        }
        return start_video(g.selected_item);
    }

    if (g.current_video >= 0 && g.movie.valid) {
        uint64_t duration = media_duration_us();
        uint64_t start = g.base_time_us;
        if (duration > 0 && start >= duration) start = 0;
        return begin_playback_from_time(start);
    }

    if (g.filtered_count > 0) return start_video(g.filtered_indices[0]);
    return false;
}

static bool next_video(bool auto_advanced = false) {
    if (g.file_count == 0) return false;
    if (g.current_video < 0) return start_video(0);

    if (auto_advanced && g.loop_enabled)
        return begin_playback_from_time(0);

    int next = g.current_video + 1;
    if (next >= g.file_count) {
        if (g.loop_enabled) next = 0;
        else {
            stop_playback(true);
            return false;
        }
    }
    return start_video(next);
}

static void prev_video() {
    if (g.file_count == 0) return;
    if (g.current_video >= 0 && current_time_us() > 3000000ull) {
        begin_playback_from_time(0);
        return;
    }

    int prev = g.current_video - 1;
    if (prev < 0) prev = g.file_count - 1;
    start_video(prev);
}

static uint64_t progress_time_from_x(int mx) {
    Rect bar = progress_rect();
    if (bar.w <= 0) return 0;
    int rel = mx - bar.x;
    if (rel < 0) rel = 0;
    if (rel > bar.w) rel = bar.w;
    uint64_t duration = media_duration_us();
    return (duration == 0) ? 0 : ((uint64_t)rel * duration) / (uint64_t)bar.w;
}

static void blit_scaled_argb(uint32_t* dst, int dw, int dh, const Rect& rect,
                             const uint32_t* src, int sw, int sh) {
    if (!dst || !src || rect.w <= 0 || rect.h <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < rect.h; y++) {
        int dy = rect.y + y;
        if (dy < 0 || dy >= dh) continue;
        int sy = (int)((uint64_t)y * (uint64_t)sh / (uint64_t)rect.h);
        const uint32_t* src_row = src + (uint64_t)sy * (uint64_t)sw;
        for (int x = 0; x < rect.w; x++) {
            int dx = rect.x + x;
            if (dx < 0 || dx >= dw) continue;
            int sx = (int)((uint64_t)x * (uint64_t)sw / (uint64_t)rect.w);
            dst[dy * dw + dx] = src_row[sx];
        }
    }
}

static void render_video_panel(uint32_t* pixels, const Rect& panel) {
    Rect inner = panel;
    px_fill(pixels, g.win_w, g.win_h, inner.x, inner.y, inner.w, inner.h, VIDEO_BG);

    if (g.movie.valid && g.frame_pixels) {
        int avail_w = inner.w;
        int avail_h = inner.h;
        int draw_w = avail_w;
        int draw_h = (int)((uint64_t)avail_w * (uint64_t)g.movie.height / (uint64_t)g.movie.width);
        if (draw_h > avail_h) {
            draw_h = avail_h;
            draw_w = (int)((uint64_t)avail_h * (uint64_t)g.movie.width / (uint64_t)g.movie.height);
        }
        if (draw_w < 1) draw_w = 1;
        if (draw_h < 1) draw_h = 1;

        Rect dst = {
            inner.x + (inner.w - draw_w) / 2,
            inner.y + (inner.h - draw_h) / 2,
            draw_w,
            draw_h
        };

        px_fill(pixels, g.win_w, g.win_h, inner.x, inner.y, inner.w, inner.h, VIDEO_BG);
        blit_scaled_argb(pixels, g.win_w, g.win_h, dst, g.frame_pixels, g.movie.width, g.movie.height);
    } else {
        const char* line1 = "Open an AVI from Files";
        const char* line2 = "Videos supports MJPEG and uncompressed RGB AVI files";
        int y = inner.y + inner.h / 2 - font_h(FONT_SIZE_LG);
        px_text(pixels, g.win_w, g.win_h, inner.x + (inner.w - text_w(line1, FONT_SIZE_LG)) / 2,
                y, line1, TEXT, FONT_SIZE_LG);
        px_text(pixels, g.win_w, g.win_h, inner.x + (inner.w - text_w(line2, FONT_SIZE_SM)) / 2,
                y + font_h(FONT_SIZE_LG) + 8, line2, TEXT_DIM, FONT_SIZE_SM);
    }

    px_hline(pixels, g.win_w, g.win_h, panel.x, panel.y + panel.h - 1, panel.w, BORDER);
}

static void render_progress(uint32_t* pixels) {
    int y = g.win_h - TRANSPORT_H - PROGRESS_H;
    px_fill(pixels, g.win_w, g.win_h, 0, y, g.win_w, PROGRESS_H, PANEL_BG_ALT);

    Rect bar = progress_rect();
    px_fill_rounded(pixels, g.win_w, g.win_h, bar.x, bar.y, bar.w, bar.h, 3, TRACK_BG);

    uint64_t current = current_time_us();
    uint64_t total = media_duration_us();
    if (total > 0) {
        int fill = (int)((current * (uint64_t)bar.w) / total);
        if (fill > bar.w) fill = bar.w;
        if (fill > 0)
            px_fill_rounded(pixels, g.win_w, g.win_h, bar.x, bar.y, fill, bar.h, 3, ACCENT);
        px_circle(pixels, g.win_w, g.win_h, bar.x + fill, bar.y + bar.h / 2, 5, ACCENT);
        px_circle(pixels, g.win_w, g.win_h, bar.x + fill, bar.y + bar.h / 2, 2, WHITE);
    }

    char cur_str[24];
    char total_str[24];
    format_time(cur_str, sizeof(cur_str), current);
    format_time(total_str, sizeof(total_str), total);
    px_text(pixels, g.win_w, g.win_h, 18, y + 4, cur_str, TEXT_DIM, FONT_SIZE_SM);
    int tw0 = text_w(total_str, FONT_SIZE_SM);
    px_text(pixels, g.win_w, g.win_h, g.win_w - tw0 - 18, y + 4, total_str, TEXT_DIM, FONT_SIZE_SM);
}

static void render_transport(uint32_t* pixels) {
    int y = g.win_h - TRANSPORT_H;
    px_fill(pixels, g.win_w, g.win_h, 0, y, g.win_w, TRANSPORT_H, BG_BOTTOM);

    int bx, by, bw, bh, gap;
    transport_layout(&bx, &by, &bw, &bh, &gap);

    px_icon_button(pixels, g.win_w, g.win_h, bx, by, bw, bh, g.ico_rewind, BTN_BG, 8);
    bx += bw + gap;

    {
        bool active = (g.play_state == PlayState::Playing);
        px_icon_button(pixels, g.win_w, g.win_h, bx, by, bw, bh,
                       active ? g.ico_pause_w : g.ico_play,
                       active ? BTN_ACTIVE : BTN_BG, 8);
    }
    bx += bw + gap;

    {
        bool active = (g.current_video >= 0 && g.movie.valid);
        px_icon_button(pixels, g.win_w, g.win_h, bx, by, bw, bh,
                       active ? g.ico_stop_w : g.ico_stop,
                       active ? BTN_STOP : BTN_BG, 8);
    }
    bx += bw + gap;

    px_icon_button(pixels, g.win_w, g.win_h, bx, by, bw, bh, g.ico_forward, BTN_BG, 8);

    Rect repeat = repeat_button_rect();
    px_icon_button(pixels, g.win_w, g.win_h, repeat.x, repeat.y, repeat.w, repeat.h,
                   g.loop_enabled ? g.ico_repeat_w : g.ico_repeat,
                   g.loop_enabled ? ACCENT : BTN_BG, 8);
}

static void render(uint32_t* pixels) {
    px_vgradient(pixels, g.win_w, g.win_h, 0, 0, g.win_w, g.win_h, BG_TOP, BG_BOTTOM);

    render_video_panel(pixels, video_rect());
    render_progress(pixels);
    render_transport(pixels);
}

static bool tick_playback() {
    if (g.play_state != PlayState::Playing || !g.movie.valid) return false;

    feed_audio();
    uint64_t now_ms = montauk::get_milliseconds();
    uint64_t t = current_time_us();
    uint64_t total = media_duration_us();
    sync_frame_to_time(t);

    if (g.audio_handle >= 0 &&
        g.audio_output_total_bytes > 0 &&
        g.audio_hw_total_bytes >= g.audio_output_total_bytes) {
        if (total > 0 && t < total) {
            close_audio_session();
            g.base_time_us = t;
            g.play_start_ms = now_ms;
        } else {
            next_video(true);
            return true;
        }
    }

    if (total > 0 && current_time_us() >= total) {
        next_video(true);
        return true;
    }

    return false;
}

static bool handle_click(int mx, int my) {
    if (repeat_button_rect().contains(mx, my)) {
        g.loop_enabled = !g.loop_enabled;
        return true;
    }

    Rect bar = progress_rect();
    if (my >= bar.y - 8 && my < bar.y + bar.h + 8 && g.movie.valid) {
        g.dragging_progress = true;
        g.drag_time_us = progress_time_from_x(mx);
        sync_frame_to_time(g.drag_time_us);
        return true;
    }

    int bx, by, bw, bh, gap;
    transport_layout(&bx, &by, &bw, &bh, &gap);
    Rect prev = {bx, by, bw, bh};
    Rect play = {bx + bw + gap, by, bw, bh};
    Rect stop = {bx + (bw + gap) * 2, by, bw, bh};
    Rect next = {bx + (bw + gap) * 3, by, bw, bh};

    if (prev.contains(mx, my)) {
        prev_video();
        return true;
    }
    if (play.contains(mx, my)) {
        if (g.play_state == PlayState::Playing) pause_playback();
        else play_selected_or_current();
        return true;
    }
    if (stop.contains(mx, my)) {
        stop_playback(false);
        return true;
    }
    if (next.contains(mx, my)) {
        next_video(false);
        return true;
    }

    return false;
}

static bool handle_key(const Montauk::KeyEvent& key, bool& quit) {
    if (!key.pressed) return false;

    if (key.scancode == 0x01) {
        quit = true;
        return false;
    }

    if (key.ascii == ' ') {
        if (g.play_state == PlayState::Playing) pause_playback();
        else play_selected_or_current();
        return true;
    }
    if (key.ascii == 's' || key.ascii == 'S') {
        stop_playback(false);
        return true;
    }
    if (key.ascii == 'l' || key.ascii == 'L') {
        g.loop_enabled = !g.loop_enabled;
        return true;
    }
    if (key.ascii == 'n' || key.ascii == 'N') {
        next_video(false);
        return true;
    }
    if (key.ascii == 'p' || key.ascii == 'P') {
        prev_video();
        return true;
    }

    if (key.scancode == 0x4B) {
        uint64_t cur = current_time_us();
        if (cur > 5000000ull) cur -= 5000000ull;
        else cur = 0;
        seek_to_time(cur);
        return true;
    }
    if (key.scancode == 0x4D) {
        seek_to_time(current_time_us() + 5000000ull);
        return true;
    }

    return false;
}

static void set_default_dir() {
    auto doc = montauk::config::load("session");
    const char* name = doc.get_string("session.username", "");
    if (name[0]) {
        snprintf(g.dir_path, sizeof(g.dir_path), "0:/users/%s/Videos", name);
        const char* probe[1];
        int ok = montauk::readdir(g.dir_path, probe, 1);
        if (ok < 0) {
            snprintf(g.dir_path, sizeof(g.dir_path), "0:/users/%s", name);
        }
    } else {
        montauk::strncpy(g.dir_path, "0:/home", (int)sizeof(g.dir_path));
    }
    doc.destroy();
}

static void parse_args(char* auto_file, int auto_file_cap) {
    char args[512] = {};
    int arglen = montauk::getargs(args, sizeof(args));
    if (arglen <= 0 || !args[0]) {
        set_default_dir();
        return;
    }

    if (str_ends_with(args, ".avi")) {
        int last_slash = -1;
        for (int i = 0; args[i]; i++) {
            if (args[i] == '/') last_slash = i;
        }
        if (last_slash > 0) {
            int len = last_slash;
            if (len >= (int)sizeof(g.dir_path)) len = sizeof(g.dir_path) - 1;
            montauk::memcpy(g.dir_path, args, len);
            g.dir_path[len] = '\0';
            montauk::strncpy(auto_file, args + last_slash + 1, auto_file_cap);
        } else {
            set_default_dir();
            montauk::strncpy(auto_file, args, auto_file_cap);
        }
    } else {
        montauk::strncpy(g.dir_path, args, (int)sizeof(g.dir_path));
    }
}

extern "C" void _start() {
    montauk::memset(&g, 0, sizeof(g));
    g.win_w = INIT_W;
    g.win_h = INIT_H;
    g.movie.fd = -1;
    g.current_video = -1;
    g.selected_item = -1;
    g.hovered_item = -1;
    g.audio_handle = -1;
    g.displayed_frame_index = -1;
    g.play_state = PlayState::Stopped;
    set_status("Open an AVI from your Videos folder");

    char auto_file[128] = {};
    parse_args(auto_file, (int)sizeof(auto_file));

    TrueTypeFont* font = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
    if (font) {
        montauk::memset(font, 0, sizeof(TrueTypeFont));
        if (!font->init("0:/fonts/Roboto-Medium.ttf")) {
            montauk::mfree(font);
            font = nullptr;
        }
    }
    g.font = font;

    Color icon_fg = TEXT;
    g.ico_rewind  = svg_load("0:/icons/media-rewind.svg",  16, 16, icon_fg);
    g.ico_play    = svg_load("0:/icons/media-play.svg",    16, 16, icon_fg);
    g.ico_pause   = svg_load("0:/icons/media-pause.svg",   16, 16, icon_fg);
    g.ico_stop    = svg_load("0:/icons/media-stop.svg",    16, 16, icon_fg);
    g.ico_forward = svg_load("0:/icons/media-forward.svg", 16, 16, icon_fg);
    g.ico_play_w  = svg_load("0:/icons/media-play.svg",    16, 16, WHITE);
    g.ico_pause_w = svg_load("0:/icons/media-pause.svg",   16, 16, WHITE);
    g.ico_stop_w  = svg_load("0:/icons/media-stop.svg",    16, 16, WHITE);
    g.ico_repeat  = svg_load("0:/icons/media-playlist-repeat.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, icon_fg);
    g.ico_repeat_w = svg_load("0:/icons/media-playlist-repeat.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, WHITE);

    scan_directory();

    int auto_play_idx = -1;
    if (auto_file[0]) {
        for (int i = 0; i < g.file_count; i++) {
            if (str_eq_case_insensitive(g.files[i].name, auto_file)) {
                auto_play_idx = i;
                break;
            }
        }
    }

    WsWindow win;
    if (!win.create("Videos", INIT_W, INIT_H))
        montauk::exit(1);

    if (auto_play_idx >= 0) {
        g.selected_item = auto_play_idx;
        start_video(auto_play_idx);
    }

    render(win.pixels);
    win.present();

    uint64_t last_draw = montauk::get_milliseconds();

    while (true) {
        Montauk::WinEvent ev;
        int r = win.poll(&ev);

        if (r < 0) break;

        bool redraw = false;
        bool quit = false;

        if (r == 0) {
            if (g.play_state == PlayState::Playing) {
                if (tick_playback()) redraw = true;
                uint64_t now = montauk::get_milliseconds();

                if (now - last_draw >= 16) redraw = true;
            }

            if (redraw) {
                render(win.pixels);
                win.present();
                last_draw = montauk::get_milliseconds();
            }

            montauk::sleep_ms(g.play_state == PlayState::Playing ? 4 : 16);
            continue;
        }

        if (ev.type == 3) break;

        if (ev.type == 2) {
            g.win_w = win.width;
            g.win_h = win.height;
            ensure_selected_visible();
            redraw = true;
        } else if (ev.type == 0 && ev.key.pressed) {
            if (handle_key(ev.key, quit)) redraw = true;
        } else if (ev.type == 1) {
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
            bool released = !(ev.mouse.buttons & 1) && (ev.mouse.prev_buttons & 1);

            if (clicked) {
                if (handle_click(ev.mouse.x, ev.mouse.y)) redraw = true;
            }

            if (g.dragging_progress && (ev.mouse.buttons & 1)) {
                g.drag_time_us = progress_time_from_x(ev.mouse.x);
                sync_frame_to_time(g.drag_time_us);
                redraw = true;
            }

            if (released && g.dragging_progress) {
                uint64_t target = g.drag_time_us;
                g.dragging_progress = false;
                seek_to_time(target);
                redraw = true;
            }
        }

        if (quit) break;

        if (g.play_state == PlayState::Playing) {
            if (tick_playback()) redraw = true;
        }

        if (redraw) {
            render(win.pixels);
            win.present();
            last_draw = montauk::get_milliseconds();
        }
    }

    unload_movie();
    if (g.font) montauk::mfree(g.font);
    win.destroy();
    montauk::exit(0);
}
