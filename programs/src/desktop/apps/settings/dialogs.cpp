/*
    * dialogs.cpp
    * User-management dialogs for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

static void dialog_append(char* buf, int* len, int max, char ch) {
    if (*len < max - 1) { buf[*len] = ch; (*len)++; buf[*len] = '\0'; }
}

static void dialog_backspace(char* buf, int* len) {
    if (*len > 0) { (*len)--; buf[*len] = '\0'; }
}

static void dialog_close_self(DesktopState* ds, void* app_data) {
    for (int i = 0; i < ds->window_count; i++) {
        if (ds->windows[i].app_data == app_data) {
            desktop_close_window(ds, i);
            return;
        }
    }
}

static void draw_input_field(Canvas& c, int x, int y, int w, int h,
                             const char* label, const char* value,
                             bool focused, bool masked, Color accent) {
    int sfh = system_font_height();
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);

    c.text(x, y, label, dim);
    y += sfh + 2;
    Color border = focused ? accent : colors::BORDER;
    c.fill_rounded_rect(x, y, w, h, 3, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.rect(x, y, w, h, border);

    if (masked) {
        int vlen = montauk::slen(value);
        char dots[65];
        for (int i = 0; i < vlen && i < 64; i++) dots[i] = '*';
        dots[vlen < 64 ? vlen : 64] = '\0';
        c.text(x + 8, y + (h - sfh) / 2, dots, colors::TEXT_COLOR);
    } else {
        c.text(x + 8, y + (h - sfh) / 2, value, colors::TEXT_COLOR);
    }

    if (focused) {
        int tw = masked ? text_width("*") * montauk::slen(value) : text_width(value);
        c.fill_rect(x + 8 + tw, y + 6, 2, h - 12, accent);
    }
}

// ============================================================================
// Add User Dialog
// ============================================================================

struct AddUserDialogState {
    DesktopState* ds;
    SettingsState* parent;
    Color accent;
    int field;              // 0=username, 1=display_name, 2=password
    char username[32];      int username_len;
    char display[64];       int display_len;
    char password[64];      int password_len;
    bool role_admin;
    char error[64];
};

static void adduser_on_draw(Window* win, Framebuffer& fb) {
    AddUserDialogState* st = (AddUserDialogState*)win->app_data;
    if (!st) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);
    Color accent = st->accent;
    int sfh = system_font_height();
    int pad = 16;
    int fw = c.w - 2 * pad;
    int y = pad;

    // Fields
    draw_input_field(c, pad, y, fw, USER_FIELD_H, "Username", st->username,
                     st->field == 0, false, accent);
    y += sfh + 2 + USER_FIELD_H + 10;

    draw_input_field(c, pad, y, fw, USER_FIELD_H, "Display Name", st->display,
                     st->field == 1, false, accent);
    y += sfh + 2 + USER_FIELD_H + 10;

    draw_input_field(c, pad, y, fw, USER_FIELD_H, "Password", st->password,
                     st->field == 2, true, accent);
    y += sfh + 2 + USER_FIELD_H + 12;

    // Role toggle
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    c.text(pad, y + 4, "Role:", dim);
    int rbx = pad + 60;
    draw_toggle_btn(c, rbx, y, 60, USER_BTN_H, "User", !st->role_admin, accent);
    draw_toggle_btn(c, rbx + 68, y, 60, USER_BTN_H, "Admin", st->role_admin, accent);
    y += USER_BTN_H + 14;

    // Error message
    if (st->error[0]) {
        c.text(pad, y, st->error, Color::from_rgb(0xD0, 0x3E, 0x3E));
        y += sfh + 6;
    }

    // Buttons at bottom
    int btn_y = c.h - USER_BTN_H - pad;
    c.fill_rounded_rect(pad, btn_y, 80, USER_BTN_H, 4, accent);
    int tw = text_width("Create");
    c.text(pad + (80 - tw) / 2, btn_y + (USER_BTN_H - sfh) / 2, "Create", colors::WHITE);

    c.fill_rounded_rect(pad + 88, btn_y, 80, USER_BTN_H, 4, colors::WINDOW_BG);
    c.rect(pad + 88, btn_y, 80, USER_BTN_H, colors::BORDER);
    tw = text_width("Cancel");
    c.text(pad + 88 + (80 - tw) / 2, btn_y + (USER_BTN_H - sfh) / 2, "Cancel", colors::TEXT_COLOR);
}

static void adduser_submit(AddUserDialogState* st) {
    if (st->username_len == 0) {
        montauk::strcpy(st->error, "Username is required");
        return;
    }
    if (st->password_len == 0) {
        montauk::strcpy(st->error, "Password is required");
        return;
    }
    const char* dname = st->display_len > 0 ? st->display : st->username;
    const char* role = st->role_admin ? "admin" : "user";
    if (montauk::user::create_user(st->username, dname, st->password, role)) {
        st->parent->users_loaded = false;
        settings_set_status(st->parent, "User created");
        dialog_close_self(st->ds, st);
    } else {
        montauk::strcpy(st->error, "Failed (username taken?)");
    }
}

static void adduser_on_mouse(Window* win, MouseEvent& ev) {
    AddUserDialogState* st = (AddUserDialogState*)win->app_data;
    if (!st || !ev.left_pressed()) return;

    Rect cr = win->content_rect();
    int mx = ev.x - cr.x;
    int my = ev.y - cr.y;
    int pad = 16;
    int sfh = system_font_height();
    int y = pad;

    // Username field hit
    y += sfh + 2;
    if (my >= y && my < y + USER_FIELD_H) { st->field = 0; return; }
    y += USER_FIELD_H + 10;

    // Display field hit
    y += sfh + 2;
    if (my >= y && my < y + USER_FIELD_H) { st->field = 1; return; }
    y += USER_FIELD_H + 10;

    // Password field hit
    y += sfh + 2;
    if (my >= y && my < y + USER_FIELD_H) { st->field = 2; return; }
    y += USER_FIELD_H + 12;

    // Role toggle
    int rbx = pad + 60;
    if (mx >= rbx && mx < rbx + 60 && my >= y && my < y + USER_BTN_H) {
        st->role_admin = false; return;
    }
    if (mx >= rbx + 68 && mx < rbx + 128 && my >= y && my < y + USER_BTN_H) {
        st->role_admin = true; return;
    }

    // Bottom buttons
    int btn_y = win->content_h - USER_BTN_H - pad;
    if (my >= btn_y && my < btn_y + USER_BTN_H) {
        if (mx >= pad && mx < pad + 80) { adduser_submit(st); return; }
        if (mx >= pad + 88 && mx < pad + 168) { dialog_close_self(st->ds, st); return; }
    }
}

static void adduser_on_key(Window* win, const Montauk::KeyEvent& key) {
    AddUserDialogState* st = (AddUserDialogState*)win->app_data;
    if (!st || !key.pressed) return;

    if (key.ascii == '\n' || key.ascii == '\r' || key.scancode == 0x1C) {
        adduser_submit(st); return;
    }
    if (key.scancode == 0x01) { dialog_close_self(st->ds, st); return; }
    if (key.scancode == 0x0F) { st->field = (st->field + 1) % 3; return; }
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        switch (st->field) {
        case 0: dialog_backspace(st->username, &st->username_len); break;
        case 1: dialog_backspace(st->display, &st->display_len); break;
        case 2: dialog_backspace(st->password, &st->password_len); break;
        }
        return;
    }
    if (key.ascii >= 0x20 && key.ascii < 0x7F) {
        switch (st->field) {
        case 0: dialog_append(st->username, &st->username_len, 31, key.ascii); break;
        case 1: dialog_append(st->display, &st->display_len, 63, key.ascii); break;
        case 2: dialog_append(st->password, &st->password_len, 63, key.ascii); break;
        }
    }
}

static void adduser_on_close(Window* win) {
    if (win->app_data) { montauk::mfree(win->app_data); win->app_data = nullptr; }
}

void open_add_user_dialog(SettingsState* parent) {
    DesktopState* ds = parent->desktop;
    int w = 340, h = 340;
    int wx = (ds->screen_w - w) / 2;
    int wy = (ds->screen_h - h) / 2;
    int idx = desktop_create_window(ds, "Add User", wx, wy, w, h);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    AddUserDialogState* st = (AddUserDialogState*)montauk::malloc(sizeof(AddUserDialogState));
    montauk::memset(st, 0, sizeof(AddUserDialogState));
    st->ds = ds;
    st->parent = parent;
    st->accent = ds->settings.accent_color;

    win->app_data = st;
    win->on_draw = adduser_on_draw;
    win->on_mouse = adduser_on_mouse;
    win->on_key = adduser_on_key;
    win->on_close = adduser_on_close;
}

// ============================================================================
// Change Password Dialog
// ============================================================================

struct ChPwdDialogState {
    DesktopState* ds;
    SettingsState* parent;
    Color accent;
    char username[32];
    char display_name[64];
    int field;              // 0=new, 1=confirm
    char new_pwd[64];       int new_len;
    char confirm[64];       int confirm_len;
    char error[64];
};

static void chpwd_on_draw(Window* win, Framebuffer& fb) {
    ChPwdDialogState* st = (ChPwdDialogState*)win->app_data;
    if (!st) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);
    Color accent = st->accent;
    int sfh = system_font_height();
    int pad = 16;
    int fw = c.w - 2 * pad;
    int y = pad;

    // Title
    char title[80];
    snprintf(title, sizeof(title), "Change password for %s", st->display_name);
    c.text(pad, y, title, colors::TEXT_COLOR);
    y += sfh + 12;

    draw_input_field(c, pad, y, fw, USER_FIELD_H, "New Password", st->new_pwd,
                     st->field == 0, true, accent);
    y += sfh + 2 + USER_FIELD_H + 10;

    draw_input_field(c, pad, y, fw, USER_FIELD_H, "Confirm Password", st->confirm,
                     st->field == 1, true, accent);
    y += sfh + 2 + USER_FIELD_H + 12;

    // Error
    if (st->error[0]) {
        c.text(pad, y, st->error, Color::from_rgb(0xD0, 0x3E, 0x3E));
    }

    // Buttons at bottom
    int btn_y = c.h - USER_BTN_H - pad;
    c.fill_rounded_rect(pad, btn_y, 80, USER_BTN_H, 4, accent);
    int tw = text_width("Save");
    c.text(pad + (80 - tw) / 2, btn_y + (USER_BTN_H - sfh) / 2, "Save", colors::WHITE);

    c.fill_rounded_rect(pad + 88, btn_y, 80, USER_BTN_H, 4, colors::WINDOW_BG);
    c.rect(pad + 88, btn_y, 80, USER_BTN_H, colors::BORDER);
    tw = text_width("Cancel");
    c.text(pad + 88 + (80 - tw) / 2, btn_y + (USER_BTN_H - sfh) / 2, "Cancel", colors::TEXT_COLOR);
}

static void chpwd_submit(ChPwdDialogState* st) {
    if (st->new_len == 0) {
        montauk::strcpy(st->error, "Password cannot be empty");
        return;
    }
    if (!montauk::streq(st->new_pwd, st->confirm)) {
        montauk::strcpy(st->error, "Passwords don't match");
        return;
    }
    montauk::user::change_password(st->username, st->new_pwd);
    settings_set_status(st->parent, "Password changed");
    dialog_close_self(st->ds, st);
}

static void chpwd_on_mouse(Window* win, MouseEvent& ev) {
    ChPwdDialogState* st = (ChPwdDialogState*)win->app_data;
    if (!st || !ev.left_pressed()) return;

    Rect cr = win->content_rect();
    int mx = ev.x - cr.x;
    int my = ev.y - cr.y;
    int pad = 16;
    int sfh = system_font_height();
    int y = pad + sfh + 12;

    // New password field
    y += sfh + 2;
    if (my >= y && my < y + USER_FIELD_H) { st->field = 0; return; }
    y += USER_FIELD_H + 10;

    // Confirm field
    y += sfh + 2;
    if (my >= y && my < y + USER_FIELD_H) { st->field = 1; return; }

    // Bottom buttons
    int btn_y = win->content_h - USER_BTN_H - pad;
    if (my >= btn_y && my < btn_y + USER_BTN_H) {
        if (mx >= pad && mx < pad + 80) { chpwd_submit(st); return; }
        if (mx >= pad + 88 && mx < pad + 168) { dialog_close_self(st->ds, st); return; }
    }
}

static void chpwd_on_key(Window* win, const Montauk::KeyEvent& key) {
    ChPwdDialogState* st = (ChPwdDialogState*)win->app_data;
    if (!st || !key.pressed) return;

    if (key.ascii == '\n' || key.ascii == '\r' || key.scancode == 0x1C) {
        chpwd_submit(st); return;
    }
    if (key.scancode == 0x01) { dialog_close_self(st->ds, st); return; }
    if (key.scancode == 0x0F) { st->field = (st->field + 1) % 2; return; }
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        if (st->field == 0) dialog_backspace(st->new_pwd, &st->new_len);
        else dialog_backspace(st->confirm, &st->confirm_len);
        return;
    }
    if (key.ascii >= 0x20 && key.ascii < 0x7F) {
        if (st->field == 0) dialog_append(st->new_pwd, &st->new_len, 63, key.ascii);
        else dialog_append(st->confirm, &st->confirm_len, 63, key.ascii);
    }
}

static void chpwd_on_close(Window* win) {
    if (win->app_data) { montauk::mfree(win->app_data); win->app_data = nullptr; }
}

void open_change_pwd_dialog(SettingsState* parent) {
    if (parent->selected_user < 0) return;
    DesktopState* ds = parent->desktop;
    int w = 340, h = 260;
    int wx = (ds->screen_w - w) / 2;
    int wy = (ds->screen_h - h) / 2;
    int idx = desktop_create_window(ds, "Change Password", wx, wy, w, h);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    ChPwdDialogState* st = (ChPwdDialogState*)montauk::malloc(sizeof(ChPwdDialogState));
    montauk::memset(st, 0, sizeof(ChPwdDialogState));
    st->ds = ds;
    st->parent = parent;
    st->accent = ds->settings.accent_color;
    montauk::strncpy(st->username, parent->users[parent->selected_user].username, 31);
    montauk::strncpy(st->display_name, parent->users[parent->selected_user].display_name, 63);

    win->app_data = st;
    win->on_draw = chpwd_on_draw;
    win->on_mouse = chpwd_on_mouse;
    win->on_key = chpwd_on_key;
    win->on_close = chpwd_on_close;
}

// ============================================================================
// Delete User Dialog
// ============================================================================

struct DeleteUserDialogState {
    DesktopState* ds;
    SettingsState* parent;
    char username[32];
    bool hover_delete, hover_cancel;
};

static void deluser_on_draw(Window* win, Framebuffer& fb) {
    DeleteUserDialogState* st = (DeleteUserDialogState*)win->app_data;
    if (!st) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);
    int sfh = system_font_height();

    char msg[96];
    snprintf(msg, sizeof(msg), "Delete user \"%s\"?", st->username);
    int tw = text_width(msg);
    c.text((c.w - tw) / 2, 24, msg, colors::TEXT_COLOR);

    const char* warn = "This action cannot be undone.";
    tw = text_width(warn);
    c.text((c.w - tw) / 2, 24 + sfh + 8, warn, Color::from_rgb(0x88, 0x88, 0x88));

    // Buttons
    int btn_w = 100, btn_h = 32;
    int btn_y = c.h - btn_h - 20;
    int gap = 20;
    int total = btn_w * 2 + gap;
    int bx = (c.w - total) / 2;

    Color del_bg = st->hover_delete
        ? Color::from_rgb(0xDD, 0x44, 0x44)
        : Color::from_rgb(0xD0, 0x3E, 0x3E);
    c.button(bx, btn_y, btn_w, btn_h, "Delete", del_bg, colors::WHITE, 4);

    Color cancel_bg = st->hover_cancel
        ? Color::from_rgb(0x99, 0x99, 0x99)
        : Color::from_rgb(0x88, 0x88, 0x88);
    c.button(bx + btn_w + gap, btn_y, btn_w, btn_h, "Cancel", cancel_bg, colors::WHITE, 4);
}

static void deluser_on_mouse(Window* win, MouseEvent& ev) {
    DeleteUserDialogState* st = (DeleteUserDialogState*)win->app_data;
    if (!st) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    int btn_w = 100, btn_h = 32;
    int btn_y = win->content_h - btn_h - 20;
    int gap = 20;
    int total = btn_w * 2 + gap;
    int bx = (win->content_w - total) / 2;

    Rect db = {bx, btn_y, btn_w, btn_h};
    Rect cb = {bx + btn_w + gap, btn_y, btn_w, btn_h};
    st->hover_delete = db.contains(lx, ly);
    st->hover_cancel = cb.contains(lx, ly);

    if (ev.left_pressed()) {
        if (st->hover_delete) {
            montauk::user::delete_user(st->username);
            st->parent->users_loaded = false;
            st->parent->selected_user = -1;
            settings_set_status(st->parent, "User deleted");
            dialog_close_self(st->ds, st);
            return;
        }
        if (st->hover_cancel) {
            dialog_close_self(st->ds, st);
            return;
        }
    }
}

static void deluser_on_key(Window* win, const Montauk::KeyEvent& key) {
    DeleteUserDialogState* st = (DeleteUserDialogState*)win->app_data;
    if (!st || !key.pressed) return;

    if (key.ascii == '\n' || key.ascii == '\r') {
        montauk::user::delete_user(st->username);
        st->parent->users_loaded = false;
        st->parent->selected_user = -1;
        settings_set_status(st->parent, "User deleted");
        dialog_close_self(st->ds, st);
    }
    if (key.scancode == 0x01) {
        dialog_close_self(st->ds, st);
    }
}

static void deluser_on_close(Window* win) {
    if (win->app_data) { montauk::mfree(win->app_data); win->app_data = nullptr; }
}

void open_delete_user_dialog(SettingsState* parent) {
    if (parent->selected_user < 0) return;
    DesktopState* ds = parent->desktop;

    // Don't allow deleting yourself
    if (montauk::streq(parent->users[parent->selected_user].username, ds->current_user)) {
        settings_set_status(parent, "Cannot delete current user");
        return;
    }

    int w = 300, h = 150;
    int wx = (ds->screen_w - w) / 2;
    int wy = (ds->screen_h - h) / 2;
    int idx = desktop_create_window(ds, "Delete User", wx, wy, w, h);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    DeleteUserDialogState* st = (DeleteUserDialogState*)montauk::malloc(sizeof(DeleteUserDialogState));
    montauk::memset(st, 0, sizeof(DeleteUserDialogState));
    st->ds = ds;
    st->parent = parent;
    montauk::strncpy(st->username, parent->users[parent->selected_user].username, 31);

    win->app_data = st;
    win->on_draw = deluser_on_draw;
    win->on_mouse = deluser_on_mouse;
    win->on_key = deluser_on_key;
    win->on_close = deluser_on_close;
}

} // namespace settings_app
