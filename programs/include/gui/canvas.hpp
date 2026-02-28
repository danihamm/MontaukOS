/*
    * canvas.hpp
    * MontaukOS Canvas — drawing primitives for pixel buffer (uint32_t*) targets
    * Mirrors Framebuffer API but operates directly on app content buffers.
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/font.hpp"
#include "gui/svg.hpp"
#include "gui/window.hpp"

namespace gui {

struct Canvas {
    uint32_t* pixels;
    int w, h;

    // ---- Constructors ----

    Canvas(uint32_t* px, int width, int height)
        : pixels(px), w(width), h(height) {}

    Canvas(Window* win)
        : pixels(win->content), w(win->content_w), h(win->content_h) {}

    // ---- Core drawing ----

    void fill(Color c) {
        uint32_t px = c.to_pixel();
        int total = w * h;
        for (int i = 0; i < total; i++) pixels[i] = px;
    }

    void put_pixel(int x, int y, Color c) {
        if (x >= 0 && x < w && y >= 0 && y < h)
            pixels[y * w + x] = c.to_pixel();
    }

    void fill_rect(int x, int y, int rw, int rh, Color c) {
        uint32_t px = c.to_pixel();
        int x0 = gui_max(x, 0), y0 = gui_max(y, 0);
        int x1 = gui_min(x + rw, w), y1 = gui_min(y + rh, h);
        for (int dy = y0; dy < y1; dy++)
            for (int dx = x0; dx < x1; dx++)
                pixels[dy * w + dx] = px;
    }

    void fill_rounded_rect(int x, int y, int rw, int rh, int radius, Color c) {
        if (radius <= 0) { fill_rect(x, y, rw, rh, c); return; }
        uint32_t px = c.to_pixel();
        for (int row = 0; row < rh; row++) {
            int dy = y + row;
            if (dy < 0 || dy >= h) continue;
            for (int col = 0; col < rw; col++) {
                int dx = x + col;
                if (dx < 0 || dx >= w) continue;
                bool in_corner = false;
                int cx_off = 0, cy_off = 0;
                if (col < radius && row < radius) {
                    cx_off = radius - col; cy_off = radius - row; in_corner = true;
                } else if (col >= rw - radius && row < radius) {
                    cx_off = col - (rw - radius - 1); cy_off = radius - row; in_corner = true;
                } else if (col < radius && row >= rh - radius) {
                    cx_off = radius - col; cy_off = row - (rh - radius - 1); in_corner = true;
                } else if (col >= rw - radius && row >= rh - radius) {
                    cx_off = col - (rw - radius - 1); cy_off = row - (rh - radius - 1); in_corner = true;
                }
                if (in_corner && cx_off * cx_off + cy_off * cy_off > radius * radius) continue;
                pixels[dy * w + dx] = px;
            }
        }
    }

    void hline(int x, int y, int len, Color c) {
        if (y < 0 || y >= h) return;
        uint32_t px = c.to_pixel();
        int x0 = gui_max(x, 0), x1 = gui_min(x + len, w);
        for (int dx = x0; dx < x1; dx++)
            pixels[y * w + dx] = px;
    }

    void vline(int x, int y, int len, Color c) {
        if (x < 0 || x >= w) return;
        uint32_t px = c.to_pixel();
        int y0 = gui_max(y, 0), y1 = gui_min(y + len, h);
        for (int dy = y0; dy < y1; dy++)
            pixels[dy * w + x] = px;
    }

    void rect(int x, int y, int rw, int rh, Color c) {
        hline(x, y, rw, c);
        hline(x, y + rh - 1, rw, c);
        vline(x, y, rh, c);
        vline(x + rw - 1, y, rh, c);
    }

    // ---- Text ----

    void text(int x, int y, const char* str, Color c) {
        if (fonts::system_font && fonts::system_font->valid) {
            fonts::system_font->draw_to_buffer(pixels, w, h, x, y, str, c, fonts::UI_SIZE);
            return;
        }
        uint32_t px = c.to_pixel();
        for (int i = 0; str[i] && x + (i + 1) * FONT_WIDTH <= w; i++) {
            const uint8_t* glyph = &font_data[(unsigned char)str[i] * FONT_HEIGHT];
            int cx = x + i * FONT_WIDTH;
            for (int fy = 0; fy < FONT_HEIGHT && y + fy < h; fy++) {
                uint8_t bits = glyph[fy];
                for (int fx = 0; fx < FONT_WIDTH; fx++) {
                    if (bits & (0x80 >> fx)) {
                        int dx = cx + fx;
                        int dy = y + fy;
                        if (dx >= 0 && dx < w && dy >= 0)
                            pixels[dy * w + dx] = px;
                    }
                }
            }
        }
    }

    void text_2x(int x, int y, const char* str, Color c) {
        if (fonts::system_font && fonts::system_font->valid) {
            fonts::system_font->draw_to_buffer(pixels, w, h, x, y, str, c, fonts::LARGE_SIZE);
            return;
        }
        uint32_t px = c.to_pixel();
        for (int i = 0; str[i] && x + (i + 1) * FONT_WIDTH * 2 <= w; i++) {
            const uint8_t* glyph = &font_data[(unsigned char)str[i] * FONT_HEIGHT];
            int cx = x + i * FONT_WIDTH * 2;
            for (int fy = 0; fy < FONT_HEIGHT; fy++) {
                uint8_t bits = glyph[fy];
                for (int fx = 0; fx < FONT_WIDTH; fx++) {
                    if (bits & (0x80 >> fx)) {
                        int dx = cx + fx * 2;
                        int dy = y + fy * 2;
                        for (int sy = 0; sy < 2; sy++)
                            for (int sx = 0; sx < 2; sx++) {
                                int pdx = dx + sx;
                                int pdy = dy + sy;
                                if (pdx >= 0 && pdx < w && pdy >= 0 && pdy < h)
                                    pixels[pdy * w + pdx] = px;
                            }
                    }
                }
            }
        }
    }

    void text_mono(int x, int y, const char* str, Color c) {
        if (fonts::mono && fonts::mono->valid) {
            fonts::mono->draw_to_buffer(pixels, w, h, x, y, str, c, fonts::TERM_SIZE);
            return;
        }
        text(x, y, str, c);
    }

    // ---- Icons ----

    void icon(int x, int y, const SvgIcon& ic) {
        if (!ic.pixels) return;
        for (int row = 0; row < ic.height; row++) {
            int dy = y + row;
            if (dy < 0 || dy >= h) continue;
            for (int col = 0; col < ic.width; col++) {
                int dx = x + col;
                if (dx < 0 || dx >= w) continue;
                uint32_t src = ic.pixels[row * ic.width + col];
                uint8_t sa = (src >> 24) & 0xFF;
                if (sa == 0) continue;
                if (sa == 255) {
                    pixels[dy * w + dx] = src;
                } else {
                    uint32_t dst = pixels[dy * w + dx];
                    uint8_t sr = (src >> 16) & 0xFF;
                    uint8_t sg = (src >> 8) & 0xFF;
                    uint8_t sb = src & 0xFF;
                    uint8_t dr = (dst >> 16) & 0xFF;
                    uint8_t dg = (dst >> 8) & 0xFF;
                    uint8_t db = dst & 0xFF;
                    uint32_t a = sa, inv_a = 255 - sa;
                    uint32_t rr = (a * sr + inv_a * dr + 128) / 255;
                    uint32_t gg = (a * sg + inv_a * dg + 128) / 255;
                    uint32_t bb = (a * sb + inv_a * db + 128) / 255;
                    pixels[dy * w + dx] = 0xFF000000 | (rr << 16) | (gg << 8) | bb;
                }
            }
        }
    }

    // ---- High-level helpers ----

    void kv_line(int x, int* y, const char* line, Color c, int line_h = 0) {
        if (line_h == 0) line_h = system_font_height() + 6;
        text(x, *y, line, c);
        *y += line_h;
    }

    void separator(int x_start, int x_end, int* y, Color c, int spacing = 8) {
        hline(x_start, *y, x_end - x_start, c);
        *y += spacing;
    }

    void button(int x, int y, int bw, int bh, const char* label,
                Color bg, Color fg, int radius = 4) {
        fill_rounded_rect(x, y, bw, bh, radius, bg);
        int tw = text_width(label);
        int fh = system_font_height();
        int tx = x + (bw - tw) / 2;
        int ty = y + (bh - fh) / 2;
        text(tx, ty, label, fg);
    }
};

} // namespace gui
