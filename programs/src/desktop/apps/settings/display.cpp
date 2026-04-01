/*
    * display.cpp
    * Display tab for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

void apply_ui_scale(int scale) {
    switch (scale) {
    case 0: fonts::UI_SIZE=14; fonts::TITLE_SIZE=14; fonts::TERM_SIZE=14; fonts::LARGE_SIZE=22; break;
    case 2: fonts::UI_SIZE=22; fonts::TITLE_SIZE=22; fonts::TERM_SIZE=22; fonts::LARGE_SIZE=34; break;
    default: fonts::UI_SIZE=18; fonts::TITLE_SIZE=18; fonts::TERM_SIZE=18; fonts::LARGE_SIZE=28; break;
    }
}

void settings_draw_display(Canvas& c, SettingsState* st) {
    DesktopSettings& s = st->desktop->settings;
    Color accent = s.accent_color;
    int x = 16;
    int y = 20;
    int btn_w = 60;
    int btn_h = 28;

    // Window Shadows
    c.text(x, y + 6, "Window Shadows", colors::TEXT_COLOR);
    int bx = x + 180;
    draw_toggle_btn(c, bx, y, btn_w, btn_h, "On", s.show_shadows, accent);
    draw_toggle_btn(c, bx + btn_w + 8, y, btn_w, btn_h, "Off", !s.show_shadows, accent);
    y += btn_h + 20;

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 16;

    // Clock Format
    c.text(x, y + 6, "Clock Format", colors::TEXT_COLOR);
    bx = x + 180;
    draw_toggle_btn(c, bx, y, btn_w, btn_h, "24h", s.clock_24h, accent);
    draw_toggle_btn(c, bx + btn_w + 8, y, btn_w, btn_h, "12h", !s.clock_24h, accent);
    y += btn_h + 20;

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 16;

    // UI Scale
    c.text(x, y + 6, "UI Scale", colors::TEXT_COLOR);
    bx = x + 180;
    int sbw = 68;
    draw_toggle_btn(c, bx, y, sbw, btn_h, "Small", s.ui_scale == 0, accent);
    draw_toggle_btn(c, bx + sbw + 8, y, sbw, btn_h, "Default", s.ui_scale == 1, accent);
    draw_toggle_btn(c, bx + (sbw + 8) * 2, y, sbw, btn_h, "Large", s.ui_scale == 2, accent);
    y += btn_h + 20;

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 16;

    // UTC Offset
    c.text(x, y + 6, "UTC Offset", colors::TEXT_COLOR);
    bx = x + 180;

    // [-] button
    draw_toggle_btn(c, bx, y, 36, btn_h, "-", false, accent);

    // Offset label
    int off = s.tz_offset_minutes;
    int offH = off / 60;
    int offM = off % 60;
    if (offM < 0) offM = -offM;
    char tz_label[16];
    if (offM)
        snprintf(tz_label, sizeof(tz_label), "UTC%s%d:%02d", offH >= 0 ? "+" : "", offH, offM);
    else
        snprintf(tz_label, sizeof(tz_label), "UTC%s%d", offH >= 0 ? "+" : "", offH);
    int tw = text_width(tz_label);
    int label_w = 90;
    c.text(bx + 36 + (label_w - tw) / 2, y + 6, tz_label, colors::TEXT_COLOR);

    // [+] button
    draw_toggle_btn(c, bx + 36 + label_w, y, 36, btn_h, "+", false, accent);
}

bool settings_handle_display_click(SettingsState* st, int mx, int cy) {
    DesktopSettings& s = st->desktop->settings;
    int x = 16;
    int y = 20;
    int btn_w = 60;
    int btn_h = 28;
    int bx = x + 180;

    // Window Shadows: On
    if (mx >= bx && mx < bx + btn_w && cy >= y && cy < y + btn_h) {
        s.show_shadows = true;
        settings_persist(st);
        return true;
    }
    // Window Shadows: Off
    if (mx >= bx + btn_w + 8 && mx < bx + btn_w * 2 + 8 && cy >= y && cy < y + btn_h) {
        s.show_shadows = false;
        settings_persist(st);
        return true;
    }
    y += btn_h + 20 + 16;

    // Clock: 24h
    if (mx >= bx && mx < bx + btn_w && cy >= y && cy < y + btn_h) {
        s.clock_24h = true;
        settings_persist(st);
        return true;
    }
    // Clock: 12h
    if (mx >= bx + btn_w + 8 && mx < bx + btn_w * 2 + 8 && cy >= y && cy < y + btn_h) {
        s.clock_24h = false;
        settings_persist(st);
        return true;
    }
    y += btn_h + 20 + 16;

    // UI Scale buttons
    int sbw = 68;
    // Small
    if (mx >= bx && mx < bx + sbw && cy >= y && cy < y + btn_h) {
        s.ui_scale = 0;
        apply_ui_scale(0);
        montauk::win_setscale(0);
        settings_persist(st);
        return true;
    }
    // Default
    if (mx >= bx + sbw + 8 && mx < bx + sbw * 2 + 8 && cy >= y && cy < y + btn_h) {
        s.ui_scale = 1;
        apply_ui_scale(1);
        montauk::win_setscale(1);
        settings_persist(st);
        return true;
    }
    // Large
    if (mx >= bx + (sbw + 8) * 2 && mx < bx + sbw * 3 + 16 && cy >= y && cy < y + btn_h) {
        s.ui_scale = 2;
        apply_ui_scale(2);
        montauk::win_setscale(2);
        settings_persist(st);
        return true;
    }
    y += btn_h + 20 + 16;

    // UTC Offset: [-] button
    if (mx >= bx && mx < bx + 36 && cy >= y && cy < y + btn_h) {
        if (s.tz_offset_minutes > -720) {
            s.tz_offset_minutes -= 60;
            montauk::settz(s.tz_offset_minutes);
            settings_persist_tz(st);
        }
        return true;
    }
    // UTC Offset: [+] button
    int label_w = 90;
    if (mx >= bx + 36 + label_w && mx < bx + 36 + label_w + 36 && cy >= y && cy < y + btn_h) {
        if (s.tz_offset_minutes < 840) {
            s.tz_offset_minutes += 60;
            montauk::settz(s.tz_offset_minutes);
            settings_persist_tz(st);
        }
        return true;
    }

    return false;
}

} // namespace settings_app
