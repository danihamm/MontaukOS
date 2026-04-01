/*
 * main.cpp
 * MontaukOS Music Player — MP3 and WAV playback
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/config.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>
#include <gui/svg.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
}

#include "minimp3.h"
#include "visualizer.hpp"

using namespace gui;

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W         = 380;
static constexpr int INIT_H         = 520;
static constexpr int TOOLBAR_H      = 36;
static constexpr int TOOL_ICON_W    = 38;
static constexpr int TOOL_ICON_H    = 26;
static constexpr int TOOL_ICON_GAP  = 8;
static constexpr int TOOL_ICON_SIZE = 16;
static constexpr int FONT_SIZE      = 18;
static constexpr int FONT_SIZE_SM   = 16;
static constexpr int FONT_SIZE_LG   = 20;

static constexpr int LIST_TOP       = TOOLBAR_H;
static constexpr int LIST_ITEM_H    = 32;
static constexpr int INFO_H         = 56;
static constexpr int PROGRESS_H     = 24;
static constexpr int TRANSPORT_H    = 44;

static constexpr int MAX_FILES      = 128;
static constexpr int MAX_PATH       = 256;
static constexpr int MAX_FILTER     = 64;

// Decode buffer: one MP3 frame = up to 1152 samples * 2 channels
static constexpr int PCM_BUF_SAMPLES = 1152 * 2;
// Audio write chunk size (bytes) — feed in small chunks for responsiveness
static constexpr int AUDIO_CHUNK     = 4096;

static constexpr Color BG_COLOR       = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TOOLBAR_BG     = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color BORDER_COLOR   = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color TEXT_COLOR     = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color DIM_TEXT       = Color::from_rgb(0x88, 0x88, 0x88);
static constexpr Color TRACK_BG       = Color::from_rgb(0xDD, 0xDD, 0xDD);
static constexpr Color WHITE          = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color BTN_BG         = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color STOP_COLOR     = Color::from_rgb(0xCC, 0x33, 0x33);

// ============================================================================
// State
// ============================================================================

enum class PlayState { Stopped, Playing, Paused };
enum class RepeatMode { Off, All, One };

struct FileEntry {
    char name[128];
    bool is_mp3;  // true = mp3, false = wav
};

struct Mp3Frame {
    uint64_t samples_before;
    uint32_t offset;
    uint16_t sample_count;
    uint16_t reserved;
};

struct PlayerState {
    // Window
    int win_w, win_h;

    // Font
    TrueTypeFont* font;

    // Directory
    char dir_path[MAX_PATH];
    FileEntry files[MAX_FILES];
    int file_count;

    // List UI
    int scroll_y;
    int hovered_item;
    int selected_item;
    int filtered_indices[MAX_FILES];
    int filtered_count;
    char filter_text[MAX_FILTER];
    bool filter_active;

    // Playback
    PlayState play_state;
    int current_track;    // index into files[], -1 if none
    bool show_visualizer;
    bool shuffle_enabled;
    RepeatMode repeat_mode;
    uint32_t rng_state;

    // Audio handle
    int audio_handle;

    // File data (entire file loaded into memory)
    uint8_t* file_data;
    uint64_t file_size;

    // MP3 decoder
    mp3dec_t mp3dec;
    uint64_t mp3_offset;     // current read position in file_data
    Mp3Frame* mp3_frames;
    int mp3_frame_count;
    int mp3_frame_cap;
    uint64_t mp3_seek_discard_samples;
    int sample_rate;
    int channels;

    // WAV state
    uint64_t wav_data_offset; // offset to PCM data in file
    uint64_t wav_data_size;   // size of PCM data
    uint64_t wav_pos;         // current position in PCM data

    // Timing
    uint64_t total_samples;   // total decoded samples (per channel)
    uint64_t played_samples;  // samples fed to audio device

    // PCM buffer for decoded audio
    int16_t pcm_buf[PCM_BUF_SAMPLES];
    int pcm_buf_len;     // samples remaining in pcm_buf
    int pcm_buf_pos;     // current read position in pcm_buf

    // Progress dragging
    bool dragging_progress;

    // Transport icons (dark for normal, white for active/colored backgrounds)
    SvgIcon ico_rewind;
    SvgIcon ico_play;
    SvgIcon ico_pause;
    SvgIcon ico_stop;
    SvgIcon ico_forward;
    SvgIcon ico_play_w;
    SvgIcon ico_pause_w;
    SvgIcon ico_stop_w;
    SvgIcon ico_shuffle;
    SvgIcon ico_shuffle_w;
    SvgIcon ico_repeat_all;
    SvgIcon ico_repeat_all_w;
    SvgIcon ico_repeat_one;
    SvgIcon ico_repeat_one_w;
    SvgIcon ico_visualizer;
    SvgIcon ico_visualizer_w;

    music_visualizer::State visualizer;
};

static PlayerState g;
static Color g_accent = colors::ACCENT;
static Color g_accent_dark = Color::from_rgb(0x2A, 0x62, 0xC8);
static Color g_list_hover = Color::from_rgb(0xE8, 0xF0, 0xFD);
static Color g_list_playing = Color::from_rgb(0xD0, 0xE2, 0xFB);
static Color g_list_selected = Color::from_rgb(0xF0, 0xF5, 0xFC);

static int visible_items();

static Color shade(Color c, int delta) {
    return Color::from_rgb(
        (uint8_t)gui_clamp((int)c.r + delta, 0, 255),
        (uint8_t)gui_clamp((int)c.g + delta, 0, 255),
        (uint8_t)gui_clamp((int)c.b + delta, 0, 255));
}

static Color mix(Color a, Color b, int t) {
    t = gui_clamp(t, 0, 255);
    int inv = 255 - t;
    return Color::from_rgb(
        (uint8_t)((a.r * inv + b.r * t + 127) / 255),
        (uint8_t)((a.g * inv + b.g * t + 127) / 255),
        (uint8_t)((a.b * inv + b.b * t + 127) / 255));
}

static void music_set_accent(Color accent) {
    g_accent = accent;
    g_accent_dark = shade(accent, -28);
    g_list_hover = mix(accent, WHITE, 220);
    g_list_playing = mix(accent, WHITE, 196);
    g_list_selected = mix(accent, WHITE, 236);
}

static void music_load_accent() {
    music_set_accent(colors::ACCENT);

    char user[64];
    montauk::memset(user, 0, sizeof(user));
    if (montauk::getuser(user, sizeof(user)) > 0 && user[0]) {
        auto doc = montauk::config::load_user(user, "desktop");
        int64_t accent = doc.get_int("appearance.accent_color", -1);
        if (accent >= 0) {
            music_set_accent(Color::from_rgb(
                (uint8_t)((accent >> 16) & 0xFF),
                (uint8_t)((accent >> 8) & 0xFF),
                (uint8_t)(accent & 0xFF)));
            doc.destroy();
            return;
        }
        doc.destroy();
    }

    auto doc = montauk::config::load("desktop");
    int64_t accent = doc.get_int("appearance.accent_color", -1);
    if (accent >= 0) {
        music_set_accent(Color::from_rgb(
            (uint8_t)((accent >> 16) & 0xFF),
            (uint8_t)((accent >> 8) & 0xFF),
            (uint8_t)(accent & 0xFF)));
    }
    doc.destroy();
}

// ============================================================================
// Pixel helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x,   y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

static void px_hline(uint32_t* px, int bw, int bh, int x, int y, int w, Color c) {
    if (y < 0 || y >= bh) return;
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > bw ? bw : x + w;
    for (int col = x0; col < x1; col++)
        px[y * bw + col] = v;
}

static void px_fill_rounded(uint32_t* px, int bw, int bh,
                             int x, int y, int w, int h, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            bool skip = false;
            int cx, cy;
            if      (col < r     && row < r)     { cx = r - col - 1; cy = r - row - 1; if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col >= w - r && row < r)     { cx = col - (w - r); cy = r - row - 1; if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col < r     && row >= h - r) { cx = r - col - 1; cy = row - (h - r); if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col >= w - r && row >= h - r){ cx = col - (w - r); cy = row - (h - r); if (cx*cx + cy*cy >= r*r) skip = true; }
            if (!skip) px[dy * bw + dx] = v;
        }
    }
}

static void px_circle(uint32_t* px, int bw, int bh,
                      int cx, int cy, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= bh) continue;
        for (int dx = -r; dx <= r; dx++) {
            int ppx = cx + dx;
            if (ppx < 0 || ppx >= bw) continue;
            if (dx * dx + dy * dy <= r * r)
                px[py * bw + ppx] = v;
        }
    }
}

// Draw a filled triangle (play button shape)
static void px_triangle_right(uint32_t* px, int bw, int bh,
                               int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        // For a right-pointing triangle: width = w * min(row, h-1-row) / (h/2)
        int half = h / 2;
        int span;
        if (half == 0) span = w;
        else span = w * (row <= half ? row : h - 1 - row) / half;
        for (int col = 0; col < span; col++) {
            int dx = x + col;
            if (dx >= 0 && dx < bw)
                px[dy * bw + dx] = v;
        }
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
                uint32_t a = sa, inv_a = 255 - sa;
                uint32_t rr = (a * ((src >> 16) & 0xFF) + inv_a * ((dst >> 16) & 0xFF) + 128) / 255;
                uint32_t gg = (a * ((src >> 8) & 0xFF) + inv_a * ((dst >> 8) & 0xFF) + 128) / 255;
                uint32_t bb = (a * (src & 0xFF) + inv_a * (dst & 0xFF) + 128) / 255;
                px[dy * bw + dx] = 0xFF000000 | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
}

static void px_text(uint32_t* px, int bw, int bh,
                    int x, int y, const char* text, Color c, int size = FONT_SIZE) {
    if (g.font)
        g.font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}

static int text_w(const char* text, int size = FONT_SIZE) {
    return g.font ? g.font->measure_text(text, size) : 0;
}

static int font_h(int size = FONT_SIZE) {
    if (!g.font) return 16;
    auto* cache = g.font->get_cache(size);
    return cache->ascent - cache->descent;
}

static void px_button(uint32_t* px, int bw, int bh,
                      int x, int y, int w, int h,
                      const char* label, Color bg, Color fg, int r,
                      int size = FONT_SIZE) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    int tw = text_w(label, size);
    int fh = font_h(size);
    px_text(px, bw, bh, x + (w - tw) / 2, y + (h - fh) / 2, label, fg, size);
}

static void px_icon_button(uint32_t* px, int bw, int bh,
                           int x, int y, int w, int h,
                           const SvgIcon& ic, Color bg, int r) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    int ix = x + (w - ic.width) / 2;
    int iy = y + (h - ic.height) / 2;
    px_icon(px, bw, bh, ix, iy, ic);
}

// ============================================================================
// String helpers
// ============================================================================

static bool str_ends_with(const char* s, const char* suffix) {
    int sl = montauk::slen(s);
    int xl = montauk::slen(suffix);
    if (xl > sl) return false;
    for (int i = 0; i < xl; i++) {
        char a = s[sl - xl + i];
        char b = suffix[i];
        // case-insensitive
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static const char* repeat_mode_label() {
    switch (g.repeat_mode) {
        case RepeatMode::All: return "Rpt All";
        case RepeatMode::One: return "Rpt 1";
        default: return "Rpt Off";
    }
}

static void cycle_repeat_mode() {
    switch (g.repeat_mode) {
        case RepeatMode::Off: g.repeat_mode = RepeatMode::All; break;
        case RepeatMode::All: g.repeat_mode = RepeatMode::One; break;
        case RepeatMode::One: g.repeat_mode = RepeatMode::Off; break;
    }
}

static uint32_t next_random_u32() {
    if (g.rng_state == 0) g.rng_state = 0xA341316Cu;
    uint32_t x = g.rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.rng_state = x;
    return x;
}

static Rect repeat_button_rect() {
    int y = (TOOLBAR_H - TOOL_ICON_H) / 2;
    return {g.win_w - 12 - TOOL_ICON_W, y, TOOL_ICON_W, TOOL_ICON_H};
}

static Rect shuffle_button_rect() {
    Rect repeat = repeat_button_rect();
    return {repeat.x - TOOL_ICON_GAP - TOOL_ICON_W, repeat.y, TOOL_ICON_W, TOOL_ICON_H};
}

static Rect visualizer_button_rect() {
    Rect shuffle = shuffle_button_rect();
    return {shuffle.x - TOOL_ICON_GAP - TOOL_ICON_W, shuffle.y, TOOL_ICON_W, TOOL_ICON_H};
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool is_printable_ascii(char c) {
    return c >= 32 && c < 127;
}

static bool str_contains_case_insensitive(const char* text, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!text || !text[0]) return false;

    int text_len = montauk::slen(text);
    int needle_len = montauk::slen(needle);
    if (needle_len > text_len) return false;

    for (int i = 0; i <= text_len - needle_len; i++) {
        bool match = true;
        for (int j = 0; j < needle_len; j++) {
            if (ascii_lower(text[i + j]) != ascii_lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

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
    int max_scroll = g.filtered_count - visible_items();
    if (max_scroll < 0) max_scroll = 0;
    if (g.scroll_y < 0) g.scroll_y = 0;
    if (g.scroll_y > max_scroll) g.scroll_y = max_scroll;
}

static void ensure_selected_visible() {
    int sel = selected_visible_index();
    if (sel < 0) {
        clamp_list_scroll();
        return;
    }

    int visible = visible_items();
    if (visible <= 0) {
        g.scroll_y = 0;
        return;
    }

    if (sel < g.scroll_y) g.scroll_y = sel;
    if (sel >= g.scroll_y + visible) g.scroll_y = sel - visible + 1;
    clamp_list_scroll();
}

static void rebuild_filtered_view() {
    int prev_selected = g.selected_item;
    g.filtered_count = 0;

    for (int i = 0; i < g.file_count && g.filtered_count < MAX_FILES; i++) {
        if (str_contains_case_insensitive(g.files[i].name, g.filter_text)) {
            g.filtered_indices[g.filtered_count++] = i;
        }
    }

    if (!filtered_contains(g.hovered_item)) g.hovered_item = -1;

    if (g.filtered_count <= 0) {
        g.selected_item = -1;
        g.scroll_y = 0;
        return;
    }

    if (filtered_contains(prev_selected)) {
        g.selected_item = prev_selected;
    } else if (filtered_contains(g.current_track)) {
        g.selected_item = g.current_track;
    } else {
        g.selected_item = g.filtered_indices[0];
    }

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
    if (sel < 0) {
        sel = delta >= 0 ? 0 : g.filtered_count - 1;
    } else {
        sel += delta;
        if (sel < 0) sel = 0;
        if (sel >= g.filtered_count) sel = g.filtered_count - 1;
    }

    g.selected_item = g.filtered_indices[sel];
    ensure_selected_visible();
}

// ============================================================================
// Directory scanning
// ============================================================================

static void scan_directory() {
    g.file_count = 0;

    const char* names[256];
    int count = montauk::readdir(g.dir_path, names, 256);

    for (int i = 0; i < count && g.file_count < MAX_FILES; i++) {
        bool is_mp3 = str_ends_with(names[i], ".mp3");
        bool is_wav = str_ends_with(names[i], ".wav");
        if (!is_mp3 && !is_wav) continue;

        FileEntry& f = g.files[g.file_count];
        int len = montauk::slen(names[i]);
        if (len >= (int)sizeof(f.name)) len = sizeof(f.name) - 1;
        montauk::memcpy(f.name, names[i], len);
        f.name[len] = 0;
        f.is_mp3 = is_mp3;
        g.file_count++;
    }

    rebuild_filtered_view();
}

// ============================================================================
// WAV parser
// ============================================================================

struct WavHeader {
    uint32_t riff_id;
    uint32_t file_size;
    uint32_t wave_id;
};

struct WavChunk {
    uint32_t id;
    uint32_t size;
};

struct WavFmt {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

static bool parse_wav(const uint8_t* data, uint64_t size) {
    if (size < 44) return false;

    auto* hdr = (const WavHeader*)data;
    if (hdr->riff_id != 0x46464952) return false; // "RIFF"
    if (hdr->wave_id != 0x45564157) return false; // "WAVE"

    uint64_t pos = 12;
    const WavFmt* fmt = nullptr;

    while (pos + 8 <= size) {
        auto* chunk = (const WavChunk*)(data + pos);
        uint64_t chunk_data = pos + 8;

        if (chunk->id == 0x20746D66) { // "fmt "
            if (chunk->size < sizeof(WavFmt)) return false;
            fmt = (const WavFmt*)(data + chunk_data);
        } else if (chunk->id == 0x61746164) { // "data"
            if (!fmt) return false;
            if (fmt->audio_format != 1) return false; // PCM only
            if (fmt->bits_per_sample != 16) return false; // 16-bit PCM only
            g.sample_rate = fmt->sample_rate;
            g.channels = fmt->channels;
            g.wav_data_offset = chunk_data;
            g.wav_data_size = chunk->size;
            g.wav_pos = 0;
            int bytes_per_sample = (fmt->bits_per_sample / 8) * fmt->channels;
            if (bytes_per_sample > 0)
                g.total_samples = chunk->size / bytes_per_sample;
            return true;
        }

        pos = chunk_data + ((chunk->size + 1) & ~1u); // align to 2 bytes
    }

    return false;
}

// ============================================================================
// MP3 helpers
// ============================================================================

static void free_mp3_index() {
    if (g.mp3_frames) {
        montauk::mfree(g.mp3_frames);
        g.mp3_frames = nullptr;
    }
    g.mp3_frame_count = 0;
    g.mp3_frame_cap = 0;
    g.mp3_seek_discard_samples = 0;
}

static bool push_mp3_frame(uint32_t offset, uint64_t samples_before, uint16_t sample_count) {
    if (g.mp3_frame_count >= g.mp3_frame_cap) {
        int new_cap = g.mp3_frame_cap ? g.mp3_frame_cap * 2 : 1024;
        auto* new_frames = (Mp3Frame*)montauk::realloc(g.mp3_frames,
                            (uint64_t)new_cap * sizeof(Mp3Frame));
        if (!new_frames) return false;
        g.mp3_frames = new_frames;
        g.mp3_frame_cap = new_cap;
    }

    Mp3Frame& frame = g.mp3_frames[g.mp3_frame_count++];
    frame.samples_before = samples_before;
    frame.offset = offset;
    frame.sample_count = sample_count;
    frame.reserved = 0;
    return true;
}

static bool build_mp3_index(uint64_t start_offset) {
    free_mp3_index();
    g.total_samples = 0;
    g.sample_rate = 0;
    g.channels = 0;

    mp3dec_t scan_dec;
    mp3dec_init(&scan_dec);

    uint64_t offset = start_offset;
    while (offset < g.file_size) {
        mp3dec_frame_info_t info = {};
        int samples = mp3dec_decode_frame(&scan_dec,
                                          g.file_data + offset,
                                          (int)(g.file_size - offset),
                                          nullptr, &info);
        if (info.frame_bytes <= 0) break;

        if (samples > 0 && info.hz > 0 && info.channels > 0) {
            uint32_t frame_offset = (uint32_t)(offset + (uint64_t)info.frame_offset);
            if (!push_mp3_frame(frame_offset, g.total_samples, (uint16_t)samples)) {
                free_mp3_index();
                return false;
            }
            if (g.sample_rate == 0) {
                g.sample_rate = info.hz;
                g.channels = info.channels;
            }
            g.total_samples += (uint64_t)samples;
        }

        offset += (uint64_t)info.frame_bytes;
    }

    if (g.mp3_frame_count <= 0 || g.sample_rate <= 0 || g.channels <= 0) {
        free_mp3_index();
        return false;
    }

    g.mp3_offset = g.mp3_frames[0].offset;
    return true;
}

static int pick_shuffled_track(int current) {
    if (g.file_count <= 1) return current >= 0 ? current : 0;
    int pick = (int)(next_random_u32() % (uint32_t)g.file_count);
    if (pick == current) {
        pick = (pick + 1 + (int)(next_random_u32() % (uint32_t)(g.file_count - 1))) % g.file_count;
    }
    return pick;
}

static void seek_to_samples(uint64_t target_samples) {
    if (g.play_state == PlayState::Stopped || g.total_samples == 0) return;
    if (target_samples > g.total_samples) target_samples = g.total_samples;

    g.pcm_buf_len = 0;
    g.pcm_buf_pos = 0;

    if (g.current_track >= 0 && g.files[g.current_track].is_mp3) {
        if (g.mp3_frame_count <= 0) return;
        int lo = 0;
        int hi = g.mp3_frame_count - 1;
        int best = 0;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (g.mp3_frames[mid].samples_before <= target_samples) {
                best = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }

        const Mp3Frame& frame = g.mp3_frames[best];
        g.mp3_offset = frame.offset;
        g.played_samples = frame.samples_before;
        g.mp3_seek_discard_samples = target_samples - frame.samples_before;
        mp3dec_init(&g.mp3dec);
    } else {
        int bytes_per_frame = 2 * g.channels;
        g.wav_pos = target_samples * (uint64_t)bytes_per_frame;
        if (g.wav_pos > g.wav_data_size) g.wav_pos = g.wav_data_size;
        g.played_samples = target_samples;
    }
}

// ============================================================================
// Playback control
// ============================================================================

static void stop_playback() {
    if (g.audio_handle >= 0) {
        montauk::audio_close(g.audio_handle);
        g.audio_handle = -1;
    }
    if (g.file_data) {
        montauk::mfree(g.file_data);
        g.file_data = nullptr;
    }
    free_mp3_index();
    music_visualizer::reset(g.visualizer);
    g.play_state = PlayState::Stopped;
    g.played_samples = 0;
    g.pcm_buf_len = 0;
    g.pcm_buf_pos = 0;
    g.mp3_offset = 0;
    g.wav_pos = 0;
}

static bool start_track(int index) {
    stop_playback();

    if (index < 0 || index >= g.file_count) return false;

    // Build full path
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s/%s", g.dir_path, g.files[index].name);

    // Open and read entire file
    int fh = montauk::open(path);
    if (fh < 0) return false;

    g.file_size = montauk::getsize(fh);
    if (g.file_size == 0 || g.file_size > 64 * 1024 * 1024) { // 64 MiB limit
        montauk::close(fh);
        return false;
    }

    g.file_data = (uint8_t*)montauk::malloc(g.file_size);
    if (!g.file_data) {
        montauk::close(fh);
        return false;
    }

    // Read in chunks (syscall may have size limits)
    uint64_t off = 0;
    while (off < g.file_size) {
        uint64_t chunk = g.file_size - off;
        if (chunk > 32768) chunk = 32768;
        int rd = montauk::read(fh, g.file_data + off, off, chunk);
        if (rd <= 0) break;
        off += rd;
    }
    montauk::close(fh);

    if (off < g.file_size) {
        montauk::mfree(g.file_data);
        g.file_data = nullptr;
        return false;
    }

    // Parse format
    if (g.files[index].is_mp3) {
        mp3dec_init(&g.mp3dec);
        g.mp3_offset = 0;

        // Skip ID3v2
        if (g.file_size >= 10 && g.file_data[0] == 'I' && g.file_data[1] == 'D' && g.file_data[2] == '3') {
            g.mp3_offset = 10 + (((uint64_t)(g.file_data[6] & 0x7F) << 21) |
                                  ((uint64_t)(g.file_data[7] & 0x7F) << 14) |
                                  ((uint64_t)(g.file_data[8] & 0x7F) << 7)  |
                                  ((uint64_t)(g.file_data[9] & 0x7F)));
        }

        if (!build_mp3_index(g.mp3_offset)) {
            montauk::mfree(g.file_data);
            g.file_data = nullptr;
            return false;
        }
    } else {
        if (!parse_wav(g.file_data, g.file_size)) {
            montauk::mfree(g.file_data);
            g.file_data = nullptr;
            return false;
        }
    }

    // Open audio device
    g.audio_handle = montauk::audio_open(g.sample_rate, g.channels, 16);
    if (g.audio_handle < 0) {
        free_mp3_index();
        montauk::mfree(g.file_data);
        g.file_data = nullptr;
        return false;
    }

    g.current_track = index;
    g.play_state = PlayState::Playing;
    g.played_samples = 0;
    g.pcm_buf_len = 0;
    g.pcm_buf_pos = 0;
    g.mp3_seek_discard_samples = 0;
    music_visualizer::reset(g.visualizer);

    if (filtered_contains(index)) {
        g.selected_item = index;
    } else if (g.filtered_count > 0 && selected_visible_index() < 0) {
        g.selected_item = g.filtered_indices[0];
    }
    ensure_selected_visible();

    return true;
}

static void toggle_pause() {
    if (g.play_state == PlayState::Playing) {
        montauk::audio_pause(g.audio_handle);
        g.play_state = PlayState::Paused;
    } else if (g.play_state == PlayState::Paused) {
        montauk::audio_resume(g.audio_handle);
        g.play_state = PlayState::Playing;
    }
}

static bool play_selected_or_default() {
    if (g.selected_item >= 0 && filtered_contains(g.selected_item)) return start_track(g.selected_item);
    if (g.filtered_count > 0) return start_track(g.filtered_indices[0]);
    if (g.filter_text[0]) return false;
    if (g.selected_item >= 0) return start_track(g.selected_item);
    if (g.current_track >= 0) return start_track(g.current_track);
    if (g.file_count > 0) return start_track(0);
    return false;
}

static bool next_track(bool auto_advanced = false) {
    if (g.file_count == 0) return false;
    if (g.current_track < 0) return start_track(0);

    if (auto_advanced && g.repeat_mode == RepeatMode::One) {
        return start_track(g.current_track);
    }

    int next = -1;
    if (g.shuffle_enabled) {
        next = pick_shuffled_track(g.current_track);
    } else if (g.current_track + 1 < g.file_count) {
        next = g.current_track + 1;
    } else if (g.repeat_mode == RepeatMode::All) {
        next = 0;
    }

    if (next >= 0) return start_track(next);
    stop_playback();
    return false;
}

static void prev_track() {
    if (g.file_count == 0) return;
    // If we're more than 3 seconds in, restart current track
    if (g.sample_rate > 0 && g.played_samples > (uint64_t)g.sample_rate * 3) {
        start_track(g.current_track);
        return;
    }
    int prev = g.current_track - 1;
    if (prev < 0) prev = g.file_count - 1;
    start_track(prev);
}

// ============================================================================
// Audio feeding — call from the main loop
// ============================================================================

static void feed_audio() {
    if (g.play_state != PlayState::Playing) return;
    if (g.audio_handle < 0) return;

    // Try to feed a chunk of audio data
    for (int iter = 0; iter < 4; iter++) {
        // If we have leftover PCM in the buffer, write it
        if (g.pcm_buf_len > 0) {
            int bytes_avail = g.pcm_buf_len * 2; // 16-bit samples
            int to_write = bytes_avail > AUDIO_CHUNK ? AUDIO_CHUNK : bytes_avail;
            int written = montauk::audio_write(g.audio_handle,
                                                (const uint8_t*)(g.pcm_buf + g.pcm_buf_pos),
                                                to_write);
            if (written <= 0) return; // device buffer full, try later
            int samples_written = written / 2;
            g.pcm_buf_pos += samples_written;
            g.pcm_buf_len -= samples_written;
            g.played_samples += samples_written / g.channels;
            continue;
        }

        // Decode more audio
        g.pcm_buf_pos = 0;
        g.pcm_buf_len = 0;

        if (g.current_track >= 0 && g.files[g.current_track].is_mp3) {
            // MP3 decode
            if (g.mp3_offset >= g.file_size) {
                // End of file — advance to next track
                next_track(true);
                return;
            }

            mp3dec_frame_info_t info = {};
            int samples = mp3dec_decode_frame(&g.mp3dec,
                                               g.file_data + g.mp3_offset,
                                               (int)(g.file_size - g.mp3_offset),
                                               g.pcm_buf, &info);
            if (info.frame_bytes > 0) {
                g.mp3_offset += info.frame_bytes;
            } else {
                // Can't decode — end
                next_track(true);
                return;
            }

            if (samples > 0) {
                if (g.mp3_seek_discard_samples > 0) {
                    uint64_t discard = g.mp3_seek_discard_samples;
                    if (discard > (uint64_t)samples) discard = (uint64_t)samples;
                    g.pcm_buf_pos = (int)(discard * (uint64_t)info.channels);
                    g.played_samples += discard;
                    g.mp3_seek_discard_samples -= discard;
                    samples -= (int)discard;
                }
                g.pcm_buf_len = samples * info.channels;
                if (g.pcm_buf_len > 0) {
                    music_visualizer::feed_pcm(g.visualizer,
                                               g.pcm_buf + g.pcm_buf_pos,
                                               g.pcm_buf_len / info.channels,
                                               info.channels,
                                               info.hz);
                } else {
                    g.pcm_buf_pos = 0;
                }
            }
        } else {
            // WAV decode — just copy PCM data
            if (g.wav_pos >= g.wav_data_size) {
                next_track(true);
                return;
            }

            uint64_t remaining = g.wav_data_size - g.wav_pos;
            uint64_t to_copy = remaining;
            if (to_copy > sizeof(g.pcm_buf)) to_copy = sizeof(g.pcm_buf);

            memcpy(g.pcm_buf, g.file_data + g.wav_data_offset + g.wav_pos, to_copy);
            g.wav_pos += to_copy;
            g.pcm_buf_len = (int)(to_copy / 2); // 16-bit samples
            if (g.channels > 0 && g.pcm_buf_len > 0) {
                music_visualizer::feed_pcm(g.visualizer,
                                           g.pcm_buf,
                                           g.pcm_buf_len / g.channels,
                                           g.channels,
                                           g.sample_rate);
            }
        }
    }
}

// ============================================================================
// Time formatting
// ============================================================================

static void format_time(char* buf, int buf_size, uint64_t samples, int rate) {
    if (rate <= 0) { snprintf(buf, buf_size, "--:--"); return; }
    uint64_t secs = samples / rate;
    int m = (int)(secs / 60);
    int s = (int)(secs % 60);
    snprintf(buf, buf_size, "%d:%02d", m, s);
}

// ============================================================================
// Render
// ============================================================================

static int list_bottom() {
    return g.win_h - INFO_H - PROGRESS_H - TRANSPORT_H;
}

static int visible_items() {
    int count = (list_bottom() - LIST_TOP) / LIST_ITEM_H;
    return count > 0 ? count : 0;
}

static Rect content_panel_rect() {
    int y = list_bottom() + 1;
    return {0, LIST_TOP, g.win_w, y - LIST_TOP};
}

static void render(uint32_t* pixels) {
    int W = g.win_w, H = g.win_h;
    int fh = font_h();
    int fh_sm = font_h(FONT_SIZE_SM);

    // Background
    px_fill(pixels, W, H, 0, 0, W, H, BG_COLOR);

    // Toolbar
    px_fill(pixels, W, H, 0, 0, W, TOOLBAR_H, TOOLBAR_BG);
    px_hline(pixels, W, H, 0, TOOLBAR_H - 1, W, BORDER_COLOR);
    Rect visualizer_rect = visualizer_button_rect();
    Rect shuffle_rect = shuffle_button_rect();
    Rect repeat_rect = repeat_button_rect();
    px_icon_button(pixels, W, H, visualizer_rect.x, visualizer_rect.y,
                   visualizer_rect.w, visualizer_rect.h,
                   g.show_visualizer ? g.ico_visualizer_w : g.ico_visualizer,
                   g.show_visualizer ? g_accent : BTN_BG, 6);
    px_icon_button(pixels, W, H, shuffle_rect.x, shuffle_rect.y, shuffle_rect.w, shuffle_rect.h,
                   g.shuffle_enabled ? g.ico_shuffle_w : g.ico_shuffle,
                   g.shuffle_enabled ? g_accent : BTN_BG, 6);
    Color repeat_bg = (g.repeat_mode == RepeatMode::Off) ? BTN_BG : g_accent;
    px_icon_button(pixels, W, H, repeat_rect.x, repeat_rect.y, repeat_rect.w, repeat_rect.h,
                   (g.repeat_mode == RepeatMode::One) ? g.ico_repeat_one_w :
                   (g.repeat_mode == RepeatMode::All) ? g.ico_repeat_all_w :
                                                        g.ico_repeat_all,
                   repeat_bg, 6);

    // Directory path in toolbar
    {
        int max_dir_w = visualizer_rect.x - 20;
        if (g.filter_active || g.filter_text[0]) {
            char filter_query[24];
            char filter_label[48];
            const char* query = g.filter_text;
            int qlen = montauk::slen(query);
            if (qlen > 15) {
                montauk::memcpy(filter_query, "...", 3);
                montauk::memcpy(filter_query + 3, query + qlen - 15, 15);
                filter_query[18] = '\0';
                query = filter_query;
            }

            if (g.filter_text[0]) {
                snprintf(filter_label, sizeof(filter_label), "%s%s%s",
                         g.filter_active ? "Find: " : "Filter: ",
                         query,
                         g.filter_active ? "_" : "");
            } else {
                snprintf(filter_label, sizeof(filter_label), "%s",
                         g.filter_active ? "Find" : "Filter");
            }

            int pill_h = TOOL_ICON_H;
            int pill_w = text_w(filter_label, FONT_SIZE_SM) + 14;
            int pill_x = visualizer_rect.x - TOOL_ICON_GAP - pill_w;
            int pill_y = (TOOLBAR_H - pill_h) / 2;
            Color pill_bg = g.filter_active ? g_list_hover : BTN_BG;
            Color pill_fg = g.filter_active ? g_accent_dark : DIM_TEXT;
            if (pill_x > 80) {
                px_fill_rounded(pixels, W, H, pill_x, pill_y, pill_w, pill_h, 6, pill_bg);
                px_text(pixels, W, H, pill_x + 7, pill_y + (pill_h - fh_sm) / 2,
                        filter_label, pill_fg, FONT_SIZE_SM);
                max_dir_w = pill_x - 12;
            }
        }

        const char* display_dir = g.dir_path;
        int dw = text_w(display_dir, FONT_SIZE_SM);
        int dir_len = montauk::slen(g.dir_path);
        if (dw > max_dir_w && dir_len > 20) {
            display_dir = g.dir_path + montauk::slen(g.dir_path) - 20;
        }
        px_text(pixels, W, H, 12,
                (TOOLBAR_H - font_h(FONT_SIZE_SM)) / 2, display_dir, TEXT_COLOR, FONT_SIZE_SM);
    }

    Rect content_panel = content_panel_rect();
    int list_end = list_bottom();
    int visible_count = visible_items();
    px_hline(pixels, W, H, 0, list_end, W, BORDER_COLOR);

    if (!g.show_visualizer) {
        for (int i = 0; i < visible_count && (i + g.scroll_y) < g.filtered_count; i++) {
            int idx = filtered_raw_at(i + g.scroll_y);
            if (idx < 0) break;
            int iy = LIST_TOP + i * LIST_ITEM_H;

            Color bg = BG_COLOR;
            if (idx == g.current_track && g.play_state != PlayState::Stopped)
                bg = g_list_playing;
            else if (idx == g.selected_item)
                bg = g_list_selected;
            else if (idx == g.hovered_item)
                bg = g_list_hover;

            if (bg.r != BG_COLOR.r || bg.g != BG_COLOR.g || bg.b != BG_COLOR.b)
                px_fill(pixels, W, H, 0, iy, W, LIST_ITEM_H, bg);

            if (idx == g.selected_item && !(idx == g.current_track && g.play_state != PlayState::Stopped))
                px_fill(pixels, W, H, 0, iy, 3, LIST_ITEM_H, g_accent);

            if (idx == g.current_track && g.play_state == PlayState::Playing) {
                px_triangle_right(pixels, W, H, 8, iy + 8, 8, 12, g_accent);
            } else if (idx == g.current_track && g.play_state == PlayState::Paused) {
                px_fill(pixels, W, H, 8, iy + 8, 3, 12, g_accent);
                px_fill(pixels, W, H, 13, iy + 8, 3, 12, g_accent);
            }

            Color name_color = (idx == g.current_track && g.play_state != PlayState::Stopped)
                                ? g_accent_dark : TEXT_COLOR;
            px_text(pixels, W, H, 24, iy + (LIST_ITEM_H - fh_sm) / 2,
                    g.files[idx].name, name_color, FONT_SIZE_SM);

            const char* tag = g.files[idx].is_mp3 ? "MP3" : "WAV";
            int tw = text_w(tag, FONT_SIZE_SM);
            px_text(pixels, W, H, W - tw - 12, iy + (LIST_ITEM_H - fh_sm) / 2,
                    tag, DIM_TEXT, FONT_SIZE_SM);
        }

        if (g.filtered_count > visible_count && visible_count > 0) {
            int list_h = list_end - LIST_TOP;
            int sb_h = (visible_count * list_h) / g.filtered_count;
            if (sb_h < 20) sb_h = 20;
            int sb_y = LIST_TOP + (g.scroll_y * (list_h - sb_h)) / (g.filtered_count - visible_count);
            px_fill_rounded(pixels, W, H, W - 6, sb_y, 4, sb_h, 2, TRACK_BG);
        }

        if (g.filtered_count == 0) {
            const char* msg = g.file_count == 0 ? "No audio files found" : "No matches for filter";
            int mw = text_w(msg);
            int cy = LIST_TOP + (list_end - LIST_TOP) / 2 - fh / 2;
            px_text(pixels, W, H, (W - mw) / 2, cy, msg, DIM_TEXT);
            if (g.file_count > 0 && g.filter_text[0]) {
                char detail[96];
                snprintf(detail, sizeof(detail), "Current filter: %s", g.filter_text);
                int dw = text_w(detail, FONT_SIZE_SM);
                px_text(pixels, W, H, (W - dw) / 2, cy + fh_sm + 6, detail, DIM_TEXT, FONT_SIZE_SM);
            }
        }
    } else {
        px_fill(pixels, W, H, content_panel.x, content_panel.y, content_panel.w, content_panel.h, TOOLBAR_BG);
        Rect viz_rect = {12, content_panel.y + 12, W - 24, content_panel.h - 24};
        music_visualizer::render(pixels, W, H, viz_rect, g.visualizer,
                                 g_accent, g_accent_dark, TRACK_BG, BORDER_COLOR);
    }

    // ---- Now Playing info ----
    int info_y = list_end + 1;
    px_fill(pixels, W, H, 0, info_y, W, INFO_H, TOOLBAR_BG);
    px_hline(pixels, W, H, 0, info_y + INFO_H - 1, W, BORDER_COLOR);

    if (g.current_track >= 0 && g.play_state != PlayState::Stopped) {
        // Track name
        px_text(pixels, W, H, 12, info_y + 8,
                g.files[g.current_track].name, TEXT_COLOR, FONT_SIZE);

        // Time display
        char time_cur[16], time_total[16], time_str[40];
        format_time(time_cur, sizeof(time_cur), g.played_samples, g.sample_rate);
        format_time(time_total, sizeof(time_total), g.total_samples, g.sample_rate);
        snprintf(time_str, sizeof(time_str), "%s / %s", time_cur, time_total);
        px_text(pixels, W, H, 12, info_y + 8 + fh + 4, time_str, DIM_TEXT, FONT_SIZE_SM);

        // State indicator on right
        const char* state_str = (g.play_state == PlayState::Playing) ? "Playing" : "Paused";
        int sw = text_w(state_str, FONT_SIZE_SM);
        px_text(pixels, W, H, W - sw - 12, info_y + 8 + fh + 4,
                state_str, g_accent, FONT_SIZE_SM);
    } else {
        const char* msg = "No track selected";
        px_text(pixels, W, H, 12, info_y + (INFO_H - fh) / 2, msg, DIM_TEXT);
    }

    // ---- Progress bar ----
    int prog_y = info_y + INFO_H;
    px_fill(pixels, W, H, 0, prog_y, W, PROGRESS_H, Color::from_rgb(0xF0, 0xF0, 0xF0));

    int bar_x = 16, bar_w = W - 32, bar_h = 6;
    int bar_y = prog_y + (PROGRESS_H - bar_h) / 2;
    px_fill_rounded(pixels, W, H, bar_x, bar_y, bar_w, bar_h, 3, TRACK_BG);

    if (g.total_samples > 0 && g.play_state != PlayState::Stopped) {
        int fill = (int)((g.played_samples * bar_w) / g.total_samples);
        if (fill > bar_w) fill = bar_w;
        if (fill > 0)
            px_fill_rounded(pixels, W, H, bar_x, bar_y, fill, bar_h, 3, g_accent);
        // Knob
        px_circle(pixels, W, H, bar_x + fill, bar_y + bar_h / 2, 5, g_accent);
        px_circle(pixels, W, H, bar_x + fill, bar_y + bar_h / 2, 2, WHITE);
    }

    // ---- Transport controls ----
    int trans_y = prog_y + PROGRESS_H;
    px_fill(pixels, W, H, 0, trans_y, W, TRANSPORT_H, BG_COLOR);

    int btn_w = 48, btn_h = 32, btn_r = 6, gap = 8;
    int total_w = btn_w * 4 + gap * 3;  // prev, play, stop, next
    int bx = (W - total_w) / 2;
    int by = trans_y + (TRANSPORT_H - btn_h) / 2;

    // [<<] Prev
    px_icon_button(pixels, W, H, bx, by, btn_w, btn_h, g.ico_rewind, BTN_BG, btn_r);
    bx += btn_w + gap;

    // [Play/Pause]
    {
        bool active = (g.play_state == PlayState::Playing);
        Color bg = active ? g_accent : BTN_BG;
        const SvgIcon& ic = active ? g.ico_pause_w : g.ico_play;
        px_icon_button(pixels, W, H, bx, by, btn_w, btn_h, ic, bg, btn_r);
    }
    bx += btn_w + gap;

    // [Stop]
    {
        bool active = (g.play_state != PlayState::Stopped);
        Color bg = active ? STOP_COLOR : BTN_BG;
        const SvgIcon& ic = active ? g.ico_stop_w : g.ico_stop;
        px_icon_button(pixels, W, H, bx, by, btn_w, btn_h, ic, bg, btn_r);
    }
    bx += btn_w + gap;

    // [>>] Next
    px_icon_button(pixels, W, H, bx, by, btn_w, btn_h, g.ico_forward, BTN_BG, btn_r);
}

// ============================================================================
// Hit testing
// ============================================================================

static bool handle_click(int mx, int my) {
    int W = g.win_w;
    int lb = list_bottom();
    Rect visualizer_rect = visualizer_button_rect();
    Rect shuffle_rect = shuffle_button_rect();
    Rect repeat_rect = repeat_button_rect();
    int info_y = lb + 1;
    int prog_y = info_y + INFO_H;
    int trans_y = prog_y + PROGRESS_H;

    if (visualizer_rect.contains(mx, my)) {
        g.show_visualizer = !g.show_visualizer;
        return true;
    }

    if (shuffle_rect.contains(mx, my)) {
        g.shuffle_enabled = !g.shuffle_enabled;
        return true;
    }

    if (repeat_rect.contains(mx, my)) {
        cycle_repeat_mode();
        return true;
    }

    // File list click
    if (!g.show_visualizer && my >= LIST_TOP && my < lb && mx >= 0 && mx < W) {
        int idx = filtered_raw_at((my - LIST_TOP) / LIST_ITEM_H + g.scroll_y);
        if (idx >= 0) {
            start_track(idx);
            return true;
        }
    }

    // Progress bar click
    if (my >= prog_y && my < prog_y + PROGRESS_H) {
        if (g.play_state != PlayState::Stopped && g.total_samples > 0) {
            int bar_x = 16, bar_w = W - 32;
            int rel = mx - bar_x;
            if (rel < 0) rel = 0;
            if (rel > bar_w) rel = bar_w;
            uint64_t target_samples = (uint64_t)rel * g.total_samples / bar_w;
            seek_to_samples(target_samples);
            g.dragging_progress = true;
            return true;
        }
    }

    // Transport buttons
    if (my >= trans_y && my < trans_y + TRANSPORT_H) {
        int btn_w = 48, btn_h = 32, gap = 8;
        int total_w = btn_w * 4 + gap * 3;
        int bx = (W - total_w) / 2;
        int by = trans_y + (TRANSPORT_H - btn_h) / 2;

        if (my >= by && my < by + btn_h) {
            // Prev
            if (mx >= bx && mx < bx + btn_w) { prev_track(); return true; }
            bx += btn_w + gap;

            // Play/Pause
            if (mx >= bx && mx < bx + btn_w) {
                if (g.play_state == PlayState::Stopped) {
                    play_selected_or_default();
                } else {
                    toggle_pause();
                }
                return true;
            }
            bx += btn_w + gap;

            // Stop
            if (mx >= bx && mx < bx + btn_w) { stop_playback(); return true; }
            bx += btn_w + gap;

            // Next
            if (mx >= bx && mx < bx + btn_w) { next_track(false); return true; }
        }
    }

    return false;
}

static bool handle_key(const Montauk::KeyEvent& key, bool& quit) {
    if (!key.pressed) return false;

    if (key.scancode == 0x01) { // Escape
        if (g.filter_active) {
            if (g.filter_text[0]) clear_filter();
            else g.filter_active = false;
            return true;
        }
        quit = true;
        return false;
    }

    if (key.ctrl && (key.ascii == 'f' || key.ascii == 'F' || key.ascii == 6)) {
        g.filter_active = true;
        return true;
    }

    if (!key.ctrl && !key.alt && key.ascii == '/') {
        g.filter_active = true;
        return true;
    }

    if (!g.show_visualizer && key.scancode == 0x48) { // Up
        move_selection(-1);
        return true;
    }
    if (!g.show_visualizer && key.scancode == 0x50) { // Down
        move_selection(1);
        return true;
    }
    if (!g.show_visualizer &&
        (key.ascii == '\n' || key.ascii == '\r' || key.scancode == 0x1C)) {
        if (g.selected_item >= 0) {
            start_track(g.selected_item);
            g.filter_active = false;
            return true;
        }
    }

    if (g.filter_active) {
        if (key.ascii == '\b' || key.scancode == 0x0E) {
            if (g.filter_text[0]) pop_filter_char();
            else g.filter_active = false;
            return true;
        }
        if (!key.ctrl && !key.alt && is_printable_ascii(key.ascii)) {
            append_filter_char(key.ascii);
            return true;
        }
        return false;
    }

    if (key.ascii == ' ') {
        if (g.play_state == PlayState::Stopped)
            play_selected_or_default();
        else
            toggle_pause();
        return true;
    }
    if (key.ascii == 's' || key.ascii == 'S') {
        stop_playback();
        return true;
    }
    if (key.ascii == 'h' || key.ascii == 'H') {
        g.shuffle_enabled = !g.shuffle_enabled;
        return true;
    }
    if (key.ascii == 'r' || key.ascii == 'R') {
        cycle_repeat_mode();
        return true;
    }
    if (key.ascii == 'v' || key.ascii == 'V') {
        g.show_visualizer = !g.show_visualizer;
        return true;
    }
    if (key.scancode == 0x4D) { // Right arrow
        next_track(false);
        return true;
    }
    if (key.scancode == 0x4B) { // Left arrow
        prev_track();
        return true;
    }

    return false;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Init state
    memset(&g, 0, sizeof(g));
    g.win_w = INIT_W;
    g.win_h = INIT_H;
    g.audio_handle = -1;
    g.current_track = -1;
    g.play_state = PlayState::Stopped;
    g.hovered_item = -1;
    g.selected_item = -1;
    g.show_visualizer = false;
    g.repeat_mode = RepeatMode::Off;
    g.rng_state = (uint32_t)montauk::get_milliseconds() ^ 0xC0DEFACEu;
    music_visualizer::reset(g.visualizer);

    // Parse arguments: accept a directory path or a file path
    // Helper: set dir_path to current user's home via session config
    auto set_user_home = [&]() {
        auto doc = montauk::config::load("session");
        const char* name = doc.get_string("session.username", "");
        if (name[0]) {
            int p = 0;
            const char* pfx = "0:/users/";
            while (*pfx && p < (int)sizeof(g.dir_path) - 1) g.dir_path[p++] = *pfx++;
            while (*name && p < (int)sizeof(g.dir_path) - 1) g.dir_path[p++] = *name++;
            g.dir_path[p] = '\0';
        } else {
            memcpy(g.dir_path, "0:/home", 8);
        }
        doc.destroy();
    };

    char arg_file[128] = {};
    {
        char args[256];
        int arglen = montauk::getargs(args, sizeof(args));
        if (arglen > 0 && args[0]) {
            if (str_ends_with(args, ".mp3") || str_ends_with(args, ".wav")) {
                // File path: extract directory and filename
                int last_slash = -1;
                for (int i = 0; args[i]; i++) {
                    if (args[i] == '/') last_slash = i;
                }
                if (last_slash > 0) {
                    memcpy(g.dir_path, args, last_slash);
                    g.dir_path[last_slash] = '\0';
                    const char* fname = args + last_slash + 1;
                    int flen = montauk::slen(fname);
                    if (flen > 0 && flen < (int)sizeof(arg_file)) {
                        memcpy(arg_file, fname, flen);
                        arg_file[flen] = '\0';
                    }
                } else {
                    set_user_home();
                }
            } else {
                int len = montauk::slen(args);
                if (len >= (int)sizeof(g.dir_path)) len = sizeof(g.dir_path) - 1;
                memcpy(g.dir_path, args, len);
                g.dir_path[len] = '\0';
            }
        } else {
            set_user_home();
        }
    }

    music_load_accent();

    // Load font
    {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (f) {
            montauk::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init("0:/fonts/Roboto-Medium.ttf")) { montauk::mfree(f); f = nullptr; }
        }
        g.font = f;
    }

    // Load transport icons (dark + white variants for colored backgrounds)
    {
        Color ic_color = TEXT_COLOR;
        g.ico_rewind  = svg_load("0:/icons/media-rewind.svg",  16, 16, ic_color);
        g.ico_play    = svg_load("0:/icons/media-play.svg",    16, 16, ic_color);
        g.ico_pause   = svg_load("0:/icons/media-pause.svg",   16, 16, ic_color);
        g.ico_stop    = svg_load("0:/icons/media-stop.svg",    16, 16, ic_color);
        g.ico_forward = svg_load("0:/icons/media-forward.svg", 16, 16, ic_color);
        g.ico_play_w  = svg_load("0:/icons/media-play.svg",    16, 16, WHITE);
        g.ico_pause_w = svg_load("0:/icons/media-pause.svg",   16, 16, WHITE);
        g.ico_stop_w  = svg_load("0:/icons/media-stop.svg",    16, 16, WHITE);
        g.ico_visualizer = svg_load("0:/icons/view-media-equalizer.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, ic_color);
        g.ico_visualizer_w = svg_load("0:/icons/view-media-equalizer.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, WHITE);
        g.ico_shuffle = svg_load("0:/icons/media-playlist-shuffle.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, ic_color);
        g.ico_shuffle_w = svg_load("0:/icons/media-playlist-shuffle.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, WHITE);
        g.ico_repeat_all = svg_load("0:/icons/media-playlist-repeat.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, ic_color);
        g.ico_repeat_all_w = svg_load("0:/icons/media-playlist-repeat.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, WHITE);
        g.ico_repeat_one = svg_load("0:/icons/media-playlist-repeat-song.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, ic_color);
        g.ico_repeat_one_w = svg_load("0:/icons/media-playlist-repeat-song.svg", TOOL_ICON_SIZE, TOOL_ICON_SIZE, WHITE);
    }

    // Scan for audio files
    scan_directory();

    // If a specific file was requested, find it and auto-play
    int auto_play_idx = -1;
    if (arg_file[0]) {
        for (int i = 0; i < g.file_count; i++) {
            if (str_ends_with(g.files[i].name, arg_file) &&
                montauk::slen(g.files[i].name) == montauk::slen(arg_file)) {
                auto_play_idx = i;
                break;
            }
        }
    }

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Music", g.win_w, g.win_h, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    // Auto-play the requested file
    if (auto_play_idx >= 0) {
        start_track(auto_play_idx);
    }

    render(pixels);
    montauk::win_present(win_id);

    uint64_t last_render = montauk::get_milliseconds();

    while (true) {
        Montauk::WinEvent ev;
        bool redraw = false;
        bool quit = false;
        int r;

        // Drain all pending events before rendering
        while ((r = montauk::win_poll(win_id, &ev)) > 0) {
            if (ev.type == 3) { quit = true; break; }

            // Keyboard
            if (ev.type == 0 && ev.key.pressed) {
                if (handle_key(ev.key, quit))
                    redraw = true;
                if (quit) break;
            }

            // Mouse
            if (ev.type == 1) {
                bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
                bool released = !(ev.mouse.buttons & 1) && (ev.mouse.prev_buttons & 1);

                if (clicked) {
                    if (handle_click(ev.mouse.x, ev.mouse.y))
                        redraw = true;
                }

                if (released) {
                    g.dragging_progress = false;
                }

                // Progress bar drag
                if (g.dragging_progress && (ev.mouse.buttons & 1)) {
                    int bar_x = 16, bar_w = g.win_w - 32;
                    int rel = ev.mouse.x - bar_x;
                    if (rel < 0) rel = 0;
                    if (rel > bar_w) rel = bar_w;
                    uint64_t target_samples = (uint64_t)rel * g.total_samples / bar_w;
                    seek_to_samples(target_samples);
                    redraw = true;
                }

                // Hover tracking for file list
                int lb = list_bottom();
                if (!g.show_visualizer && ev.mouse.y >= LIST_TOP && ev.mouse.y < lb) {
                    int new_hover = filtered_raw_at((ev.mouse.y - LIST_TOP) / LIST_ITEM_H + g.scroll_y);
                    if (new_hover != g.hovered_item) {
                        g.hovered_item = new_hover;
                        redraw = true;
                    }
                } else if (g.hovered_item != -1) {
                    g.hovered_item = -1;
                    redraw = true;
                }

                // Scroll
                if (!g.show_visualizer && ev.mouse.scroll != 0) {
                    g.scroll_y -= ev.mouse.scroll * 2;
                    clamp_list_scroll();
                    redraw = true;
                }
            }

            // Resize
            if (ev.type == 2) {
                g.win_w = ev.resize.w;
                g.win_h = ev.resize.h;
                pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g.win_w, g.win_h);
                ensure_selected_visible();
                redraw = true;
            }
        }

        if (quit || r < 0) break;

        // Feed audio data to the device
        feed_audio();

        // Periodic redraw while playing (update time display)
        uint64_t now = montauk::get_milliseconds();
        if (g.play_state == PlayState::Playing && now - last_render >= 50) {
            redraw = true;
        }

        if (redraw) {
            music_visualizer::tick(g.visualizer);
            render(pixels);
            montauk::win_present(win_id);
            last_render = now;
        }

        // Sleep less when playing to keep audio fed
        if (g.play_state == PlayState::Playing)
            montauk::sleep_ms(4);
        else
            montauk::sleep_ms(16);
    }

    stop_playback();
    montauk::win_destroy(win_id);
    montauk::exit(0);
}
