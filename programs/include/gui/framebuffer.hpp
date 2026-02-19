/*
    * framebuffer.hpp
    * ZenithOS double-buffered framebuffer abstraction
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <zenith/syscall.h>
#include "gui/gui.hpp"

namespace gui {

class Framebuffer {
    uint32_t* hw_fb;
    uint32_t* back_buf;
    int fb_width;
    int fb_height;
    int fb_pitch; // in bytes

public:
    Framebuffer() : hw_fb(nullptr), back_buf(nullptr), fb_width(0), fb_height(0), fb_pitch(0) {
        Zenith::FbInfo info;
        zenith::fb_info(&info);

        fb_width  = (int)info.width;
        fb_height = (int)info.height;
        fb_pitch  = (int)info.pitch;

        hw_fb = (uint32_t*)zenith::fb_map();
        back_buf = (uint32_t*)zenith::alloc((uint64_t)fb_height * fb_pitch);
    }

    int width() const { return fb_width; }
    int height() const { return fb_height; }
    int pitch() const { return fb_pitch; }

    uint32_t* buffer() { return back_buf; }

    inline void put_pixel(int x, int y, Color c) {
        if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
        uint32_t* row = (uint32_t*)((uint8_t*)back_buf + y * fb_pitch);
        row[x] = c.to_pixel();
    }

    inline void put_pixel_alpha(int x, int y, Color c) {
        if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;
        if (c.a == 0) return;
        if (c.a == 255) {
            put_pixel(x, y, c);
            return;
        }

        uint32_t* row = (uint32_t*)((uint8_t*)back_buf + y * fb_pitch);
        uint32_t dst = row[x];

        uint8_t dr = (dst >> 16) & 0xFF;
        uint8_t dg = (dst >> 8) & 0xFF;
        uint8_t db = dst & 0xFF;

        uint32_t a = c.a;
        uint32_t inv_a = 255 - a;

        // Fast alpha blend: out = (src * alpha + dst * (255 - alpha) + 128) / 255
        // Approximation: (x + 1 + (x >> 8)) >> 8 for division by 255
        uint32_t rr = a * c.r + inv_a * dr;
        uint32_t gg = a * c.g + inv_a * dg;
        uint32_t bb = a * c.b + inv_a * db;

        rr = (rr + 1 + (rr >> 8)) >> 8;
        gg = (gg + 1 + (gg >> 8)) >> 8;
        bb = (bb + 1 + (bb >> 8)) >> 8;

        row[x] = (0xFF000000) | (rr << 16) | (gg << 8) | bb;
    }

    inline void fill_rect(int x, int y, int w, int h, Color c) {
        // Clip to screen bounds
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = (x + w) > fb_width ? fb_width : (x + w);
        int y1 = (y + h) > fb_height ? fb_height : (y + h);

        if (x0 >= x1 || y0 >= y1) return;

        uint32_t pixel = c.to_pixel();
        int clipped_w = x1 - x0;

        for (int row = y0; row < y1; row++) {
            uint32_t* dst = (uint32_t*)((uint8_t*)back_buf + row * fb_pitch) + x0;
            for (int col = 0; col < clipped_w; col++) {
                dst[col] = pixel;
            }
        }
    }

    inline void fill_rect_alpha(int x, int y, int w, int h, Color c) {
        if (c.a == 0) return;
        if (c.a == 255) {
            fill_rect(x, y, w, h, c);
            return;
        }

        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = (x + w) > fb_width ? fb_width : (x + w);
        int y1 = (y + h) > fb_height ? fb_height : (y + h);

        if (x0 >= x1 || y0 >= y1) return;

        uint32_t a = c.a;
        uint32_t inv_a = 255 - a;
        uint32_t src_r = a * c.r;
        uint32_t src_g = a * c.g;
        uint32_t src_b = a * c.b;

        for (int row = y0; row < y1; row++) {
            uint32_t* dst = (uint32_t*)((uint8_t*)back_buf + row * fb_pitch) + x0;
            for (int col = 0; col < x1 - x0; col++) {
                uint32_t d = dst[col];
                uint32_t dr = (d >> 16) & 0xFF;
                uint32_t dg = (d >> 8) & 0xFF;
                uint32_t db = d & 0xFF;

                uint32_t rr = src_r + inv_a * dr;
                uint32_t gg = src_g + inv_a * dg;
                uint32_t bb = src_b + inv_a * db;

                rr = (rr + 1 + (rr >> 8)) >> 8;
                gg = (gg + 1 + (gg >> 8)) >> 8;
                bb = (bb + 1 + (bb >> 8)) >> 8;

                dst[col] = (0xFF000000) | (rr << 16) | (gg << 8) | bb;
            }
        }
    }

    inline void blit(int x, int y, int w, int h, const uint32_t* pixels) {
        for (int row = 0; row < h; row++) {
            int dy = y + row;
            if (dy < 0 || dy >= fb_height) continue;
            for (int col = 0; col < w; col++) {
                int dx = x + col;
                if (dx < 0 || dx >= fb_width) continue;
                uint32_t* dst_row = (uint32_t*)((uint8_t*)back_buf + dy * fb_pitch);
                dst_row[dx] = pixels[row * w + col];
            }
        }
    }

    inline void blit_alpha(int x, int y, int w, int h, const uint32_t* pixels) {
        for (int row = 0; row < h; row++) {
            int dy = y + row;
            if (dy < 0 || dy >= fb_height) continue;
            for (int col = 0; col < w; col++) {
                int dx = x + col;
                if (dx < 0 || dx >= fb_width) continue;

                uint32_t src = pixels[row * w + col];
                uint8_t sa = (src >> 24) & 0xFF;
                if (sa == 0) continue;

                uint8_t sr = (src >> 16) & 0xFF;
                uint8_t sg = (src >> 8) & 0xFF;
                uint8_t sb = src & 0xFF;

                if (sa == 255) {
                    uint32_t* dst_row = (uint32_t*)((uint8_t*)back_buf + dy * fb_pitch);
                    dst_row[dx] = src;
                    continue;
                }

                Color sc = {sr, sg, sb, sa};
                put_pixel_alpha(dx, dy, sc);
            }
        }
    }

    inline void clear(Color c) {
        fill_rect(0, 0, fb_width, fb_height, c);
    }

    inline void flip() {
        // Copy back buffer to hardware framebuffer, row by row (pitch may differ)
        int row_pixels = fb_width;
        for (int y = 0; y < fb_height; y++) {
            uint32_t* src = (uint32_t*)((uint8_t*)back_buf + y * fb_pitch);
            uint32_t* dst = (uint32_t*)((uint8_t*)hw_fb + y * fb_pitch);
            for (int x = 0; x < row_pixels; x++) {
                dst[x] = src[x];
            }
        }
    }
};

} // namespace gui
