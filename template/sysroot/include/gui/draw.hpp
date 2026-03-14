/*
    * draw.hpp
    * MontaukOS drawing primitives (lines, circles, rounded rects, cursor)
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"

namespace gui {

// Fast horizontal line
inline void draw_hline(Framebuffer& fb, int x, int y, int w, Color c) {
    for (int i = 0; i < w; i++) fb.put_pixel(x + i, y, c);
}

// Fast vertical line
inline void draw_vline(Framebuffer& fb, int x, int y, int h, Color c) {
    for (int i = 0; i < h; i++) fb.put_pixel(x, y + i, c);
}

// Rectangle outline
inline void draw_rect(Framebuffer& fb, int x, int y, int w, int h, Color c) {
    draw_hline(fb, x, y, w, c);
    draw_hline(fb, x, y + h - 1, w, c);
    draw_vline(fb, x, y, h, c);
    draw_vline(fb, x + w - 1, y, h, c);
}

// Filled rounded rectangle using corner circles
inline void fill_rounded_rect(Framebuffer& fb, int x, int y, int w, int h, int radius, Color c) {
    if (radius <= 0) {
        fb.fill_rect(x, y, w, h, c);
        return;
    }

    // Clamp radius to half the smaller dimension
    int max_r = gui_min(w / 2, h / 2);
    if (radius > max_r) radius = max_r;

    // Fill the center rectangle
    fb.fill_rect(x + radius, y, w - 2 * radius, h, c);
    // Fill the left and right strips (excluding corners)
    fb.fill_rect(x, y + radius, radius, h - 2 * radius, c);
    fb.fill_rect(x + w - radius, y + radius, radius, h - 2 * radius, c);

    // Draw the four rounded corners using midpoint circle
    int cx_tl = x + radius;
    int cy_tl = y + radius;
    int cx_tr = x + w - radius - 1;
    int cy_tr = y + radius;
    int cx_bl = x + radius;
    int cy_bl = y + h - radius - 1;
    int cx_br = x + w - radius - 1;
    int cy_br = y + h - radius - 1;

    int px = 0;
    int py = radius;
    int d = 1 - radius;

    while (px <= py) {
        // Top-left corner
        draw_hline(fb, cx_tl - py, cy_tl - px, py - radius + radius, c);
        draw_hline(fb, cx_tl - px, cy_tl - py, px, c);

        // Top-right corner
        draw_hline(fb, cx_tr + 1, cy_tr - px, py, c);
        draw_hline(fb, cx_tr + 1, cy_tr - py, px, c);

        // Bottom-left corner
        draw_hline(fb, cx_bl - py, cy_bl + px, py, c);
        draw_hline(fb, cx_bl - px, cy_bl + py, px, c);

        // Bottom-right corner
        draw_hline(fb, cx_br + 1, cy_br + px, py, c);
        draw_hline(fb, cx_br + 1, cy_br + py, px, c);

        if (d < 0) {
            d += 2 * px + 3;
        } else {
            d += 2 * (px - py) + 5;
            py--;
        }
        px++;
    }
}

// Filled circle (midpoint algorithm)
inline void fill_circle(Framebuffer& fb, int cx, int cy, int r, Color c) {
    if (r <= 0) return;

    int x = 0;
    int y = r;
    int d = 1 - r;

    // Draw initial horizontal lines
    draw_hline(fb, cx - r, cy, 2 * r + 1, c);

    while (x < y) {
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
            draw_hline(fb, cx - x, cy + y + 1, 2 * x + 1, c);
            draw_hline(fb, cx - x, cy - y - 1, 2 * x + 1, c);
        }
        x++;
        draw_hline(fb, cx - y, cy + x, 2 * y + 1, c);
        draw_hline(fb, cx - y, cy - x, 2 * y + 1, c);
    }
}

// Circle outline (midpoint algorithm)
inline void draw_circle(Framebuffer& fb, int cx, int cy, int r, Color c) {
    if (r <= 0) {
        fb.put_pixel(cx, cy, c);
        return;
    }

    int x = 0;
    int y = r;
    int d = 1 - r;

    while (x <= y) {
        fb.put_pixel(cx + x, cy + y, c);
        fb.put_pixel(cx - x, cy + y, c);
        fb.put_pixel(cx + x, cy - y, c);
        fb.put_pixel(cx - x, cy - y, c);
        fb.put_pixel(cx + y, cy + x, c);
        fb.put_pixel(cx - y, cy + x, c);
        fb.put_pixel(cx + y, cy - x, c);
        fb.put_pixel(cx - y, cy - x, c);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// Bresenham line drawing
inline void draw_line(Framebuffer& fb, int x0, int y0, int x1, int y1, Color c) {
    int dx = gui_abs(x1 - x0);
    int dy = gui_abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        fb.put_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// Drop shadow: offset darker rectangles below/right
inline void draw_shadow(Framebuffer& fb, int x, int y, int w, int h, int offset, Color shadow_color) {
    // Bottom shadow strip
    fb.fill_rect_alpha(x + offset, y + h, w, offset, shadow_color);
    // Right shadow strip
    fb.fill_rect_alpha(x + w, y + offset, offset, h, shadow_color);
    // Corner
    fb.fill_rect_alpha(x + w, y + h, offset, offset, shadow_color);
}

// 16x16 mouse cursor bitmaps
// Outline (black, where design has '1')
static constexpr uint16_t cursor_outline[16] = {
    0x8000, // 1000000000000000
    0xC000, // 1100000000000000
    0xA000, // 1010000000000000
    0x9000, // 1001000000000000
    0x8800, // 1000100000000000
    0x8400, // 1000010000000000
    0x8200, // 1000001000000000
    0x8100, // 1000000100000000
    0x8080, // 1000000010000000
    0x8040, // 1000000001000000
    0x8780, // 1000011110000000   (changed: row 10 = 1 2 2 2 2 2 1 1 1 1)
    0x9200, // 1001001000000000   (row 11 = 1 2 2 1 2 2 1)
    0xA900, // 1010100100000000   (row 12 = 1 2 1 0 1 2 2 1)
    0xC900, // 1100100100000000   (row 13 = 1 1 0 0 1 2 2 1)
    0x8480, // 1000010010000000   (row 14 = 1 0 0 0 0 1 2 2 1)
    0x0700, // 0000011100000000   (row 15 = 0 0 0 0 0 1 1 1)
};

// Fill (white, where design has '2')
static constexpr uint16_t cursor_fill[16] = {
    0x0000, // row 0:  no fill
    0x0000, // row 1:  no fill
    0x4000, // row 2:  0100000000000000
    0x6000, // row 3:  0110000000000000
    0x7000, // row 4:  0111000000000000
    0x7800, // row 5:  0111100000000000
    0x7C00, // row 6:  0111110000000000
    0x7E00, // row 7:  0111111000000000
    0x7F00, // row 8:  0111111100000000
    0x7F80, // row 9:  0111111110000000
    0x7800, // row 10: 0111100000000000  (fill only in positions 1-5)
    0x6C00, // row 11: 0110110000000000  (fill at positions 1,2 and 4,5)
    0x4600, // row 12: 0100011000000000  (fill at position 1 and 5,6)
    0x0600, // row 13: 0000011000000000  (fill at positions 5,6)
    0x0300, // row 14: 0000001100000000  (fill at positions 6,7)
    0x0000, // row 15: no fill
};

// Resize cursor: horizontal double arrow (left-right)
static constexpr uint16_t cursor_h_resize_outline[16] = {
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0820, // 0000100000100000
    0x1830, // 0001100000110000
    0x3FF8, // 0011111111111000
    0x7FFC, // 0111111111111100
    0x3FF8, // 0011111111111000
    0x1830, // 0001100000110000
    0x0820, // 0000100000100000
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};
static constexpr uint16_t cursor_h_resize_fill[16] = {
    0x0000, 0x0000, 0x0000, 0x0000,
    0x0000,
    0x0000,
    0x1FF0, // 0001111111110000
    0x3FF8, // 0011111111111000
    0x1FF0, // 0001111111110000
    0x0000,
    0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

// Resize cursor: vertical double arrow (up-down)
static constexpr uint16_t cursor_v_resize_outline[16] = {
    0x0000, 0x0000,
    0x0200, // 0000001000000000
    0x0700, // 0000011100000000
    0x0F80, // 0000111110000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0F80, // 0000111110000000
    0x0700, // 0000011100000000
    0x0200, // 0000001000000000
    0x0000, 0x0000,
};
static constexpr uint16_t cursor_v_resize_fill[16] = {
    0x0000, 0x0000, 0x0000,
    0x0200, // 0000001000000000
    0x0700, // 0000011100000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0200, // 0000001000000000
    0x0700, // 0000011100000000
    0x0200, // 0000001000000000
    0x0000, 0x0000, 0x0000,
};

// Resize cursor: diagonal NW-SE double arrow
static constexpr uint16_t cursor_nwse_resize_outline[16] = {
    0x0000, 0x0000,
    0x7C00, // 0111110000000000
    0x6000, // 0110000000000000
    0x5000, // 0101000000000000
    0x4800, // 0100100000000000
    0x2400, // 0010010000000000
    0x1200, // 0001001000000000
    0x0900, // 0000100100000000
    0x0480, // 0000010010000000
    0x0240, // 0000001001000000
    0x0140, // 0000000101000000
    0x00C0, // 0000000011000000
    0x07C0, // 0000011111000000
    0x0000, 0x0000,
};
static constexpr uint16_t cursor_nwse_resize_fill[16] = {
    0x0000, 0x0000, 0x0000,
    0x1C00, // 0001110000000000
    0x2800, // 0010100000000000
    0x0400, // 0000010000000000
    0x0200, // 0000001000000000
    0x0100, // 0000000100000000
    0x0080, // 0000000010000000
    0x0040, // 0000000001000000
    0x0280, // 0000001010000000
    0x0380, // 0000001110000000
    0x0000, 0x0000, 0x0000, 0x0000,
};

// Resize cursor: diagonal NE-SW double arrow
static constexpr uint16_t cursor_nesw_resize_outline[16] = {
    0x0000, 0x0000,
    0x07C0, // 0000011111000000
    0x00C0, // 0000000011000000
    0x0140, // 0000000101000000
    0x0240, // 0000001001000000
    0x0480, // 0000010010000000
    0x0900, // 0000100100000000
    0x1200, // 0001001000000000
    0x2400, // 0010010000000000
    0x4800, // 0100100000000000
    0x5000, // 0101000000000000
    0x6000, // 0110000000000000
    0x7C00, // 0111110000000000
    0x0000, 0x0000,
};
static constexpr uint16_t cursor_nesw_resize_fill[16] = {
    0x0000, 0x0000, 0x0000,
    0x0380, // 0000001110000000
    0x0280, // 0000001010000000
    0x0040, // 0000000001000000
    0x0080, // 0000000010000000
    0x0100, // 0000000100000000
    0x0200, // 0000001000000000
    0x0400, // 0000010000000000
    0x2800, // 0010100000000000
    0x1C00, // 0001110000000000
    0x0000, 0x0000, 0x0000, 0x0000,
};

enum CursorStyle {
    CURSOR_ARROW = 0,
    CURSOR_RESIZE_H,    // left-right
    CURSOR_RESIZE_V,    // up-down
    CURSOR_RESIZE_NWSE, // diagonal NW-SE
    CURSOR_RESIZE_NESW, // diagonal NE-SW
};

// Draw the mouse cursor at (x, y)
inline void draw_cursor(Framebuffer& fb, int x, int y, CursorStyle style = CURSOR_ARROW) {
    const uint16_t* outline_data = cursor_outline;
    const uint16_t* fill_data = cursor_fill;
    int ox = 0, oy = 0; // hotspot offset for centered cursors

    switch (style) {
    case CURSOR_RESIZE_H:
        outline_data = cursor_h_resize_outline;
        fill_data = cursor_h_resize_fill;
        ox = -8; oy = -8;
        break;
    case CURSOR_RESIZE_V:
        outline_data = cursor_v_resize_outline;
        fill_data = cursor_v_resize_fill;
        ox = -8; oy = -8;
        break;
    case CURSOR_RESIZE_NWSE:
        outline_data = cursor_nwse_resize_outline;
        fill_data = cursor_nwse_resize_fill;
        ox = -8; oy = -8;
        break;
    case CURSOR_RESIZE_NESW:
        outline_data = cursor_nesw_resize_outline;
        fill_data = cursor_nesw_resize_fill;
        ox = -8; oy = -8;
        break;
    default:
        break;
    }

    Color black = colors::BLACK;
    Color white = colors::WHITE;

    for (int row = 0; row < 16; row++) {
        uint16_t outline = outline_data[row];
        uint16_t fill = fill_data[row];
        for (int col = 0; col < 16; col++) {
            uint16_t mask = (uint16_t)(0x8000 >> col);
            if (outline & mask) {
                fb.put_pixel(x + ox + col, y + oy + row, black);
            } else if (fill & mask) {
                fb.put_pixel(x + ox + col, y + oy + row, white);
            }
        }
    }
}

} // namespace gui
