/*
    * users.cpp
    * Users tab for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

void users_reload(SettingsState* st) {
    st->user_count = montauk::user::load_users(st->users, 16);
    st->users_loaded = true;
}

int user_row_height() {
    return system_font_height() * 2 + 12;  // Two lines of text + padding
}

void settings_draw_users(Canvas& c, SettingsState* st) {
    if (!st->users_loaded) users_reload(st);

    Color accent = st->desktop->settings.accent_color;
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    Color card_bg = Color::from_rgb(0xF8, 0xF8, 0xF8);
    int x = 16;
    int y = 20;
    int sfh = system_font_height();
    int content_w = c.w - 2 * x;
    int row_h = user_row_height();

    if (!st->desktop->is_admin) {
        c.text(x, y, "Admin access required to manage users.", dim);
        return;
    }

    // User list
    for (int i = 0; i < st->user_count; i++) {
        bool sel = (i == st->selected_user);
        bool is_current = montauk::streq(st->users[i].username, st->desktop->current_user);

        if (sel) {
            c.fill_rounded_rect(x, y, content_w, row_h, 4, accent);
        } else if (i % 2 == 0) {
            c.fill_rounded_rect(x, y, content_w, row_h, 4, card_bg);
        }

        Color tc = sel ? colors::WHITE : colors::TEXT_COLOR;
        Color sc = sel ? Color::from_rgb(0xDD, 0xDD, 0xFF) : dim;

        // Display name (primary line)
        int text_y = y + 4;
        c.text(x + 12, text_y, st->users[i].display_name, tc);

        // Username (secondary line)
        char sub[48];
        if (is_current) {
            snprintf(sub, sizeof(sub), "@%s (you)", st->users[i].username);
        } else {
            snprintf(sub, sizeof(sub), "@%s", st->users[i].username);
        }
        c.text(x + 12, text_y + sfh + 2, sub, sc);

        // Role badge
        const char* role = st->users[i].role;
        int rw = text_width(role) + 12;
        int badge_x = c.w - x - rw - 8;
        int badge_y = y + (row_h - sfh - 4) / 2;
        if (sel) {
            c.fill_rounded_rect(badge_x, badge_y, rw, sfh + 4, (sfh + 4) / 2,
                                Color::from_rgb(0xFF, 0xFF, 0xFF));
            c.text(badge_x + 6, badge_y + 2, role, accent);
        } else {
            bool is_admin_role = montauk::streq(role, "admin");
            Color badge_bg = is_admin_role
                ? Color::from_rgb(0xE8, 0xD8, 0xF0)
                : Color::from_rgb(0xE0, 0xE8, 0xF0);
            Color badge_fg = is_admin_role
                ? Color::from_rgb(0x7B, 0x3E, 0xB8)
                : Color::from_rgb(0x36, 0x7B, 0xF0);
            c.fill_rounded_rect(badge_x, badge_y, rw, sfh + 4, (sfh + 4) / 2, badge_bg);
            c.text(badge_x + 6, badge_y + 2, role, badge_fg);
        }

        y += row_h + 4;
    }

    // Separator
    y += 8;
    c.hline(x, y, content_w, colors::BORDER);
    y += 16;

    // Status message
    if (settings_status_visible(st)) {
        c.text(x, y, st->status_msg, accent);
        y += sfh + 8;
    }

    // Action buttons
    int btn_x = x;

    // Add User
    c.fill_rounded_rect(btn_x, y, 90, USER_BTN_H, 4, accent);
    {
        int tw = text_width("Add User");
        c.text(btn_x + (90 - tw) / 2, y + (USER_BTN_H - sfh) / 2, "Add User", colors::WHITE);
    }
    btn_x += 98;

    // Change Password (disabled when no selection)
    {
        bool enabled = st->selected_user >= 0;
        Color bg = enabled ? accent : Color::from_rgb(0xCC, 0xCC, 0xCC);
        c.fill_rounded_rect(btn_x, y, USER_BTN_W, USER_BTN_H, 4, bg);
        int tw = text_width("Change Pwd");
        c.text(btn_x + (USER_BTN_W - tw) / 2, y + (USER_BTN_H - sfh) / 2, "Change Pwd", colors::WHITE);
    }
    btn_x += USER_BTN_W + 8;

    // Delete (disabled when no selection)
    {
        bool enabled = st->selected_user >= 0;
        Color del_bg = enabled ? Color::from_rgb(0xD0, 0x3E, 0x3E) : Color::from_rgb(0xCC, 0xCC, 0xCC);
        c.fill_rounded_rect(btn_x, y, 70, USER_BTN_H, 4, del_bg);
        int tw = text_width("Delete");
        c.text(btn_x + (70 - tw) / 2, y + (USER_BTN_H - sfh) / 2, "Delete", colors::WHITE);
    }
}

bool settings_handle_users_click(Window* win, SettingsState* st, int mx, int cy) {
    if (!st->desktop->is_admin) return false;

    int x = 16;
    int sfh = system_font_height();
    int y = 20;
    int content_w = win->content_w - 2 * x;
    int row_h = user_row_height();

    // User list rows
    for (int i = 0; i < st->user_count; i++) {
        if (cy >= y && cy < y + row_h && mx >= x && mx < x + content_w) {
            st->selected_user = (st->selected_user == i) ? -1 : i;
            return true;
        }
        y += row_h + 4;
    }

    // Separator + gap
    y += 8 + 1 + 16;

    // Status message (if shown, takes space)
    if (settings_status_visible(st)) {
        y += sfh + 8;
    }

    // Action buttons
    int btn_x = x;

    // Add User
    if (mx >= btn_x && mx < btn_x + 90 && cy >= y && cy < y + USER_BTN_H) {
        open_add_user_dialog(st);
        return true;
    }
    btn_x += 98;

    // Change Password
    if (mx >= btn_x && mx < btn_x + USER_BTN_W && cy >= y && cy < y + USER_BTN_H) {
        if (st->selected_user >= 0) open_change_pwd_dialog(st);
        return true;
    }
    btn_x += USER_BTN_W + 8;

    // Delete
    if (mx >= btn_x && mx < btn_x + 70 && cy >= y && cy < y + USER_BTN_H) {
        if (st->selected_user >= 0) open_delete_user_dialog(st);
        return true;
    }

    return false;
}

} // namespace settings_app
