/*
    * font.hpp
    * ZenithOS 8x16 VGA bitmap font rendering
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"

namespace gui {

static constexpr int FONT_WIDTH = 8;
static constexpr int FONT_HEIGHT = 16;

// Defined in font_data.cpp
extern const uint8_t font_data[256 * 16];

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
    for (int i = 0; text[i]; i++) {
        draw_char(fb, x + i * FONT_WIDTH, y, text[i], fg);
    }
}

inline void draw_text_bg(Framebuffer& fb, int x, int y, const char* text, Color fg, Color bg) {
    for (int i = 0; text[i]; i++) {
        draw_char_bg(fb, x + i * FONT_WIDTH, y, text[i], fg, bg);
    }
}

inline int text_width(const char* text) {
    int len = 0;
    while (text[len]) len++;
    return len * FONT_WIDTH;
}

} // namespace gui
