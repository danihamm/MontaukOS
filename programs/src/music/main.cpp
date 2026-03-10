/*
 * main.cpp
 * MontaukOS Music Player — MP3 and WAV playback
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>
#include <gui/svg.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
}

#include "minimp3.h"

using namespace gui;

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W         = 380;
static constexpr int INIT_H         = 460;
static constexpr int TOOLBAR_H      = 36;
static constexpr int FONT_SIZE      = 18;
static constexpr int FONT_SIZE_SM   = 16;
static constexpr int FONT_SIZE_LG   = 20;

static constexpr int LIST_TOP       = TOOLBAR_H;
static constexpr int LIST_ITEM_H    = 32;
static constexpr int INFO_H         = 64;
static constexpr int PROGRESS_H     = 24;
static constexpr int TRANSPORT_H    = 44;

static constexpr int MAX_FILES      = 128;
static constexpr int MAX_PATH       = 256;

// Decode buffer: one MP3 frame = up to 1152 samples * 2 channels
static constexpr int PCM_BUF_SAMPLES = 1152 * 2;
// Audio write chunk size (bytes) — feed in small chunks for responsiveness
static constexpr int AUDIO_CHUNK     = 4096;

static constexpr Color BG_COLOR       = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TOOLBAR_BG     = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color BORDER_COLOR   = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color TEXT_COLOR     = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color DIM_TEXT       = Color::from_rgb(0x88, 0x88, 0x88);
static constexpr Color ACCENT         = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color ACCENT_DARK    = Color::from_rgb(0x2A, 0x62, 0xC8);
static constexpr Color TRACK_BG       = Color::from_rgb(0xDD, 0xDD, 0xDD);
static constexpr Color WHITE          = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color LIST_HOVER     = Color::from_rgb(0xE8, 0xF0, 0xFD);
static constexpr Color LIST_PLAYING   = Color::from_rgb(0xD0, 0xE2, 0xFB);
static constexpr Color BTN_BG         = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color BTN_HOVER      = Color::from_rgb(0xD8, 0xD8, 0xD8);
static constexpr Color STOP_COLOR     = Color::from_rgb(0xCC, 0x33, 0x33);

// ============================================================================
// State
// ============================================================================

enum class PlayState { Stopped, Playing, Paused };

struct FileEntry {
    char name[128];
    bool is_mp3;  // true = mp3, false = wav
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

    // Playback
    PlayState play_state;
    int current_track;    // index into files[], -1 if none

    // Audio handle
    int audio_handle;

    // File data (entire file loaded into memory)
    uint8_t* file_data;
    uint64_t file_size;

    // MP3 decoder
    mp3dec_t mp3dec;
    uint64_t mp3_offset;     // current read position in file_data
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
};

static PlayerState g;

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
                      const char* label, Color bg, Color fg, int r) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    int tw = text_w(label);
    int fh = font_h();
    px_text(px, bw, bh, x + (w - tw) / 2, y + (h - fh) / 2, label, fg);
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

static bool scan_mp3_info() {
    // Scan through the file to estimate total duration
    mp3dec_t tmp;
    mp3dec_init(&tmp);

    uint64_t offset = 0;
    g.total_samples = 0;
    g.sample_rate = 0;
    g.channels = 0;

    // Skip ID3v2 tag if present
    if (g.file_size >= 10 && g.file_data[0] == 'I' && g.file_data[1] == 'D' && g.file_data[2] == '3') {
        uint64_t tag_size = ((uint64_t)(g.file_data[6] & 0x7F) << 21) |
                            ((uint64_t)(g.file_data[7] & 0x7F) << 14) |
                            ((uint64_t)(g.file_data[8] & 0x7F) << 7)  |
                            ((uint64_t)(g.file_data[9] & 0x7F));
        offset = tag_size + 10;
    }

    // Quick scan: decode first frame for format, then estimate from bitrate
    mp3dec_frame_info_t info;
    int16_t pcm_tmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int samples = mp3dec_decode_frame(&tmp, g.file_data + offset,
                                       (int)(g.file_size - offset), pcm_tmp, &info);
    if (samples <= 0 || info.hz == 0) return false;

    g.sample_rate = info.hz;
    g.channels = info.channels;

    // Estimate total samples from file size and bitrate
    if (info.bitrate_kbps > 0) {
        uint64_t audio_bytes = g.file_size - offset;
        // duration_sec = audio_bytes * 8 / (bitrate_kbps * 1000)
        // total_samples = duration_sec * sample_rate
        g.total_samples = (audio_bytes * 8 * (uint64_t)g.sample_rate) /
                          ((uint64_t)info.bitrate_kbps * 1000);
    } else {
        // VBR with no bitrate info — rough estimate
        g.total_samples = g.file_size / 4 * (uint64_t)g.sample_rate / 11025;
    }

    return true;
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

        if (!scan_mp3_info()) {
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
        montauk::mfree(g.file_data);
        g.file_data = nullptr;
        return false;
    }

    g.current_track = index;
    g.play_state = PlayState::Playing;
    g.played_samples = 0;
    g.pcm_buf_len = 0;
    g.pcm_buf_pos = 0;

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

static void next_track() {
    if (g.file_count == 0) return;
    int next = (g.current_track + 1) % g.file_count;
    start_track(next);
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
                next_track();
                return;
            }

            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&g.mp3dec,
                                               g.file_data + g.mp3_offset,
                                               (int)(g.file_size - g.mp3_offset),
                                               g.pcm_buf, &info);
            if (info.frame_bytes > 0) {
                g.mp3_offset += info.frame_bytes;
            } else {
                // Can't decode — end
                next_track();
                return;
            }

            if (samples > 0) {
                g.pcm_buf_len = samples * info.channels;
            }
        } else {
            // WAV decode — just copy PCM data
            if (g.wav_pos >= g.wav_data_size) {
                next_track();
                return;
            }

            uint64_t remaining = g.wav_data_size - g.wav_pos;
            uint64_t to_copy = remaining;
            if (to_copy > sizeof(g.pcm_buf)) to_copy = sizeof(g.pcm_buf);

            memcpy(g.pcm_buf, g.file_data + g.wav_data_offset + g.wav_pos, to_copy);
            g.wav_pos += to_copy;
            g.pcm_buf_len = (int)(to_copy / 2); // 16-bit samples
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

static void render(uint32_t* pixels) {
    int W = g.win_w, H = g.win_h;
    int fh = font_h();
    int fh_sm = font_h(FONT_SIZE_SM);

    // Background
    px_fill(pixels, W, H, 0, 0, W, H, BG_COLOR);

    // Toolbar
    px_fill(pixels, W, H, 0, 0, W, TOOLBAR_H, TOOLBAR_BG);
    px_hline(pixels, W, H, 0, TOOLBAR_H - 1, W, BORDER_COLOR);
    // Directory path in toolbar
    {
        const char* display_dir = g.dir_path;
        int dw = text_w(display_dir, FONT_SIZE_SM);
        int max_dir_w = W - 24;
        if (dw > max_dir_w) {
            display_dir = g.dir_path + montauk::slen(g.dir_path) - 20;
        }
        px_text(pixels, W, H, 12,
                (TOOLBAR_H - fh) / 2, display_dir, TEXT_COLOR);
    }

    // ---- File list ----
    int list_bottom = H - INFO_H - PROGRESS_H - TRANSPORT_H;
    int visible_items = (list_bottom - LIST_TOP) / LIST_ITEM_H;

    px_hline(pixels, W, H, 0, list_bottom, W, BORDER_COLOR);

    for (int i = 0; i < visible_items && (i + g.scroll_y) < g.file_count; i++) {
        int idx = i + g.scroll_y;
        int iy = LIST_TOP + i * LIST_ITEM_H;

        // Highlight
        Color bg = BG_COLOR;
        if (idx == g.current_track && g.play_state != PlayState::Stopped)
            bg = LIST_PLAYING;
        else if (idx == g.hovered_item)
            bg = LIST_HOVER;

        if (bg.r != BG_COLOR.r || bg.g != BG_COLOR.g || bg.b != BG_COLOR.b)
            px_fill(pixels, W, H, 0, iy, W, LIST_ITEM_H, bg);

        // Playing indicator
        if (idx == g.current_track && g.play_state == PlayState::Playing) {
            px_triangle_right(pixels, W, H, 8, iy + 8, 8, 12, ACCENT);
        } else if (idx == g.current_track && g.play_state == PlayState::Paused) {
            // Pause bars
            px_fill(pixels, W, H, 8, iy + 8, 3, 12, ACCENT);
            px_fill(pixels, W, H, 13, iy + 8, 3, 12, ACCENT);
        }

        // File name
        Color name_color = (idx == g.current_track && g.play_state != PlayState::Stopped)
                            ? ACCENT_DARK : TEXT_COLOR;
        px_text(pixels, W, H, 24, iy + (LIST_ITEM_H - fh_sm) / 2,
                g.files[idx].name, name_color, FONT_SIZE_SM);

        // Format tag
        const char* tag = g.files[idx].is_mp3 ? "MP3" : "WAV";
        int tw = text_w(tag, FONT_SIZE_SM);
        px_text(pixels, W, H, W - tw - 12, iy + (LIST_ITEM_H - fh_sm) / 2,
                tag, DIM_TEXT, FONT_SIZE_SM);
    }

    // Scrollbar
    if (g.file_count > visible_items && visible_items > 0) {
        int list_h = list_bottom - LIST_TOP;
        int sb_h = (visible_items * list_h) / g.file_count;
        if (sb_h < 20) sb_h = 20;
        int sb_y = LIST_TOP + (g.scroll_y * (list_h - sb_h)) / (g.file_count - visible_items);
        px_fill_rounded(pixels, W, H, W - 6, sb_y, 4, sb_h, 2, TRACK_BG);
    }

    // Empty state
    if (g.file_count == 0) {
        const char* msg = "No audio files found";
        int mw = text_w(msg);
        int cy = LIST_TOP + (list_bottom - LIST_TOP) / 2 - fh / 2;
        px_text(pixels, W, H, (W - mw) / 2, cy, msg, DIM_TEXT);
    }

    // ---- Now Playing info ----
    int info_y = list_bottom + 1;
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
                state_str, ACCENT, FONT_SIZE_SM);
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
            px_fill_rounded(pixels, W, H, bar_x, bar_y, fill, bar_h, 3, ACCENT);
        // Knob
        px_circle(pixels, W, H, bar_x + fill, bar_y + bar_h / 2, 5, ACCENT);
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
        Color bg = active ? ACCENT : BTN_BG;
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

static int list_bottom() {
    return g.win_h - INFO_H - PROGRESS_H - TRANSPORT_H;
}

static int visible_items() {
    return (list_bottom() - LIST_TOP) / LIST_ITEM_H;
}

static bool handle_click(int mx, int my) {
    int W = g.win_w;
    int lb = list_bottom();
    int info_y = lb + 1;
    int prog_y = info_y + INFO_H;
    int trans_y = prog_y + PROGRESS_H;

    // File list click
    if (my >= LIST_TOP && my < lb && mx >= 0 && mx < W) {
        int idx = (my - LIST_TOP) / LIST_ITEM_H + g.scroll_y;
        if (idx >= 0 && idx < g.file_count) {
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
            // Seek: restart track and skip ahead
            // For simplicity, we estimate the file offset from the progress ratio
            uint64_t target_samples = (uint64_t)rel * g.total_samples / bar_w;

            if (g.current_track >= 0 && g.files[g.current_track].is_mp3) {
                // MP3 seek: estimate byte offset from ratio
                uint64_t target_offset = (g.file_size * rel) / bar_w;
                g.mp3_offset = target_offset;
                g.played_samples = target_samples;
                g.pcm_buf_len = 0;
                g.pcm_buf_pos = 0;
                mp3dec_init(&g.mp3dec); // reset decoder for clean seek
            } else {
                // WAV seek: direct offset
                int bytes_per_sample = 2 * g.channels;
                g.wav_pos = target_samples * bytes_per_sample;
                if (g.wav_pos > g.wav_data_size) g.wav_pos = g.wav_data_size;
                g.played_samples = target_samples;
                g.pcm_buf_len = 0;
                g.pcm_buf_pos = 0;
            }
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
                if (g.play_state == PlayState::Stopped && g.current_track >= 0) {
                    start_track(g.current_track);
                } else if (g.play_state == PlayState::Stopped && g.file_count > 0) {
                    start_track(0);
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
            if (mx >= bx && mx < bx + btn_w) { next_track(); return true; }
        }
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

    // Parse arguments: accept a directory path or a file path
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
                    memcpy(g.dir_path, "0:/home", 8);
                }
            } else {
                int len = montauk::slen(args);
                if (len >= (int)sizeof(g.dir_path)) len = sizeof(g.dir_path) - 1;
                memcpy(g.dir_path, args, len);
                g.dir_path[len] = '\0';
            }
        } else {
            memcpy(g.dir_path, "0:/home", 8);
        }
    }

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
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;

        bool redraw = false;

        if (r > 0) {
            if (ev.type == 3) break; // close

            // Keyboard
            if (ev.type == 0 && ev.key.pressed) {
                if (ev.key.scancode == 0x01) break; // Escape
                if (ev.key.ascii == ' ') {
                    if (g.play_state == PlayState::Stopped && g.file_count > 0)
                        start_track(g.current_track >= 0 ? g.current_track : 0);
                    else
                        toggle_pause();
                    redraw = true;
                }
                if (ev.key.ascii == 's' || ev.key.ascii == 'S') {
                    stop_playback();
                    redraw = true;
                }
                if (ev.key.scancode == 0x4D) { // Right arrow
                    next_track();
                    redraw = true;
                }
                if (ev.key.scancode == 0x4B) { // Left arrow
                    prev_track();
                    redraw = true;
                }
            }

            // Mouse click
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

                    if (g.current_track >= 0 && g.files[g.current_track].is_mp3) {
                        uint64_t target_offset = (g.file_size * rel) / bar_w;
                        g.mp3_offset = target_offset;
                        g.played_samples = target_samples;
                        g.pcm_buf_len = 0;
                        mp3dec_init(&g.mp3dec);
                    } else {
                        int bytes_per_sample = 2 * g.channels;
                        g.wav_pos = target_samples * bytes_per_sample;
                        if (g.wav_pos > g.wav_data_size) g.wav_pos = g.wav_data_size;
                        g.played_samples = target_samples;
                        g.pcm_buf_len = 0;
                    }
                    redraw = true;
                }

                // Hover tracking for file list
                int lb = list_bottom();
                if (ev.mouse.y >= LIST_TOP && ev.mouse.y < lb) {
                    int new_hover = (ev.mouse.y - LIST_TOP) / LIST_ITEM_H + g.scroll_y;
                    if (new_hover >= g.file_count) new_hover = -1;
                    if (new_hover != g.hovered_item) {
                        g.hovered_item = new_hover;
                        redraw = true;
                    }
                } else if (g.hovered_item != -1) {
                    g.hovered_item = -1;
                    redraw = true;
                }

                // Scroll
                if (ev.mouse.scroll != 0) {
                    g.scroll_y -= ev.mouse.scroll * 2;
                    int max_scroll = g.file_count - visible_items();
                    if (max_scroll < 0) max_scroll = 0;
                    if (g.scroll_y < 0) g.scroll_y = 0;
                    if (g.scroll_y > max_scroll) g.scroll_y = max_scroll;
                    redraw = true;
                }
            }

            // Resize
            if (ev.type == 2) {
                g.win_w = ev.resize.w;
                g.win_h = ev.resize.h;
                pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g.win_w, g.win_h);
                redraw = true;
            }
        }

        // Feed audio data to the device
        feed_audio();

        // Periodic redraw while playing (update time display)
        uint64_t now = montauk::get_milliseconds();
        if (g.play_state == PlayState::Playing && now - last_render >= 250) {
            redraw = true;
        }

        if (redraw) {
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
