/*
    * desktop_internal.hpp
    * Shared declarations for desktop source files
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include "apps/apps_common.hpp"
#include "wallpaper.hpp"
#include <montauk/toml.h>

// ============================================================================
// App Menu Data (dynamic — built at startup from embedded + external apps)
// ============================================================================

static constexpr int MENU_W = 220;
static constexpr int MENU_ITEM_H = 36;
static constexpr int MENU_CAT_H = 32;
static constexpr int MENU_DIV_H = 10;

struct MenuRow {
    bool is_category;
    char label[48];
    int  app_id;            // embedded dispatch ID, or -1 for categories/dividers
    bool external;          // true = spawn binary_path
    char binary_path[128];  // full VFS path for external apps
    SvgIcon* icon;          // loaded menu icon (null for categories/dividers)
};

static constexpr int MAX_MENU_ROWS = 48;
inline MenuRow menu_rows[MAX_MENU_ROWS];
inline int menu_row_count = 0;

// Collapsible category state (categories 0-3 are toggleable; divider category always expanded)
static constexpr int MENU_NUM_CATS = 5;
inline bool menu_cat_expanded[MENU_NUM_CATS] = { false, false, false, false, true };

inline int menu_get_cat(int row_idx) {
    int cat = -1;
    for (int i = 0; i <= row_idx; i++)
        if (menu_rows[i].is_category) cat++;
    return cat;
}

inline bool menu_row_visible(int row_idx) {
    const MenuRow& row = menu_rows[row_idx];
    if (row.is_category) return true;
    int cat = menu_get_cat(row_idx);
    if (cat < 0 || cat >= MENU_NUM_CATS) return true;
    return menu_cat_expanded[cat];
}

inline int menu_row_height(const MenuRow& row) {
    if (!row.is_category) return MENU_ITEM_H;
    return row.label[0] ? MENU_CAT_H : MENU_DIV_H;
}

inline int menu_total_height() {
    int h = 10; // top + bottom padding
    for (int i = 0; i < menu_row_count; i++)
        if (menu_row_visible(i))
            h += menu_row_height(menu_rows[i]);
    return h;
}

// ============================================================================
// Menu Builder Helpers
// ============================================================================

inline int menu_add_category(const char* label) {
    if (menu_row_count >= MAX_MENU_ROWS) return -1;
    MenuRow& r = menu_rows[menu_row_count++];
    r.is_category = true;
    montauk::strncpy(r.label, label, sizeof(r.label));
    r.app_id = -1;
    r.external = false;
    r.binary_path[0] = '\0';
    r.icon = nullptr;
    return menu_row_count - 1;
}

inline int menu_add_embedded(const char* label, int app_id, SvgIcon* icon) {
    if (menu_row_count >= MAX_MENU_ROWS) return -1;
    MenuRow& r = menu_rows[menu_row_count++];
    r.is_category = false;
    montauk::strncpy(r.label, label, sizeof(r.label));
    r.app_id = app_id;
    r.external = false;
    r.binary_path[0] = '\0';
    r.icon = icon;
    return menu_row_count - 1;
}

inline int menu_add_external(const char* label, const char* binary, SvgIcon* icon) {
    if (menu_row_count >= MAX_MENU_ROWS) return -1;
    MenuRow& r = menu_rows[menu_row_count++];
    r.is_category = false;
    montauk::strncpy(r.label, label, sizeof(r.label));
    r.app_id = -1;
    r.external = true;
    montauk::strncpy(r.binary_path, binary, sizeof(r.binary_path));
    r.icon = icon;
    return menu_row_count - 1;
}

// ============================================================================
// Forward Declarations (internal functions)
// ============================================================================

// window.cpp
gui::ResizeEdge hit_test_resize_edge(const gui::Rect& f, int mx, int my);
gui::CursorStyle cursor_for_edge(gui::ResizeEdge edge);

// panel.cpp
void desktop_draw_app_menu(gui::DesktopState* ds);
void desktop_draw_net_popup(gui::DesktopState* ds);
void desktop_draw_vol_popup(gui::DesktopState* ds);

// compose.cpp
void desktop_draw_lock_screen(gui::DesktopState* ds);

// main.cpp
void desktop_scan_apps(gui::DesktopState* ds);
void desktop_build_menu(gui::DesktopState* ds);

// Month names (shared by panel clock)
inline const char* month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
