/*
    * gui.hpp
    * ZenithOS core GUI types and utilities
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace gui {

// 16.16 fixed-point type
using fixed_t = int32_t;

static constexpr int FIXED_SHIFT = 16;

inline fixed_t int_to_fixed(int v) { return v << FIXED_SHIFT; }
inline int fixed_to_int(fixed_t v) { return v >> FIXED_SHIFT; }
inline fixed_t fixed_mul(fixed_t a, fixed_t b) { return (int32_t)(((int64_t)a * b) >> FIXED_SHIFT); }
inline fixed_t fixed_div(fixed_t a, fixed_t b) { return (int32_t)(((int64_t)a << FIXED_SHIFT) / b); }
inline fixed_t fixed_from_parts(int whole, int frac_num, int frac_den) {
    return int_to_fixed(whole) + (int32_t)(((int64_t)frac_num << FIXED_SHIFT) / frac_den);
}

struct Color {
    uint8_t r, g, b, a;

    static Color from_rgb(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b, 255}; }
    static Color from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return {r, g, b, a}; }
    static Color from_hex(uint32_t hex) {
        return {(uint8_t)((hex >> 16) & 0xFF), (uint8_t)((hex >> 8) & 0xFF), (uint8_t)(hex & 0xFF), 255};
    }

    uint32_t to_pixel() const { return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
};

// Named colors for the desktop theme
namespace colors {
    static constexpr Color DESKTOP_BG    = {0xE0, 0xE0, 0xE0, 0xFF};
    static constexpr Color PANEL_BG      = {0x2B, 0x3E, 0x50, 0xFF};
    static constexpr Color TITLEBAR_BG   = {0xF5, 0xF5, 0xF5, 0xFF};
    static constexpr Color WINDOW_BG     = {0xFF, 0xFF, 0xFF, 0xFF};
    static constexpr Color BORDER        = {0xCC, 0xCC, 0xCC, 0xFF};
    static constexpr Color TEXT_COLOR    = {0x33, 0x33, 0x33, 0xFF};
    static constexpr Color PANEL_TEXT    = {0xFF, 0xFF, 0xFF, 0xFF};
    static constexpr Color ACCENT        = {0x36, 0x7B, 0xF0, 0xFF};
    static constexpr Color CLOSE_BTN     = {0xFF, 0x5F, 0x57, 0xFF};
    static constexpr Color MAX_BTN       = {0x28, 0xCA, 0x42, 0xFF};
    static constexpr Color MIN_BTN       = {0xFF, 0xBD, 0x2E, 0xFF};
    static constexpr Color SHADOW        = {0x00, 0x00, 0x00, 0x40};
    static constexpr Color TRANSPARENT   = {0x00, 0x00, 0x00, 0x00};
    static constexpr Color BLACK         = {0x00, 0x00, 0x00, 0xFF};
    static constexpr Color WHITE         = {0xFF, 0xFF, 0xFF, 0xFF};
    static constexpr Color ICON_COLOR    = {0x5C, 0x61, 0x6C, 0xFF};
    static constexpr Color SCROLLBAR_BG  = {0xF0, 0xF0, 0xF0, 0xFF};
    static constexpr Color SCROLLBAR_FG  = {0xC0, 0xC0, 0xC0, 0xFF};
    static constexpr Color MENU_BG       = {0xFF, 0xFF, 0xFF, 0xFF};
    static constexpr Color MENU_HOVER    = {0xE8, 0xF0, 0xFE, 0xFF};
    static constexpr Color TERM_BG       = {0x2D, 0x2D, 0x2D, 0xFF};
    static constexpr Color TERM_FG       = {0xCC, 0xCC, 0xCC, 0xFF};
}

struct Point {
    int x, y;
};

struct Rect {
    int x, y, w, h;

    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    Rect intersect(const Rect& other) const {
        int rx = x > other.x ? x : other.x;
        int ry = y > other.y ? y : other.y;
        int rx2 = (x + w) < (other.x + other.w) ? (x + w) : (other.x + other.w);
        int ry2 = (y + h) < (other.y + other.h) ? (y + h) : (other.y + other.h);
        if (rx2 <= rx || ry2 <= ry) return {0, 0, 0, 0};
        return {rx, ry, rx2 - rx, ry2 - ry};
    }

    bool empty() const { return w <= 0 || h <= 0; }
};

// Simple inline utility functions
inline int gui_min(int a, int b) { return a < b ? a : b; }
inline int gui_max(int a, int b) { return a > b ? a : b; }
inline int gui_abs(int a) { return a < 0 ? -a : a; }
inline int gui_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace gui
