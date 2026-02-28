/*
    * font.hpp
    * MontaukOS text rendering — TrueType with bitmap fallback
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"
#include "gui/truetype.hpp"

namespace gui {

static constexpr int FONT_WIDTH = 8;
static constexpr int FONT_HEIGHT = 16;

// Defined in font_data.cpp
extern const uint8_t font_data[256 * 16];

// Dynamic font height: TTF line height or 16 (bitmap fallback)
inline int system_font_height() {
    if (fonts::system_font && fonts::system_font->valid)
        return fonts::system_font->get_line_height(fonts::UI_SIZE);
    return FONT_HEIGHT;
}

// Dynamic mono font cell dimensions
inline int mono_cell_width() {
    if (fonts::mono && fonts::mono->valid) {
        // Monospace: all glyphs have the same advance
        GlyphCache* gc = fonts::mono->get_cache(fonts::TERM_SIZE);
        CachedGlyph* g = fonts::mono->get_glyph(gc, 'M');
        if (g) return g->advance;
    }
    return FONT_WIDTH;
}

inline int mono_cell_height() {
    if (fonts::mono && fonts::mono->valid)
        return fonts::mono->get_line_height(fonts::TERM_SIZE);
    return FONT_HEIGHT;
}

inline void draw_char(Framebuffer& fb, int x, int y, char c, Color fg) {
    const uint8_t* glyph = &font_data[(unsigned char)c * FONT_HEIGHT];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                fb.put_pixel(x + col, y + row, fg);
            }
        }
    }
}

inline void draw_char_bg(Framebuffer& fb, int x, int y, char c, Color fg, Color bg) {
    const uint8_t* glyph = &font_data[(unsigned char)c * FONT_HEIGHT];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                fb.put_pixel(x + col, y + row, fg);
            } else {
                fb.put_pixel(x + col, y + row, bg);
            }
        }
    }
}

inline void draw_text(Framebuffer& fb, int x, int y, const char* text, Color fg) {
    if (fonts::system_font && fonts::system_font->valid) {
        fonts::system_font->draw(fb, x, y, text, fg, fonts::UI_SIZE);
        return;
    }
    for (int i = 0; text[i]; i++) {
        draw_char(fb, x + i * FONT_WIDTH, y, text[i], fg);
    }
}

inline void draw_text_bg(Framebuffer& fb, int x, int y, const char* text, Color fg, Color bg) {
    if (fonts::system_font && fonts::system_font->valid) {
        fonts::system_font->draw_bg(fb, x, y, text, fg, bg, fonts::UI_SIZE);
        return;
    }
    for (int i = 0; text[i]; i++) {
        draw_char_bg(fb, x + i * FONT_WIDTH, y, text[i], fg, bg);
    }
}

inline int text_width(const char* text) {
    if (fonts::system_font && fonts::system_font->valid) {
        return fonts::system_font->measure_text(text, fonts::UI_SIZE);
    }
    int len = 0;
    while (text[len]) len++;
    return len * FONT_WIDTH;
}

} // namespace gui
