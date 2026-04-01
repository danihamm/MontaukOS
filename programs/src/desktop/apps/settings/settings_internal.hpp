/*
    * settings_internal.hpp
    * Shared declarations for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include "../apps_common.hpp"
#include "../../wallpaper.hpp"
#include <montauk/config.h>
#include <montauk/user.h>

namespace settings_app {

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

inline constexpr int SWATCH_COUNT = 8;
inline constexpr int SWATCH_SIZE = 24;
inline constexpr int SWATCH_GAP = 6;

// Background colors (light tones)
inline const Color bg_palette[SWATCH_COUNT] = {
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
inline const Color panel_palette[SWATCH_COUNT] = {
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
inline const Color accent_palette[SWATCH_COUNT] = {
    Color::from_rgb(0x36, 0x7B, 0xF0),  // blue (default)
    Color::from_rgb(0x00, 0x9B, 0x9B),  // teal
    Color::from_rgb(0x2E, 0x9E, 0x3E),  // green
    Color::from_rgb(0xE0, 0x8A, 0x20),  // orange
    Color::from_rgb(0xD0, 0x3E, 0x3E),  // red
    Color::from_rgb(0x7B, 0x3E, 0xB8),  // purple
    Color::from_rgb(0xD0, 0x5C, 0x9E),  // pink
    Color::from_rgb(0x44, 0x44, 0xCC),  // indigo
};

inline constexpr int TAB_BAR_H = 36;
inline constexpr int TAB_COUNT = 4;
inline constexpr const char* tab_labels[TAB_COUNT] = { "Appearance", "Display", "Users", "About" };

inline constexpr int WP_ITEM_H = 24;
inline constexpr int USER_BTN_H = 30;
inline constexpr int USER_BTN_W = 100;
inline constexpr int USER_FIELD_H = 32;

bool color_eq(Color a, Color b);
bool settings_status_visible(SettingsState* st);
void settings_set_status(SettingsState* st, const char* msg);
int find_swatch(const Color* palette, Color current);

void draw_swatch_row(Canvas& c, int x, int y, const Color* palette, Color selected, Color accent);
void draw_radio(Canvas& c, int x, int y, bool selected, Color accent);
void draw_toggle_btn(Canvas& c, int x, int y, int bw, int bh,
                     const char* label, bool active, Color accent);
bool swatch_hit(int mx, int my, int row_x, int row_y, int* out_idx);

void apply_ui_scale(int scale);

void settings_draw_appearance(Canvas& c, SettingsState* st);
bool settings_handle_appearance_click(Window* win, SettingsState* st, int mx, int cy);

void settings_draw_display(Canvas& c, SettingsState* st);
bool settings_handle_display_click(SettingsState* st, int mx, int cy);

void settings_draw_users(Canvas& c, SettingsState* st);
bool settings_handle_users_click(Window* win, SettingsState* st, int mx, int cy);

void settings_draw_about(Canvas& c, SettingsState* st);

void settings_persist(SettingsState* st);
void settings_persist_tz(SettingsState* st);

void users_reload(SettingsState* st);
int user_row_height();

void open_add_user_dialog(SettingsState* parent);
void open_change_pwd_dialog(SettingsState* parent);
void open_delete_user_dialog(SettingsState* parent);

} // namespace settings_app
