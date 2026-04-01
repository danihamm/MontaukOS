/*
    * helpers.cpp
    * Shared helpers for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

bool color_eq(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool settings_status_visible(SettingsState* st) {
    return st->status_msg[0] &&
           (montauk::get_milliseconds() - st->status_time < 4000);
}

void settings_set_status(SettingsState* st, const char* msg) {
    montauk::strncpy(st->status_msg, msg ? msg : "", (int)sizeof(st->status_msg));
    st->status_time = montauk::get_milliseconds();
}

int find_swatch(const Color* palette, Color current) {
    for (int i = 0; i < SWATCH_COUNT; i++) {
        if (color_eq(palette[i], current)) return i;
    }
    return -1;
}

void draw_swatch_row(Canvas& c, int x, int y, const Color* palette, Color selected, Color accent) {
    int sel = find_swatch(palette, selected);
    for (int i = 0; i < SWATCH_COUNT; i++) {
        int sx = x + i * (SWATCH_SIZE + SWATCH_GAP);
        // Draw selection border
        if (i == sel) {
            c.fill_rounded_rect(sx - 2, y - 2, SWATCH_SIZE + 4, SWATCH_SIZE + 4, 4, accent);
        }
        c.fill_rounded_rect(sx, y, SWATCH_SIZE, SWATCH_SIZE, 3, palette[i]);
        // Thin border for light colors
        c.rect(sx, y, SWATCH_SIZE, SWATCH_SIZE, Color::from_rgb(0xCC, 0xCC, 0xCC));
    }
}

void draw_radio(Canvas& c, int x, int y, bool selected, Color accent) {
    int r = 7;
    // Outer circle (simple square approximation with rounded rect)
    c.fill_rounded_rect(x, y, r * 2, r * 2, r, Color::from_rgb(0xCC, 0xCC, 0xCC));
    c.fill_rounded_rect(x + 1, y + 1, r * 2 - 2, r * 2 - 2, r - 1, colors::WHITE);
    if (selected) {
        c.fill_rounded_rect(x + 4, y + 4, r * 2 - 8, r * 2 - 8, r - 4, accent);
    }
}

void draw_toggle_btn(Canvas& c, int x, int y, int bw, int bh,
                     const char* label, bool active, Color accent) {
    Color bg = active ? accent : colors::WINDOW_BG;
    Color fg = active ? colors::WHITE : colors::TEXT_COLOR;
    c.fill_rounded_rect(x, y, bw, bh, 4, bg);
    if (!active) {
        c.rect(x, y, bw, bh, colors::BORDER);
    }
    int tw = text_width(label);
    int fh = system_font_height();
    c.text(x + (bw - tw) / 2, y + (bh - fh) / 2, label, fg);
}

bool swatch_hit(int mx, int my, int row_x, int row_y, int* out_idx) {
    for (int i = 0; i < SWATCH_COUNT; i++) {
        int sx = row_x + i * (SWATCH_SIZE + SWATCH_GAP);
        if (mx >= sx && mx < sx + SWATCH_SIZE && my >= row_y && my < row_y + SWATCH_SIZE) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

} // namespace settings_app
