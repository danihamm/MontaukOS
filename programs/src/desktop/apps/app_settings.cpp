/*
    * app_settings.cpp
    * MontaukOS Desktop - Settings application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"
#include "../wallpaper.hpp"
#include <montauk/config.h>
#include <montauk/user.h>

// ============================================================================
// Settings state
// ============================================================================

struct SettingsState {
    DesktopState* desktop;
    int active_tab;        // 0=Appearance, 1=Display, 2=Users, 3=About
    Montauk::SysInfo sys_info;
    uint64_t uptime_ms;
    WallpaperFileList wp_files;
    bool wp_scanned;

    // Users tab
    montauk::user::UserInfo users[16];
    int user_count;
    int selected_user;     // index into users[], or -1
    bool users_loaded;

    char status_msg[128];
    uint64_t status_time;
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
static constexpr int TAB_COUNT = 4;
static const char* tab_labels[TAB_COUNT] = { "Appearance", "Display", "Users", "About" };

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

static constexpr int WP_ITEM_H = 24;

static void settings_draw_appearance(Canvas& c, SettingsState* st) {
    DesktopSettings& s = st->desktop->settings;
    Color accent = s.accent_color;
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    int x = 16;
    int y = 12;
    int sfh = system_font_height();
    int line_h = sfh + 10;

    // Section: Background
    c.text(x, y, "Background", colors::TEXT_COLOR);
    y += line_h;

    // Radio buttons: Gradient / Solid Color / Image
    bool mode_grad  = s.bg_gradient && !s.bg_image;
    bool mode_solid = !s.bg_gradient && !s.bg_image;
    bool mode_image = s.bg_image;

    draw_radio(c, x, y, mode_grad, accent);
    c.text(x + 20, y + 2, "Gradient", colors::TEXT_COLOR);

    draw_radio(c, x + 120, y, mode_solid, accent);
    c.text(x + 140, y + 2, "Solid", colors::TEXT_COLOR);

    draw_radio(c, x + 220, y, mode_image, accent);
    c.text(x + 240, y + 2, "Image", colors::TEXT_COLOR);
    y += line_h + 4;

    if (mode_image) {
        // Scan for images lazily
        if (!st->wp_scanned) {
            wallpaper_scan_dir(st->desktop->home_dir, &st->wp_files);
            st->wp_scanned = true;
        }

        c.text(x, y, "Wallpapers", dim);
        y += sfh + 6;

        if (st->wp_files.count == 0) {
            c.text(x + 8, y, "No .jpg files found", dim);
            y += WP_ITEM_H;
        } else {
            for (int i = 0; i < st->wp_files.count && i < 8; i++) {
                // Build full path for comparison
                char fullpath[256];
                montauk::strcpy(fullpath, st->desktop->home_dir);
                str_append(fullpath, "/", 256);
                str_append(fullpath, st->wp_files.names[i], 256);

                bool selected = s.bg_image &&
                    montauk::streq(s.bg_image_path, fullpath);

                if (selected) {
                    c.fill_rounded_rect(x, y, c.w - 2 * x, WP_ITEM_H, 3, accent);
                }

                // Truncate long filenames
                char label[40];
                int nlen = montauk::slen(st->wp_files.names[i]);
                if (nlen > 35) {
                    montauk::strncpy(label, st->wp_files.names[i], 32);
                    label[32] = '.';
                    label[33] = '.';
                    label[34] = '.';
                    label[35] = '\0';
                } else {
                    montauk::strncpy(label, st->wp_files.names[i], 39);
                }

                Color tc = selected ? colors::WHITE : colors::TEXT_COLOR;
                c.text(x + 8, y + (WP_ITEM_H - sfh) / 2, label, tc);
                y += WP_ITEM_H;
            }
        }
        y += 6;
    } else if (mode_grad) {
        // Top color swatches
        c.text(x, y + 4, "Top", dim);
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_grad_top, accent);
        y += SWATCH_SIZE + 14;

        // Bottom color swatches
        c.text(x, y + 4, "Bottom", dim);
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_grad_bottom, accent);
        y += SWATCH_SIZE + 14;
    } else {
        // Solid color swatches
        c.text(x, y + 4, "Color", dim);
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
    st->uptime_ms = montauk::get_milliseconds();

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

    // snprintf(line, sizeof(line), "Build: %s %s", __DATE__, __TIME__);
    // c.text(x, y, line, colors::TEXT_COLOR);
    y += 8;

    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    c.text(x, y, "Copyright (c) 2026 Daniel Hammer", dim);
}

// ============================================================================
// Config persistence
// ============================================================================

static int64_t color_to_int(Color c) {
    return ((int64_t)c.r << 16) | ((int64_t)c.g << 8) | c.b;
}

static Color int_to_color(int64_t v) {
    return Color::from_rgb((uint8_t)((v >> 16) & 0xFF),
                           (uint8_t)((v >> 8) & 0xFF),
                           (uint8_t)(v & 0xFF));
}

static void settings_persist(SettingsState* st) {
    DesktopSettings& s = st->desktop->settings;
    const char* user = st->desktop->current_user;

    montauk::toml::Doc doc;
    doc.init();

    // Background mode
    const char* mode = s.bg_image ? "image" : (s.bg_gradient ? "gradient" : "solid");
    montauk::config::set_string(&doc, "background.mode", mode);

    // Wallpaper path
    if (s.bg_image && s.bg_image_path[0])
        montauk::config::set_string(&doc, "wallpaper.path", s.bg_image_path);

    // Background colors
    montauk::config::set_int(&doc, "background.solid_color", color_to_int(s.bg_solid));
    montauk::config::set_int(&doc, "background.grad_top", color_to_int(s.bg_grad_top));
    montauk::config::set_int(&doc, "background.grad_bottom", color_to_int(s.bg_grad_bottom));

    // Appearance
    montauk::config::set_int(&doc, "appearance.panel_color", color_to_int(s.panel_color));
    montauk::config::set_int(&doc, "appearance.accent_color", color_to_int(s.accent_color));

    // Display
    montauk::config::set_int(&doc, "display.ui_scale", s.ui_scale);
    montauk::config::set_bool(&doc, "display.clock_24h", s.clock_24h);
    montauk::config::set_bool(&doc, "display.show_shadows", s.show_shadows);

    montauk::config::save_user(user, "desktop", &doc);
    doc.destroy();
}

// ============================================================================
// Users tab
// ============================================================================

static void users_reload(SettingsState* st) {
    st->user_count = montauk::user::load_users(st->users, 16);
    st->users_loaded = true;
}

static constexpr int USER_BTN_H = 30;
static constexpr int USER_BTN_W = 100;
static constexpr int USER_FIELD_H = 32;

// Forward declarations for dialog openers
static void open_add_user_dialog(SettingsState* st);
static void open_change_pwd_dialog(SettingsState* st);
static void open_delete_user_dialog(SettingsState* st);

static int user_row_height() {
    return system_font_height() * 2 + 12;  // Two lines of text + padding
}

static void settings_draw_users(Canvas& c, SettingsState* st) {
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
    if (st->status_msg[0] && (montauk::get_milliseconds() - st->status_time < 3000)) {
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

// ============================================================================
// Dialog helpers
// ============================================================================

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
        montauk::strcpy(st->parent->status_msg, "User created");
        st->parent->status_time = montauk::get_milliseconds();
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
    int fw = win->content_w - 2 * pad;
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

static void open_add_user_dialog(SettingsState* parent) {
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
    montauk::strcpy(st->parent->status_msg, "Password changed");
    st->parent->status_time = montauk::get_milliseconds();
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

static void open_change_pwd_dialog(SettingsState* parent) {
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
            montauk::strcpy(st->parent->status_msg, "User deleted");
            st->parent->status_time = montauk::get_milliseconds();
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
        montauk::strcpy(st->parent->status_msg, "User deleted");
        st->parent->status_time = montauk::get_milliseconds();
        dialog_close_self(st->ds, st);
    }
    if (key.scancode == 0x01) {
        dialog_close_self(st->ds, st);
    }
}

static void deluser_on_close(Window* win) {
    if (win->app_data) { montauk::mfree(win->app_data); win->app_data = nullptr; }
}

static void open_delete_user_dialog(SettingsState* parent) {
    if (parent->selected_user < 0) return;
    DesktopState* ds = parent->desktop;

    // Don't allow deleting yourself
    if (montauk::streq(parent->users[parent->selected_user].username, ds->current_user)) {
        montauk::strcpy(parent->status_msg, "Cannot delete current user");
        parent->status_time = montauk::get_milliseconds();
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
    case 2: settings_draw_users(content, st); break;
    case 3: settings_draw_about(content, st); break;
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

        bool mode_grad  = s.bg_gradient && !s.bg_image;
        bool mode_image = s.bg_image;

        // "Background" label
        y += line_h;

        // Radio: Gradient
        if (mx >= x && mx < x + 100 && cy >= y && cy < y + 16) {
            s.bg_gradient = true;
            s.bg_image = false;
            settings_persist(st);
            return;
        }
        // Radio: Solid
        if (mx >= x + 120 && mx < x + 210 && cy >= y && cy < y + 16) {
            s.bg_gradient = false;
            s.bg_image = false;
            settings_persist(st);
            return;
        }
        // Radio: Image
        if (mx >= x + 220 && mx < x + 320 && cy >= y && cy < y + 16) {
            s.bg_image = true;
            s.bg_gradient = false;
            if (!st->wp_scanned) {
                wallpaper_scan_dir(st->desktop->home_dir, &st->wp_files);
                st->wp_scanned = true;
            }
            settings_persist(st);
            return;
        }
        y += line_h + 4;

        int idx;
        if (mode_image) {
            // Wallpaper file list
            y += sfh + 6;

            // File list clicks
            for (int i = 0; i < st->wp_files.count && i < 8; i++) {
                if (cy >= y && cy < y + WP_ITEM_H &&
                    mx >= x && mx < win->content_w - x) {
                    // Build full path and load wallpaper
                    char fullpath[256];
                    montauk::strcpy(fullpath, st->desktop->home_dir);
                    str_append(fullpath, "/", 256);
                    str_append(fullpath, st->wp_files.names[i], 256);
                    wallpaper_load(&s, fullpath,
                                   st->desktop->screen_w, st->desktop->screen_h);
                    settings_persist(st);
                    return;
                }
                y += WP_ITEM_H;
            }
            if (st->wp_files.count == 0) y += WP_ITEM_H;
            y += 6;
        } else if (mode_grad) {
            // Top swatches
            if (swatch_hit(mx, cy, x + 70, y, &idx)) {
                s.bg_grad_top = bg_palette[idx];
                settings_persist(st);
                return;
            }
            y += SWATCH_SIZE + 14;

            // Bottom swatches
            if (swatch_hit(mx, cy, x + 70, y, &idx)) {
                s.bg_grad_bottom = bg_palette[idx];
                settings_persist(st);
                return;
            }
            y += SWATCH_SIZE + 14;
        } else {
            // Solid color swatches
            if (swatch_hit(mx, cy, x + 70, y, &idx)) {
                s.bg_solid = bg_palette[idx];
                settings_persist(st);
                return;
            }
            y += SWATCH_SIZE + 14;
        }

        // Separator
        y += 12;

        // Panel color swatches
        if (swatch_hit(mx, cy, x + 110, y, &idx)) {
            s.panel_color = panel_palette[idx];
            settings_persist(st);
            return;
        }
        y += SWATCH_SIZE + 14;

        // Separator
        y += 12;

        // Accent color swatches
        if (swatch_hit(mx, cy, x + 110, y, &idx)) {
            s.accent_color = accent_palette[idx];
            settings_persist(st);
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
            settings_persist(st);
            return;
        }
        // Window Shadows: Off
        if (mx >= bx + btn_w + 8 && mx < bx + btn_w * 2 + 8 && cy >= y && cy < y + btn_h) {
            s.show_shadows = false;
            settings_persist(st);
            return;
        }
        y += btn_h + 20 + 16;

        // Clock: 24h
        if (mx >= bx && mx < bx + btn_w && cy >= y && cy < y + btn_h) {
            s.clock_24h = true;
            settings_persist(st);
            return;
        }
        // Clock: 12h
        if (mx >= bx + btn_w + 8 && mx < bx + btn_w * 2 + 8 && cy >= y && cy < y + btn_h) {
            s.clock_24h = false;
            settings_persist(st);
            return;
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
            return;
        }
        // Default
        if (mx >= bx + sbw + 8 && mx < bx + sbw * 2 + 8 && cy >= y && cy < y + btn_h) {
            s.ui_scale = 1;
            apply_ui_scale(1);
            montauk::win_setscale(1);
            settings_persist(st);
            return;
        }
        // Large
        if (mx >= bx + (sbw + 8) * 2 && mx < bx + sbw * 3 + 16 && cy >= y && cy < y + btn_h) {
            s.ui_scale = 2;
            apply_ui_scale(2);
            montauk::win_setscale(2);
            settings_persist(st);
            return;
        }
    } else if (st->active_tab == 2) {
        // Users tab
        if (!st->desktop->is_admin) return;

        int x = 16;
        int sfh = system_font_height();
        int y = 20;
        int content_w = win->content_w - 2 * x;
        int row_h = user_row_height();

        // User list rows
        for (int i = 0; i < st->user_count; i++) {
            if (cy >= y && cy < y + row_h && mx >= x && mx < x + content_w) {
                st->selected_user = (st->selected_user == i) ? -1 : i;
                return;
            }
            y += row_h + 4;
        }

        // Separator + gap
        y += 8 + 1 + 16;

        // Status message (if shown, takes space)
        if (st->status_msg[0] && (montauk::get_milliseconds() - st->status_time < 3000)) {
            y += sfh + 8;
        }

        // Action buttons
        int btn_x = x;

        // Add User
        if (mx >= btn_x && mx < btn_x + 90 && cy >= y && cy < y + USER_BTN_H) {
            open_add_user_dialog(st);
            return;
        }
        btn_x += 98;

        // Change Password
        if (mx >= btn_x && mx < btn_x + USER_BTN_W && cy >= y && cy < y + USER_BTN_H) {
            if (st->selected_user >= 0) open_change_pwd_dialog(st);
            return;
        }
        btn_x += USER_BTN_W + 8;

        // Delete
        if (mx >= btn_x && mx < btn_x + 70 && cy >= y && cy < y + USER_BTN_H) {
            if (st->selected_user >= 0) open_delete_user_dialog(st);
            return;
        }
    }
}

// ============================================================================
// Keyboard
// ============================================================================

static void settings_on_key(Window* win, const Montauk::KeyEvent& key) {
    // Settings main window no longer needs keyboard handling —
    // text input is handled by dialog windows.
    (void)win; (void)key;
}

// ============================================================================
// Cleanup
// ============================================================================

static void settings_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
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
    SettingsState* st = (SettingsState*)montauk::malloc(sizeof(SettingsState));
    montauk::memset(st, 0, sizeof(SettingsState));
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
    win->on_draw = settings_on_draw;
    win->on_mouse = settings_on_mouse;
    win->on_key = settings_on_key;
    win->on_close = settings_on_close;
}
