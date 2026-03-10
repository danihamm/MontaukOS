/*
 * main.cpp
 * MontaukOS Volume Control
 * Copyright (c) 2026 Daniel Hammer
 */

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

static constexpr int WIN_W        = 280;
static constexpr int WIN_H        = 164;
static constexpr int FONT_SIZE    = 16;
static constexpr int FONT_SIZE_LG = 28;

static constexpr int SLIDER_X     = 24;
static constexpr int SLIDER_W     = WIN_W - 48;
static constexpr int SLIDER_Y     = 78;
static constexpr int SLIDER_H     = 8;
static constexpr int KNOB_R       = 10;

static constexpr int BTN_W        = 48;
static constexpr int BTN_H        = 28;
static constexpr int BTN_Y        = 118;
static constexpr int BTN_RAD      = 6;

static constexpr Color BG_COLOR     = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TEXT_COLOR   = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color DIM_TEXT     = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color ACCENT       = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color ACCENT_HOVER = Color::from_rgb(0x2A, 0x62, 0xC8);
static constexpr Color TRACK_BG     = Color::from_rgb(0xDD, 0xDD, 0xDD);
static constexpr Color WHITE        = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color MUTE_COLOR   = Color::from_rgb(0xCC, 0x33, 0x33);

// ============================================================================
// State
// ============================================================================

static TrueTypeFont* g_font = nullptr;
static int g_volume = 80;
static bool g_muted = false;
static int g_pre_mute_vol = 80;
static bool g_dragging = false;

// ============================================================================
// Pixel helpers (same pattern as disks app)
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
            if (col < r && row < r) { cx = r - col - 1; cy = r - row - 1; if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col >= w - r && row < r) { cx = col - (w - r); cy = r - row - 1; if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col < r && row >= h - r) { cx = r - col - 1; cy = row - (h - r); if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col >= w - r && row >= h - r) { cx = col - (w - r); cy = row - (h - r); if (cx*cx + cy*cy >= r*r) skip = true; }
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

static void px_text(uint32_t* px, int bw, int bh,
                    int x, int y, const char* text, Color c, int size = FONT_SIZE) {
    if (g_font)
        g_font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}

static int text_w(const char* text, int size = FONT_SIZE) {
    return g_font ? g_font->measure_text(text, size) : 0;
}

static int font_h(int size = FONT_SIZE) {
    if (!g_font) return 16;
    auto* cache = g_font->get_cache(size);
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

// ============================================================================
// Volume helpers
// ============================================================================

static void apply_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_volume = vol;
    montauk::audio_set_volume(0, g_volume);
}

static void refresh_volume() {
    int v = montauk::audio_get_volume(0);
    if (v >= 0) g_volume = v;
}

// ============================================================================
// Render
// ============================================================================

static void render(uint32_t* pixels) {
    int fh_lg = font_h(FONT_SIZE_LG);

    // Background
    px_fill(pixels, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, BG_COLOR);

    // Volume percentage (large, centered)
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", g_muted ? 0 : g_volume);
    int vw = text_w(vol_str, FONT_SIZE_LG);
    Color vol_color = g_muted ? MUTE_COLOR : ACCENT;
    px_text(pixels, WIN_W, WIN_H, (WIN_W - vw) / 2, 20, vol_str, vol_color, FONT_SIZE_LG);

    // "Muted" label
    if (g_muted) {
        const char* muted_label = "Muted";
        int mw = text_w(muted_label);
        px_text(pixels, WIN_W, WIN_H, (WIN_W - mw) / 2,
                20 + fh_lg + 4, muted_label, MUTE_COLOR);
    }

    // Slider track
    px_fill_rounded(pixels, WIN_W, WIN_H, SLIDER_X, SLIDER_Y, SLIDER_W, SLIDER_H, 4, TRACK_BG);

    // Filled portion
    int display_vol = g_muted ? 0 : g_volume;
    int fill_w = (display_vol * SLIDER_W) / 100;
    if (fill_w > 0)
        px_fill_rounded(pixels, WIN_W, WIN_H, SLIDER_X, SLIDER_Y, fill_w, SLIDER_H, 4, ACCENT);

    // Knob
    int knob_x = SLIDER_X + fill_w;
    int knob_y = SLIDER_Y + SLIDER_H / 2;
    px_circle(pixels, WIN_W, WIN_H, knob_x, knob_y, KNOB_R, ACCENT);
    px_circle(pixels, WIN_W, WIN_H, knob_x, knob_y, KNOB_R - 3, WHITE);

    // Buttons: [-] and [+] and [Mute]
    int total_btn_w = BTN_W * 2 + 60 + 12 * 2;  // minus, plus, mute + gaps
    int bx = (WIN_W - total_btn_w) / 2;

    px_button(pixels, WIN_W, WIN_H, bx, BTN_Y, BTN_W, BTN_H,
              "-", Color::from_rgb(0xE0, 0xE0, 0xE0), TEXT_COLOR, BTN_RAD);
    bx += BTN_W + 12;

    px_button(pixels, WIN_W, WIN_H, bx, BTN_Y, BTN_W, BTN_H,
              "+", Color::from_rgb(0xE0, 0xE0, 0xE0), TEXT_COLOR, BTN_RAD);
    bx += BTN_W + 12;

    Color mute_bg = g_muted ? MUTE_COLOR : Color::from_rgb(0xE0, 0xE0, 0xE0);
    Color mute_fg = g_muted ? WHITE : TEXT_COLOR;
    px_button(pixels, WIN_W, WIN_H, bx, BTN_Y, 60, BTN_H,
              "Mute", mute_bg, mute_fg, BTN_RAD);
}

