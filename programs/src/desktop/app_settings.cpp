/*
    * app_settings.cpp
    * ZenithOS Desktop - Settings application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Settings state
// ============================================================================

struct SettingsState {
    DesktopState* desktop;
    int active_tab;        // 0=Appearance, 1=Display, 2=About
    Zenith::SysInfo sys_info;
    uint64_t uptime_ms;
};

// ============================================================================
// Color palette presets
// ============================================================================

static constexpr int SWATCH_COUNT = 8;
static constexpr int SWATCH_SIZE = 24;
static constexpr int SWATCH_GAP = 6;

// Background colors (light tones)
static const Color bg_palette[SWATCH_COUNT] = {
    Color::from_rgb(0xD0, 0xD8, 0xE8),  // light blue-gray (default)
    Color::from_rgb(0xE8, 0xDD, 0xCB),  // warm beige
    Color::from_rgb(0xC8, 0xE6, 0xD0),  // mint green
    Color::from_rgb(0xD8, 0xD0, 0xE8),  // lavender
    Color::from_rgb(0xB8, 0xBE, 0xC8),  // slate
    Color::from_rgb(0xF0, 0xF0, 0xF0),  // white
    Color::from_rgb(0xE8, 0xD0, 0xD8),  // soft pink
    Color::from_rgb(0xE8, 0xE0, 0xC8),  // light gold
};

// Panel colors (dark tones)
static const Color panel_palette[SWATCH_COUNT] = {
    Color::from_rgb(0x2B, 0x3E, 0x50),  // dark blue-gray (default)
    Color::from_rgb(0x2D, 0x2D, 0x2D),  // dark charcoal
    Color::from_rgb(0x1B, 0x2A, 0x4A),  // navy
    Color::from_rgb(0x1A, 0x3A, 0x3A),  // dark teal
    Color::from_rgb(0x1A, 0x3A, 0x1A),  // dark green
    Color::from_rgb(0x30, 0x20, 0x40),  // dark purple
    Color::from_rgb(0x40, 0x1A, 0x1A),  // dark red
    Color::from_rgb(0x10, 0x10, 0x10),  // black
};

// Accent colors
static const Color accent_palette[SWATCH_COUNT] = {
    Color::from_rgb(0x36, 0x7B, 0xF0),  // blue (default)
    Color::from_rgb(0x00, 0x9B, 0x9B),  // teal
    Color::from_rgb(0x2E, 0x9E, 0x3E),  // green
    Color::from_rgb(0xE0, 0x8A, 0x20),  // orange
    Color::from_rgb(0xD0, 0x3E, 0x3E),  // red
    Color::from_rgb(0x7B, 0x3E, 0xB8),  // purple
    Color::from_rgb(0xD0, 0x5C, 0x9E),  // pink
    Color::from_rgb(0x44, 0x44, 0xCC),  // indigo
};

// ============================================================================
// Layout constants
// ============================================================================

static constexpr int TAB_BAR_H = 36;
static constexpr int TAB_COUNT = 3;
static const char* tab_labels[TAB_COUNT] = { "Appearance", "Display", "About" };

// ============================================================================
// Helper: check if two colors match
// ============================================================================

static bool color_eq(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// ============================================================================
// Helper: find selected swatch index in a palette
// ============================================================================

static int find_swatch(const Color* palette, Color current) {
    for (int i = 0; i < SWATCH_COUNT; i++) {
        if (color_eq(palette[i], current)) return i;
    }
    return -1;
}

// ============================================================================
// Drawing
// ============================================================================

static void draw_swatch_row(Canvas& c, int x, int y, const Color* palette,
                             Color selected, Color accent) {
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

static void draw_radio(Canvas& c, int x, int y, bool selected, Color accent) {
    int r = 7;
    // Outer circle (simple square approximation with rounded rect)
    c.fill_rounded_rect(x, y, r * 2, r * 2, r, Color::from_rgb(0xCC, 0xCC, 0xCC));
    c.fill_rounded_rect(x + 1, y + 1, r * 2 - 2, r * 2 - 2, r - 1, colors::WHITE);
    if (selected) {
        c.fill_rounded_rect(x + 4, y + 4, r * 2 - 8, r * 2 - 8, r - 4, accent);
    }
}

static void draw_toggle_btn(Canvas& c, int x, int y, int bw, int bh,
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

static void settings_draw_appearance(Canvas& c, SettingsState* st) {
    DesktopSettings& s = st->desktop->settings;
    Color accent = s.accent_color;
    int x = 16;
    int y = 12;
    int sfh = system_font_height();
    int line_h = sfh + 10;

    // Section: Background
    c.text(x, y, "Background", colors::TEXT_COLOR);
    y += line_h;

    // Radio buttons: Gradient / Solid
    draw_radio(c, x, y, s.bg_gradient, accent);
    c.text(x + 20, y + 2, "Gradient", colors::TEXT_COLOR);

    draw_radio(c, x + 120, y, !s.bg_gradient, accent);
    c.text(x + 140, y + 2, "Solid Color", colors::TEXT_COLOR);
    y += line_h + 4;

    if (s.bg_gradient) {
        // Top color swatches
        c.text(x, y + 4, "Top", Color::from_rgb(0x88, 0x88, 0x88));
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_grad_top, accent);
        y += SWATCH_SIZE + 14;

        // Bottom color swatches
        c.text(x, y + 4, "Bottom", Color::from_rgb(0x88, 0x88, 0x88));
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_grad_bottom, accent);
        y += SWATCH_SIZE + 14;
    } else {
        // Solid color swatches
        c.text(x, y + 4, "Color", Color::from_rgb(0x88, 0x88, 0x88));
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_solid, accent);
        y += SWATCH_SIZE + 14;
    }

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    // Panel color
    c.text(x, y + 4, "Panel Color", colors::TEXT_COLOR);
    draw_swatch_row(c, x + 110, y, panel_palette, s.panel_color, accent);
    y += SWATCH_SIZE + 14;

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    // Accent color
    c.text(x, y + 4, "Accent Color", colors::TEXT_COLOR);
    draw_swatch_row(c, x + 110, y, accent_palette, s.accent_color, accent);
}

static void apply_ui_scale(int scale) {
    switch (scale) {
    case 0: fonts::UI_SIZE=14; fonts::TITLE_SIZE=14; fonts::TERM_SIZE=14; fonts::LARGE_SIZE=22; break;
    case 2: fonts::UI_SIZE=22; fonts::TITLE_SIZE=22; fonts::TERM_SIZE=22; fonts::LARGE_SIZE=34; break;
    default: fonts::UI_SIZE=18; fonts::TITLE_SIZE=18; fonts::TERM_SIZE=18; fonts::LARGE_SIZE=28; break;
    }
}

static void settings_draw_display(Canvas& c, SettingsState* st) {
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
}

static void settings_draw_about(Canvas& c, SettingsState* st) {
    st->uptime_ms = zenith::get_milliseconds();

    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    int x = 16;
    int y = 20;
    char line[128];
    int sfh = system_font_height();
    int line_h = sfh + 6;

    // OS name in 2x size
    c.text_2x(x, y, st->sys_info.osName, st->desktop->settings.accent_color);
    int large_h = (fonts::system_font && fonts::system_font->valid)
        ? fonts::system_font->get_line_height(fonts::LARGE_SIZE) : (FONT_HEIGHT * 2);
    y += large_h + 8;

    snprintf(line, sizeof(line), "Version %s", st->sys_info.osVersion);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += line_h + 8;

    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    snprintf(line, sizeof(line), "API version: %d", (int)st->sys_info.apiVersion);
    c.kv_line(x, &y, line, colors::TEXT_COLOR, line_h);

    int up_sec = (int)(st->uptime_ms / 1000);
    int up_min = up_sec / 60;
    int up_hr = up_min / 60;
    snprintf(line, sizeof(line), "Uptime: %d:%02d:%02d", up_hr, up_min % 60, up_sec % 60);
    c.kv_line(x, &y, line, colors::TEXT_COLOR, line_h);

    snprintf(line, sizeof(line), "Build: %s %s", __DATE__, __TIME__);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += line_h + 16;

    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    c.text(x, y, "Copyright (c) 2026 Daniel Hammer", dim);
}

static void settings_on_draw(Window* win, Framebuffer& fb) {
    SettingsState* st = (SettingsState*)win->app_data;
    if (!st) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    Color accent = st->desktop->settings.accent_color;
    int sfh = system_font_height();

    // Draw tab bar background
    c.fill_rect(0, 0, c.w, TAB_BAR_H, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.hline(0, TAB_BAR_H - 1, c.w, colors::BORDER);

    // Draw tabs
    int tab_w = c.w / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = i * tab_w;
        bool active = (i == st->active_tab);

        if (active) {
            c.fill_rect(tx, 0, tab_w, TAB_BAR_H, colors::WINDOW_BG);
            // Active tab underline
            c.fill_rect(tx + 4, TAB_BAR_H - 3, tab_w - 8, 3, accent);
        }

        int tw = text_width(tab_labels[i]);
        Color tc = active ? accent : Color::from_rgb(0x66, 0x66, 0x66);
        c.text(tx + (tab_w - tw) / 2, (TAB_BAR_H - sfh) / 2, tab_labels[i], tc);
    }

    // Draw tab content below the tab bar
    // Create a sub-canvas offset by TAB_BAR_H
    Canvas content(win->content + TAB_BAR_H * win->content_w,
                   win->content_w, win->content_h - TAB_BAR_H);

    switch (st->active_tab) {
    case 0: settings_draw_appearance(content, st); break;
    case 1: settings_draw_display(content, st); break;
    case 2: settings_draw_about(content, st); break;
    }
}

// ============================================================================
// Mouse interaction
// ============================================================================

static bool swatch_hit(int mx, int my, int row_x, int row_y, int* out_idx) {
    for (int i = 0; i < SWATCH_COUNT; i++) {
        int sx = row_x + i * (SWATCH_SIZE + SWATCH_GAP);
        if (mx >= sx && mx < sx + SWATCH_SIZE && my >= row_y && my < row_y + SWATCH_SIZE) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

static void settings_on_mouse(Window* win, MouseEvent& ev) {
    SettingsState* st = (SettingsState*)win->app_data;
    if (!st) return;

    if (!ev.left_pressed()) return;

    Rect cr = win->content_rect();
    int mx = ev.x - cr.x;
    int my = ev.y - cr.y;

    // Tab bar click
    if (my >= 0 && my < TAB_BAR_H) {
        int tab_w = win->content_w / TAB_COUNT;
        int tab = mx / tab_w;
        if (tab >= 0 && tab < TAB_COUNT) {
            st->active_tab = tab;
        }
        return;
    }

    // Content area (offset by TAB_BAR_H)
    int cy = my - TAB_BAR_H;
    DesktopSettings& s = st->desktop->settings;

    if (st->active_tab == 0) {
        // Appearance tab
        int x = 16;
        int sfh = system_font_height();
        int line_h = sfh + 10;
        int y = 12;

        // "Background" label
        y += line_h;

        // Radio: Gradient
        if (mx >= x && mx < x + 100 && cy >= y && cy < y + 16) {
            s.bg_gradient = true;
            return;
        }
        // Radio: Solid
        if (mx >= x + 120 && mx < x + 260 && cy >= y && cy < y + 16) {
            s.bg_gradient = false;
            return;
        }
        y += line_h + 4;

        int idx;
        if (s.bg_gradient) {
            // Top swatches
            if (swatch_hit(mx, cy, x + 70, y, &idx)) {
                s.bg_grad_top = bg_palette[idx];
                return;
            }
            y += SWATCH_SIZE + 14;

            // Bottom swatches
            if (swatch_hit(mx, cy, x + 70, y, &idx)) {
                s.bg_grad_bottom = bg_palette[idx];
                return;
            }
            y += SWATCH_SIZE + 14;
        } else {
            // Solid color swatches
            if (swatch_hit(mx, cy, x + 70, y, &idx)) {
                s.bg_solid = bg_palette[idx];
                return;
            }
            y += SWATCH_SIZE + 14;
        }

        // Separator
        y += 12;

        // Panel color swatches
        if (swatch_hit(mx, cy, x + 110, y, &idx)) {
            s.panel_color = panel_palette[idx];
            return;
        }
        y += SWATCH_SIZE + 14;

        // Separator
        y += 12;

        // Accent color swatches
        if (swatch_hit(mx, cy, x + 110, y, &idx)) {
            s.accent_color = accent_palette[idx];
            return;
        }
    } else if (st->active_tab == 1) {
        // Display tab
        int x = 16;
        int y = 20;
        int btn_w = 60;
        int btn_h = 28;
        int bx = x + 180;

        // Window Shadows: On
        if (mx >= bx && mx < bx + btn_w && cy >= y && cy < y + btn_h) {
            s.show_shadows = true;
            return;
        }
        // Window Shadows: Off
        if (mx >= bx + btn_w + 8 && mx < bx + btn_w * 2 + 8 && cy >= y && cy < y + btn_h) {
            s.show_shadows = false;
            return;
        }
        y += btn_h + 20 + 16;

        // Clock: 24h
        if (mx >= bx && mx < bx + btn_w && cy >= y && cy < y + btn_h) {
            s.clock_24h = true;
            return;
        }
        // Clock: 12h
        if (mx >= bx + btn_w + 8 && mx < bx + btn_w * 2 + 8 && cy >= y && cy < y + btn_h) {
            s.clock_24h = false;
            return;
        }
        y += btn_h + 20 + 16;

        // UI Scale buttons
        int sbw = 68;
        // Small
        if (mx >= bx && mx < bx + sbw && cy >= y && cy < y + btn_h) {
            s.ui_scale = 0;
            apply_ui_scale(0);
            zenith::win_setscale(0);
            return;
        }
        // Default
        if (mx >= bx + sbw + 8 && mx < bx + sbw * 2 + 8 && cy >= y && cy < y + btn_h) {
            s.ui_scale = 1;
            apply_ui_scale(1);
            zenith::win_setscale(1);
            return;
        }
        // Large
        if (mx >= bx + (sbw + 8) * 2 && mx < bx + sbw * 3 + 16 && cy >= y && cy < y + btn_h) {
            s.ui_scale = 2;
            apply_ui_scale(2);
            zenith::win_setscale(2);
            return;
        }
    }
}

// ============================================================================
// Cleanup
// ============================================================================

static void settings_on_close(Window* win) {
    if (win->app_data) {
        zenith::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Settings launcher
// ============================================================================

void open_settings(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Settings", 200, 100, 480, 420);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    SettingsState* st = (SettingsState*)zenith::malloc(sizeof(SettingsState));
    zenith::memset(st, 0, sizeof(SettingsState));
    st->desktop = ds;
    st->active_tab = 0;
    zenith::get_info(&st->sys_info);
    st->uptime_ms = zenith::get_milliseconds();

    win->app_data = st;
    win->on_draw = settings_on_draw;
    win->on_mouse = settings_on_mouse;
    win->on_close = settings_on_close;
}
