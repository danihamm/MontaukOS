/*
    * truetype.hpp
    * ZenithOS TrueType font rendering via stb_truetype
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <zenith/syscall.h>
#include <zenith/heap.h>
#include <zenith/string.h>
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"

// Forward-declare stbtt_fontinfo to avoid including stb_truetype.h in every TU.
// The actual struct is defined in stb_truetype.h and only used in truetype.hpp
// method bodies (which are inline but only instantiated where stb_truetype.h
// is also included — i.e. stb_truetype_impl.cpp). We include the full header
// here since our inline methods need the complete type.
#include "gui/stb_math.h"

// We need the stb macros defined before including stb_truetype.h (header-only mode)
#ifndef STBTT_ifloor
#define STBTT_ifloor(x)   ((int) stb_floor(x))
#define STBTT_iceil(x)    ((int) stb_ceil(x))
#define STBTT_sqrt(x)     stb_sqrt(x)
#define STBTT_pow(x,y)    stb_pow(x,y)
#define STBTT_fmod(x,y)   stb_fmod(x,y)
#define STBTT_cos(x)      stb_cos(x)
#define STBTT_acos(x)     stb_acos(x)
#define STBTT_fabs(x)     stb_fabs(x)
#define STBTT_malloc(x,u)  ((void)(u), zenith::malloc(x))
#define STBTT_free(x,u)    ((void)(u), zenith::mfree(x))
#define STBTT_memcpy(d,s,n) zenith::memcpy(d,s,n)
#define STBTT_memset(d,v,n) zenith::memset(d,v,n)
#define STBTT_strlen(x)    zenith::slen(x)
#define STBTT_assert(x)    ((void)(x))
#endif

#include "gui/stb_truetype.h"

namespace gui {

struct CachedGlyph {
    uint8_t* bitmap;
    int width, height;
    int xoff, yoff;
    int advance;
    bool loaded;
};

struct GlyphCache {
    CachedGlyph glyphs[256];  // Latin-1 Supplement (U+0080..U+00FF) included
    int pixel_size;
    float scale;
    int ascent, descent, line_gap;
    int line_height;
};

struct TrueTypeFont {
    stbtt_fontinfo info;
    uint8_t* data;
    GlyphCache caches[4];
    int cache_count;
    bool valid;

    bool init(const char* vfs_path) {
        valid = false;
        data = nullptr;
        cache_count = 0;

        int fd = zenith::open(vfs_path);
        if (fd < 0) return false;

        uint64_t size = zenith::getsize(fd);
        if (size == 0 || size > 1024 * 1024) {
            zenith::close(fd);
            return false;
        }

        data = (uint8_t*)zenith::alloc(size);
        if (!data) {
            zenith::close(fd);
            return false;
        }

        zenith::read(fd, data, 0, size);
        zenith::close(fd);

        if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
            zenith::free(data);
            data = nullptr;
            return false;
        }

        valid = true;
        return true;
    }

    GlyphCache* get_cache(int pixel_size) {
        // Search existing caches
        for (int i = 0; i < cache_count; i++) {
            if (caches[i].pixel_size == pixel_size)
                return &caches[i];
        }

        // Create new cache
        if (cache_count >= 4) return &caches[0]; // fallback to first

        GlyphCache* gc = &caches[cache_count++];
        gc->pixel_size = pixel_size;
        gc->scale = stbtt_ScaleForPixelHeight(&info, (float)pixel_size);

        int asc, desc, lg;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
        gc->ascent = (int)(asc * gc->scale);
        gc->descent = (int)(desc * gc->scale);
        gc->line_gap = (int)(lg * gc->scale);
        gc->line_height = gc->ascent - gc->descent + gc->line_gap;

        for (int i = 0; i < 128; i++) {
            gc->glyphs[i].bitmap = nullptr;
            gc->glyphs[i].loaded = false;
        }

        return gc;
    }

    CachedGlyph* get_glyph(GlyphCache* gc, int codepoint) {
        if (codepoint < 0 || codepoint >= 256) return nullptr;
        CachedGlyph* g = &gc->glyphs[codepoint];
        if (g->loaded) return g;

        g->loaded = true;
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&info, codepoint, &advance, &lsb);
        g->advance = (int)(advance * gc->scale);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&info, codepoint, gc->scale, gc->scale,
                                     &x0, &y0, &x1, &y1);
        g->width = x1 - x0;
        g->height = y1 - y0;
        g->xoff = x0;
        g->yoff = y0;

        if (g->width > 0 && g->height > 0) {
            g->bitmap = (uint8_t*)zenith::malloc(g->width * g->height);
            stbtt_MakeCodepointBitmap(&info, g->bitmap, g->width, g->height,
                                      g->width, gc->scale, gc->scale, codepoint);
        }

        return g;
    }

    int measure_text(const char* text, int pixel_size) {
        if (!valid) return 0;
        GlyphCache* gc = get_cache(pixel_size);
        int w = 0;
        for (int i = 0; text[i]; i++) {
            CachedGlyph* g = get_glyph(gc, (unsigned char)text[i]);
            if (g) w += g->advance;
        }
        return w;
    }

    int get_line_height(int pixel_size) {
        if (!valid) return 16;
        GlyphCache* gc = get_cache(pixel_size);
        return gc->line_height;
    }

    void draw(Framebuffer& fb, int x, int y, const char* text,
              Color color, int pixel_size) {
        if (!valid) return;
        GlyphCache* gc = get_cache(pixel_size);
        int cx = x;
        int baseline = y + gc->ascent;

        for (int i = 0; text[i]; i++) {
            CachedGlyph* g = get_glyph(gc, (unsigned char)text[i]);
            if (!g) continue;

            if (g->bitmap) {
                int gx = cx + g->xoff;
                int gy = baseline + g->yoff;
                for (int row = 0; row < g->height; row++) {
                    for (int col = 0; col < g->width; col++) {
                        uint8_t alpha = g->bitmap[row * g->width + col];
                        if (alpha > 0) {
                            Color c = {color.r, color.g, color.b, alpha};
                            fb.put_pixel_alpha(gx + col, gy + row, c);
                        }
                    }
                }
            }
            cx += g->advance;
        }
    }

    void draw_bg(Framebuffer& fb, int x, int y, const char* text,
                 Color fg, Color bg, int pixel_size) {
        if (!valid) return;
        GlyphCache* gc = get_cache(pixel_size);

        // Fill background for the text extent
        int tw = measure_text(text, pixel_size);
        fb.fill_rect(x, y, tw, gc->line_height, bg);

        // Then draw foreground
        draw(fb, x, y, text, fg, pixel_size);
    }

    void draw_to_buffer(uint32_t* pixels, int buf_w, int buf_h,
                        int x, int y, const char* text,
                        Color color, int pixel_size) {
        if (!valid) return;
        GlyphCache* gc = get_cache(pixel_size);
        int cx = x;
        int baseline = y + gc->ascent;

        for (int i = 0; text[i]; i++) {
            CachedGlyph* g = get_glyph(gc, (unsigned char)text[i]);
            if (!g) continue;

            if (g->bitmap) {
                int gx = cx + g->xoff;
                int gy = baseline + g->yoff;
                for (int row = 0; row < g->height; row++) {
                    int dy = gy + row;
                    if (dy < 0 || dy >= buf_h) continue;
                    for (int col = 0; col < g->width; col++) {
                        int dx = gx + col;
                        if (dx < 0 || dx >= buf_w) continue;
                        uint8_t alpha = g->bitmap[row * g->width + col];
                        if (alpha == 0) continue;

                        if (alpha == 255) {
                            pixels[dy * buf_w + dx] =
                                0xFF000000 | ((uint32_t)color.r << 16) |
                                ((uint32_t)color.g << 8) | color.b;
                        } else {
                            uint32_t dst = pixels[dy * buf_w + dx];
                            uint8_t dr = (dst >> 16) & 0xFF;
                            uint8_t dg = (dst >> 8) & 0xFF;
                            uint8_t db = dst & 0xFF;
                            uint32_t a = alpha, inv_a = 255 - alpha;
                            uint32_t rr = (a * color.r + inv_a * dr + 128) / 255;
                            uint32_t gg = (a * color.g + inv_a * dg + 128) / 255;
                            uint32_t bb = (a * color.b + inv_a * db + 128) / 255;
                            pixels[dy * buf_w + dx] =
                                0xFF000000 | (rr << 16) | (gg << 8) | bb;
                        }
                    }
                }
            }
            cx += g->advance;
        }
    }

    // Draw single character to buffer, returning advance width
    int draw_char_to_buffer(uint32_t* pixels, int buf_w, int buf_h,
                            int x, int baseline, int codepoint,
                            Color color, GlyphCache* gc) {
        CachedGlyph* g = get_glyph(gc, codepoint);
        if (!g) return 0;

        if (g->bitmap) {
            int gx = x + g->xoff;
            int gy = baseline + g->yoff;
            for (int row = 0; row < g->height; row++) {
                int dy = gy + row;
                if (dy < 0 || dy >= buf_h) continue;
                for (int col = 0; col < g->width; col++) {
                    int dx = gx + col;
                    if (dx < 0 || dx >= buf_w) continue;
                    uint8_t alpha = g->bitmap[row * g->width + col];
                    if (alpha == 0) continue;

                    if (alpha == 255) {
                        pixels[dy * buf_w + dx] =
                            0xFF000000 | ((uint32_t)color.r << 16) |
                            ((uint32_t)color.g << 8) | color.b;
                    } else {
                        uint32_t dst = pixels[dy * buf_w + dx];
                        uint8_t dr = (dst >> 16) & 0xFF;
                        uint8_t dg = (dst >> 8) & 0xFF;
                        uint8_t db = dst & 0xFF;
                        uint32_t a = alpha, inv_a = 255 - alpha;
                        uint32_t rr = (a * color.r + inv_a * dr + 128) / 255;
                        uint32_t gg = (a * color.g + inv_a * dg + 128) / 255;
                        uint32_t bb = (a * color.b + inv_a * db + 128) / 255;
                        pixels[dy * buf_w + dx] =
                            0xFF000000 | (rr << 16) | (gg << 8) | bb;
                    }
                }
            }
        }
        return g->advance;
    }
};

// Global font manager
namespace fonts {
    inline TrueTypeFont* system_font = nullptr;
    inline TrueTypeFont* system_bold = nullptr;
    inline TrueTypeFont* mono = nullptr;
    inline TrueTypeFont* mono_bold = nullptr;

    inline int UI_SIZE    = 18;
    inline int TITLE_SIZE = 18;
    inline int TERM_SIZE  = 18;
    inline int LARGE_SIZE = 28;

    inline bool init() {
        auto load = [](const char* path) -> TrueTypeFont* {
            TrueTypeFont* f = (TrueTypeFont*)zenith::malloc(sizeof(TrueTypeFont));
            zenith::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init(path)) {
                zenith::mfree(f);
                return nullptr;
            }
            return f;
        };

        system_font = load("0:/fonts/Roboto-Medium.ttf");
        system_bold = load("0:/fonts/Roboto-Bold.ttf");
        mono        = load("0:/fonts/JetBrainsMono-Regular.ttf");
        mono_bold   = load("0:/fonts/JetBrainsMono-Bold.ttf");

        return system_font != nullptr;
    }
}

} // namespace gui