// ============================================================================
// Hit testing
// ============================================================================

static int vol_from_slider_x(int mx) {
    int v = ((mx - SLIDER_X) * 100) / SLIDER_W;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static bool handle_click(int mx, int my) {
    // Slider area (generous vertical hit zone)
    if (my >= SLIDER_Y - KNOB_R && my <= SLIDER_Y + SLIDER_H + KNOB_R &&
        mx >= SLIDER_X - KNOB_R && mx <= SLIDER_X + SLIDER_W + KNOB_R) {
        g_muted = false;
        apply_volume(vol_from_slider_x(mx));
        g_dragging = true;
        return true;
    }

    // Buttons
    int total_btn_w = BTN_W * 2 + 60 + 12 * 2;
    int bx = (WIN_W - total_btn_w) / 2;

    if (my >= BTN_Y && my < BTN_Y + BTN_H) {
        // [-] button
        if (mx >= bx && mx < bx + BTN_W) {
            g_muted = false;
            apply_volume(g_volume - 5);
            return true;
        }
        bx += BTN_W + 12;

        // [+] button
        if (mx >= bx && mx < bx + BTN_W) {
            g_muted = false;
            apply_volume(g_volume + 5);
            return true;
        }
        bx += BTN_W + 12;

        // [Mute] button
        if (mx >= bx && mx < bx + 60) {
            if (g_muted) {
                g_muted = false;
                apply_volume(g_pre_mute_vol);
            } else {
                g_pre_mute_vol = g_volume;
                g_muted = true;
                montauk::audio_set_volume(0, 0);
            }
            return true;
        }
    }

    return false;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Load font
    {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (f) {
            montauk::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init("0:/fonts/Roboto-Medium.ttf")) { montauk::mfree(f); f = nullptr; }
        }
        g_font = f;
    }

    refresh_volume();

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Volume", WIN_W, WIN_H, &wres) < 0 || wres.id < 0)
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

        bool redraw = false;

        if (ev.type == 3) break; // close

        // Keyboard
        if (ev.type == 0 && ev.key.pressed) {
            if (ev.key.scancode == 0x01) break; // Escape
            if (ev.key.scancode == 0x4D || ev.key.ascii == '+' || ev.key.ascii == '=') { // Right / +
                g_muted = false;
                apply_volume(g_volume + 5);
                redraw = true;
            } else if (ev.key.scancode == 0x4B || ev.key.ascii == '-') { // Left / -
                g_muted = false;
                apply_volume(g_volume - 5);
                redraw = true;
            } else if (ev.key.ascii == 'm' || ev.key.ascii == 'M') {
                if (g_muted) {
                    g_muted = false;
                    apply_volume(g_pre_mute_vol);
                } else {
                    g_pre_mute_vol = g_volume;
                    g_muted = true;
                    montauk::audio_set_volume(0, 0);
                }
                redraw = true;
            }
        }

        // Mouse
        if (ev.type == 1) {
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
            bool released = !(ev.mouse.buttons & 1) && (ev.mouse.prev_buttons & 1);

            if (clicked) {
                if (handle_click(ev.mouse.x, ev.mouse.y))
                    redraw = true;
            }

            if (g_dragging && (ev.mouse.buttons & 1)) {
                g_muted = false;
                apply_volume(vol_from_slider_x(ev.mouse.x));
                redraw = true;
            }

            if (released) {
                g_dragging = false;
            }
        }

        if (redraw) {
            render(pixels);
            montauk::win_present(win_id);
        }
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
