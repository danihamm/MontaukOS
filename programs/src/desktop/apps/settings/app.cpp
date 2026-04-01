/*
    * app.cpp
    * Settings launcher and desktop integration entry points
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

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
    Canvas content(win->content + TAB_BAR_H * win->content_w,
                   win->content_w, win->content_h - TAB_BAR_H);

    switch (st->active_tab) {
    case 0: settings_draw_appearance(content, st); break;
    case 1: settings_draw_display(content, st); break;
    case 2: settings_draw_users(content, st); break;
    case 3: settings_draw_about(content, st); break;
    }
}

static void settings_on_mouse(Window* win, MouseEvent& ev) {
    SettingsState* st = (SettingsState*)win->app_data;
    if (!st || !ev.left_pressed()) return;

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

    switch (st->active_tab) {
    case 0:
        settings_handle_appearance_click(win, st, mx, cy);
        break;
    case 1:
        settings_handle_display_click(st, mx, cy);
        break;
    case 2:
        settings_handle_users_click(win, st, mx, cy);
        break;
    default:
        break;
    }
}

static void settings_on_key(Window* win, const Montauk::KeyEvent& key) {
    // Settings main window no longer needs keyboard handling.
    (void)win;
    (void)key;
}

static void settings_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

} // namespace settings_app

void open_settings(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Settings", 200, 100, 480, 420);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    settings_app::SettingsState* st =
        (settings_app::SettingsState*)montauk::malloc(sizeof(settings_app::SettingsState));
    montauk::memset(st, 0, sizeof(settings_app::SettingsState));
    st->desktop = ds;
    st->active_tab = 0;
    montauk::get_info(&st->sys_info);
    st->uptime_ms = montauk::get_milliseconds();
    st->wp_scanned = false;
    st->wp_files.count = 0;
    st->selected_user = -1;
    st->users_loaded = false;
    st->status_msg[0] = '\0';

    win->app_data = st;
    win->on_draw = settings_app::settings_on_draw;
    win->on_mouse = settings_app::settings_on_mouse;
    win->on_key = settings_app::settings_on_key;
    win->on_close = settings_app::settings_on_close;
}
