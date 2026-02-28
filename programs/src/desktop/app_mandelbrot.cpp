/*
    * app_mandelbrot.cpp
    * MontaukOS Desktop - Mandelbrot set visualizer
    * Supports zoom (scroll wheel), pan (drag), and reset (R key)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Fixed-point Mandelbrot (avoids FPU/SSE dependency issues)
// Uses 28.36 fixed-point: 36 fractional bits gives ~1e-10 precision
// ============================================================================

using fp_t = int64_t;
static constexpr int FP_SHIFT = 36;
static constexpr fp_t FP_ONE = (fp_t)1 << FP_SHIFT;

static inline fp_t fp_from_int(int v) { return (fp_t)v << FP_SHIFT; }

static inline fp_t fp_mul(fp_t a, fp_t b) {
    // Split to avoid overflow: a * b >> SHIFT
    // Use 128-bit intermediate via compiler builtin
    __int128 r = (__int128)a * b;
    return (fp_t)(r >> FP_SHIFT);
}

// fp_div for small numerators (avoids __divti3 by using 64-bit division)
// Only valid when a is small enough that a << FP_SHIFT fits in int64_t
static inline fp_t fp_div_small(int a, int b) {
    return ((fp_t)a << FP_SHIFT) / (fp_t)b;
}

// ============================================================================
// State
// ============================================================================

static constexpr int MB_MAX_ITER = 256;
static constexpr int MB_TOOLBAR_H = 32;

struct MandelbrotState {
    DesktopState* desktop;
    fp_t center_x, center_y;   // center of view in fractal coords
    fp_t scale;                 // units per pixel (fixed-point)
    int max_iter;
    bool needs_render;
    // Drag state
    bool dragging;
    int drag_start_x, drag_start_y;
    fp_t drag_start_cx, drag_start_cy;
};

// ============================================================================
// Color palette
// ============================================================================

static uint32_t mandelbrot_color(int iter, int max_iter) {
    if (iter >= max_iter) return 0xFF000000; // black for inside set

    // Smooth cycling palette using bit manipulation
    int t = (iter * 7) & 0xFF;
    int phase = (iter * 7) >> 8;

    uint8_t r, g, b;
    switch (phase % 6) {
    case 0: r = 255; g = (uint8_t)t; b = 0; break;
    case 1: r = (uint8_t)(255 - t); g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = (uint8_t)t; break;
    case 3: r = 0; g = (uint8_t)(255 - t); b = 255; break;
    case 4: r = (uint8_t)t; g = 0; b = 255; break;
    default: r = 255; g = 0; b = (uint8_t)(255 - t); break;
    }

    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ============================================================================
// Render
// ============================================================================

static void mandelbrot_render(MandelbrotState* mb, uint32_t* pixels, int w, int h) {
    int render_h = h - MB_TOOLBAR_H;
    if (render_h <= 0) return;

    fp_t half_w = fp_mul(fp_from_int(w / 2), mb->scale);
    fp_t half_h = fp_mul(fp_from_int(render_h / 2), mb->scale);
    fp_t x_min = mb->center_x - half_w;
    fp_t y_min = mb->center_y - half_h;
    fp_t bailout = fp_from_int(4);

    for (int py = 0; py < render_h; py++) {
        fp_t ci = y_min + fp_mul(fp_from_int(py), mb->scale);
        uint32_t* row = pixels + (py + MB_TOOLBAR_H) * w;

        for (int px = 0; px < w; px++) {
            fp_t cr = x_min + fp_mul(fp_from_int(px), mb->scale);

            fp_t zr = 0, zi = 0;
            int iter = 0;

            while (iter < mb->max_iter) {
                fp_t zr2 = fp_mul(zr, zr);
                fp_t zi2 = fp_mul(zi, zi);
                if (zr2 + zi2 > bailout) break;
                fp_t new_zr = zr2 - zi2 + cr;
                zi = fp_mul(fp_from_int(2), fp_mul(zr, zi)) + ci;
                zr = new_zr;
                iter++;
            }

            row[px] = mandelbrot_color(iter, mb->max_iter);
        }
    }

    mb->needs_render = false;
}

// ============================================================================
// Callbacks
// ============================================================================

static void mb_on_draw(Window* win, Framebuffer& fb) {
    MandelbrotState* mb = (MandelbrotState*)win->app_data;
    if (!mb) return;

    Canvas c(win);

    if (mb->needs_render) {
        mandelbrot_render(mb, win->content, c.w, c.h);
    }

    // Draw toolbar over the render
    c.fill_rect(0, 0, c.w, MB_TOOLBAR_H, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.hline(0, MB_TOOLBAR_H - 1, c.w, colors::BORDER);

    // Reset button
    c.button(8, 4, 60, 24, "Reset", colors::ACCENT, colors::WHITE, 4);

    // Iter +/- buttons
    char iter_str[24];
    snprintf(iter_str, sizeof(iter_str), "Iter: %d", mb->max_iter);
    int fh = system_font_height();
    c.text(80, (MB_TOOLBAR_H - fh) / 2, iter_str, colors::TEXT_COLOR);

    int iter_text_w = text_width(iter_str);
    int btn_x = 80 + iter_text_w + 8;
    c.button(btn_x, 4, 24, 24, "-", Color::from_rgb(0xAA, 0xAA, 0xAA), colors::WHITE, 4);
    c.button(btn_x + 28, 4, 24, 24, "+", Color::from_rgb(0xAA, 0xAA, 0xAA), colors::WHITE, 4);

    // Hint
    const char* hint = "Scroll=zoom  Drag=pan";
    int hw = text_width(hint);
    c.text(c.w - hw - 8, (MB_TOOLBAR_H - fh) / 2, hint, Color::from_rgb(0x99, 0x99, 0x99));
}

static void mb_on_mouse(Window* win, MouseEvent& ev) {
    MandelbrotState* mb = (MandelbrotState*)win->app_data;
    if (!mb) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    // Handle toolbar clicks
    if (ev.left_pressed() && ly < MB_TOOLBAR_H) {
        // Reset button
        Rect reset_r = {8, 4, 60, 24};
        if (reset_r.contains(lx, ly)) {
            mb->center_x = -fp_from_int(1) / 2; // -0.5
            mb->center_y = 0;
            mb->scale = fp_div_small(3, cr.w > 0 ? cr.w : 400);
            mb->max_iter = MB_MAX_ITER;
            mb->needs_render = true;
            return;
        }

        // Iter buttons
        char iter_str[24];
        snprintf(iter_str, sizeof(iter_str), "Iter: %d", mb->max_iter);
        int iter_text_w = text_width(iter_str);
        int btn_x = 80 + iter_text_w + 8;

        Rect minus_r = {btn_x, 4, 24, 24};
        Rect plus_r = {btn_x + 28, 4, 24, 24};

        if (minus_r.contains(lx, ly)) {
            if (mb->max_iter > 32) { mb->max_iter /= 2; mb->needs_render = true; }
            return;
        }
        if (plus_r.contains(lx, ly)) {
            if (mb->max_iter < 4096) { mb->max_iter *= 2; mb->needs_render = true; }
            return;
        }
        return;
    }

    // Drag panning
    if (ev.left_pressed() && ly >= MB_TOOLBAR_H) {
        mb->dragging = true;
        mb->drag_start_x = lx;
        mb->drag_start_y = ly;
        mb->drag_start_cx = mb->center_x;
        mb->drag_start_cy = mb->center_y;
        return;
    }

    if (mb->dragging && ev.left_held()) {
        int dx = lx - mb->drag_start_x;
        int dy = ly - mb->drag_start_y;
        mb->center_x = mb->drag_start_cx - fp_mul(fp_from_int(dx), mb->scale);
        mb->center_y = mb->drag_start_cy - fp_mul(fp_from_int(dy), mb->scale);
        mb->needs_render = true;
        return;
    }

    if (mb->dragging && !ev.left_held()) {
        mb->dragging = false;
    }

    // Scroll zoom (centered on mouse position)
    if (ev.scroll != 0 && ly >= MB_TOOLBAR_H) {
        int render_h = cr.h - MB_TOOLBAR_H;
        // Mouse position in fractal coords before zoom
        fp_t mx_frac = mb->center_x + fp_mul(fp_from_int(lx - cr.w / 2), mb->scale);
        fp_t my_frac = mb->center_y + fp_mul(fp_from_int((ly - MB_TOOLBAR_H) - render_h / 2), mb->scale);

        if (ev.scroll < 0) {
            // Zoom in
            mb->scale = fp_mul(mb->scale, fp_from_int(3) / 4); // * 0.75
        } else {
            // Zoom out
            mb->scale = fp_mul(mb->scale, fp_from_int(4) / 3); // * 1.33
        }

        // Adjust center so mouse stays at same fractal point
        fp_t new_mx = mb->center_x + fp_mul(fp_from_int(lx - cr.w / 2), mb->scale);
        fp_t new_my = mb->center_y + fp_mul(fp_from_int((ly - MB_TOOLBAR_H) - render_h / 2), mb->scale);
        mb->center_x += mx_frac - new_mx;
        mb->center_y += my_frac - new_my;

        mb->needs_render = true;
    }
}

static void mb_on_key(Window* win, const Montauk::KeyEvent& key) {
    MandelbrotState* mb = (MandelbrotState*)win->app_data;
    if (!mb || !key.pressed) return;

    if (key.ascii == 'r' || key.ascii == 'R') {
        Rect cr = win->content_rect();
        mb->center_x = -fp_from_int(1) / 2;
        mb->center_y = 0;
        mb->scale = fp_div_small(3, cr.w > 0 ? cr.w : 400);
        mb->max_iter = MB_MAX_ITER;
        mb->needs_render = true;
    } else if (key.ascii == '+' || key.ascii == '=') {
        if (mb->max_iter < 4096) { mb->max_iter *= 2; mb->needs_render = true; }
    } else if (key.ascii == '-') {
        if (mb->max_iter > 32) { mb->max_iter /= 2; mb->needs_render = true; }
    }
}

static void mb_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Mandelbrot launcher
// ============================================================================

void open_mandelbrot(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Mandelbrot", 120, 60, 500, 400);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    Rect cr = win->content_rect();

    MandelbrotState* mb = (MandelbrotState*)montauk::malloc(sizeof(MandelbrotState));
    montauk::memset(mb, 0, sizeof(MandelbrotState));
    mb->desktop = ds;
    mb->center_x = -fp_from_int(1) / 2; // -0.5
    mb->center_y = 0;
    mb->scale = fp_div_small(3, cr.w > 0 ? cr.w : 400);
    mb->max_iter = MB_MAX_ITER;
    mb->needs_render = true;
    mb->dragging = false;

    win->app_data = mb;
    win->on_draw = mb_on_draw;
    win->on_mouse = mb_on_mouse;
    win->on_key = mb_on_key;
    win->on_close = mb_on_close;
}
