/*
    * launcher.cpp
    * Super + Space launcher
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

static constexpr int LAUNCHER_W = 560;
static constexpr int LAUNCHER_SEARCH_H = 52;
static constexpr int LAUNCHER_ROW_H = 52;
static constexpr int LAUNCHER_EMPTY_H = 84;
static constexpr int LAUNCHER_VISIBLE_ROWS = 7;
static constexpr int LAUNCHER_TEXT_PAD = 18;
static constexpr int LAUNCHER_CURSOR_BLINK_MS = 700;
static constexpr int LAUNCHER_SPECIAL_FOLDER_COUNT = 6;

static constexpr Color LAUNCHER_BORDER      = Color::from_rgb(0xD4, 0xDB, 0xE4);
static constexpr Color LAUNCHER_FIELD_BG    = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color LAUNCHER_SELECTED_BG = Color::from_rgb(0xDA, 0xE7, 0xFD);
static constexpr Color LAUNCHER_HOVER_BG    = Color::from_rgb(0xEC, 0xF3, 0xFE);
static constexpr Color LAUNCHER_MUTED       = Color::from_rgb(0x6F, 0x7B, 0x89);
static constexpr Color LAUNCHER_PLACEHOLDER = Color::from_rgb(0x98, 0xA1, 0xAD);

static const char* launcher_special_folder_names[LAUNCHER_SPECIAL_FOLDER_COUNT] = {
    "Documents", "Desktop", "Music", "Videos", "Pictures", "Downloads"
};

static char fold_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool launcher_super_down(const DesktopState* ds) {
    return ds->super_left_down || ds->super_right_down;
}

static bool launcher_show_results(const DesktopState* ds) {
    return ds->launcher_query_len > 0;
}

static int launcher_search_y(const DesktopState* ds) {
    int avail_h = ds->screen_h - PANEL_HEIGHT;
    int y = PANEL_HEIGHT + ((avail_h - LAUNCHER_SEARCH_H) * 2) / 10;
    if (y < PANEL_HEIGHT + 8) y = PANEL_HEIGHT + 8;
    if (y + LAUNCHER_SEARCH_H > ds->screen_h - 8) y = ds->screen_h - LAUNCHER_SEARCH_H - 8;
    return y;
}

static int launcher_visible_rows(const DesktopState* ds) {
    if (ds->launcher_result_count <= 0) return 0;
    return gui_min(ds->launcher_result_count, LAUNCHER_VISIBLE_ROWS);
}

static int launcher_scroll_max(const DesktopState* ds) {
    int extra = ds->launcher_result_count - LAUNCHER_VISIBLE_ROWS;
    return extra > 0 ? extra : 0;
}

static Rect launcher_bounds(const DesktopState* ds) {
    int w = gui_min(LAUNCHER_W, ds->screen_w - 16);
    if (w < 260) w = ds->screen_w;
    int result_h = launcher_show_results(ds)
        ? (ds->launcher_result_count > 0
        ? launcher_visible_rows(ds) * LAUNCHER_ROW_H
        : LAUNCHER_EMPTY_H)
        : 0;
    int h = LAUNCHER_SEARCH_H + result_h;
    int x = (ds->screen_w - w) / 2;
    int y = launcher_search_y(ds);
    return {x, y, w, h};
}

static Rect launcher_search_rect(const DesktopState* ds) {
    Rect bounds = launcher_bounds(ds);
    return {bounds.x, launcher_search_y(ds), bounds.w, LAUNCHER_SEARCH_H};
}

static Rect launcher_results_rect(const DesktopState* ds) {
    Rect search = launcher_search_rect(ds);
    Rect bounds = launcher_bounds(ds);
    int y = search.y + search.h;
    return {
        bounds.x,
        y,
        bounds.w,
        bounds.y + bounds.h - y
    };
}

static int launcher_visible_result_count(const DesktopState* ds) {
    if (!launcher_show_results(ds)) return 0;
    int remaining = ds->launcher_result_count - ds->launcher_scroll;
    if (remaining <= 0) return 0;
    return gui_min(remaining, LAUNCHER_VISIBLE_ROWS);
}

static Rect launcher_row_rect(const DesktopState* ds, int visible_idx) {
    Rect list = launcher_results_rect(ds);
    return {list.x, list.y + visible_idx * LAUNCHER_ROW_H, list.w, LAUNCHER_ROW_H};
}

static bool launcher_contains_folded(const char* haystack, const char* needle) {
    if (!needle[0]) return true;

    int nlen = montauk::slen(needle);
    if (nlen == 0) return true;

    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (j < nlen && haystack[i + j] &&
               fold_ascii(haystack[i + j]) == fold_ascii(needle[j])) {
            j++;
        }
        if (j == nlen) return true;
    }

    return false;
}

static void launcher_append_search_term(char* dst, int dst_max, const char* term) {
    if (!term || term[0] == '\0') return;
    if (dst[0] != '\0') str_append(dst, " ", dst_max);
    str_append(dst, term, dst_max);
}

static void launcher_build_search_text(char* dst, int dst_max,
                                       const char* name, const char* category,
                                       const char* path, const char* keywords) {
    dst[0] = '\0';
    launcher_append_search_term(dst, dst_max, name);
    launcher_append_search_term(dst, dst_max, category);
    launcher_append_search_term(dst, dst_max, path);
    launcher_append_search_term(dst, dst_max, keywords);
}

static bool launcher_item_matches(const LauncherItem* item, const char* query) {
    if (!query[0]) return false;
    return launcher_contains_folded(item->search_text, query);
}

static int launcher_compare_items(const LauncherItem* a, const LauncherItem* b) {
    for (int i = 0; a->name[i] || b->name[i]; i++) {
        char ca = fold_ascii(a->name[i]);
        char cb = fold_ascii(b->name[i]);
        if (ca != cb) return (int)((unsigned char)ca) - (int)((unsigned char)cb);
    }

    for (int i = 0; a->category[i] || b->category[i]; i++) {
        char ca = fold_ascii(a->category[i]);
        char cb = fold_ascii(b->category[i]);
        if (ca != cb) return (int)((unsigned char)ca) - (int)((unsigned char)cb);
    }

    return 0;
}

static void launcher_add_item(DesktopState* ds, LauncherItemKind kind,
                              const char* name, const char* category,
                              SvgIcon* icon, int ref_index,
                              const char* path = nullptr,
                              const char* keywords = nullptr) {
    if (ds->launcher_item_count >= MAX_LAUNCHER_ITEMS) return;

    LauncherItem* item = &ds->launcher_items[ds->launcher_item_count++];
    montauk::memset(item, 0, sizeof(LauncherItem));
    montauk::strncpy(item->name, name ? name : "", sizeof(item->name));
    montauk::strncpy(item->category, category ? category : "", sizeof(item->category));
    if (path) montauk::strncpy(item->path, path, sizeof(item->path));
    launcher_build_search_text(item->search_text, sizeof(item->search_text),
                               item->name, item->category, item->path, keywords);
    item->icon = icon ? icon : &ds->icon_exec;
    item->kind = kind;
    item->ref_index = ref_index;
}

static void desktop_launch_external_app(DesktopState* ds, const ExternalApp* app) {
    if (!app) return;

    if (app->launch_with_home) {
        montauk::spawn(app->binary_path, ds->home_dir);
    } else {
        montauk::spawn(app->binary_path);
    }
}

static void desktop_build_launcher_items(DesktopState* ds) {
    ds->launcher_item_count = 0;

    ensure_filemanager_icons_loaded(ds);

    launcher_add_item(ds, LAUNCHER_ITEM_BUILTIN_FILES,
                      "Files", "Built-in App",
                      &ds->icon_filemanager, -1, nullptr,
                      "file manager browser");

    for (int i = 0; i < ds->external_app_count; i++) {
        ExternalApp* app = &ds->external_apps[i];
        SvgIcon* icon = app->icon.pixels ? &app->icon : &ds->icon_exec;
        launcher_add_item(ds, LAUNCHER_ITEM_EXTERNAL_APP,
                          app->name, app->category,
                          icon, i, nullptr, nullptr);
    }

    launcher_add_item(ds, LAUNCHER_ITEM_COMMAND_REBOOT,
                      "Reboot", "Command",
                      &ds->icon_reboot, -1, nullptr,
                      "restart reboot power");
    launcher_add_item(ds, LAUNCHER_ITEM_COMMAND_SHUTDOWN,
                      "Shutdown", "Command",
                      &ds->icon_shutdown, -1, nullptr,
                      "power off halt shutdown");

    if (ds->home_dir[0] != '\0') {
        for (int sf = 0; sf < LAUNCHER_SPECIAL_FOLDER_COUNT; sf++) {
            char folder_path[128];
            montauk::strcpy(folder_path, ds->home_dir);
            int plen = montauk::slen(folder_path);
            if (plen > 0 && folder_path[plen - 1] != '/')
                str_append(folder_path, "/", sizeof(folder_path));
            str_append(folder_path, launcher_special_folder_names[sf], sizeof(folder_path));

            int probe_fd = montauk::open(folder_path);
            if (probe_fd < 0) continue;
            montauk::close(probe_fd);

            SvgIcon* icon = ds->icon_special_folder[sf].pixels
                ? &ds->icon_special_folder[sf]
                : &ds->icon_folder;
            launcher_add_item(ds, LAUNCHER_ITEM_SPECIAL_FOLDER,
                              launcher_special_folder_names[sf], "Folder",
                              icon, sf, folder_path, "user folder");
        }
    }
}

static void desktop_update_launcher_results(DesktopState* ds) {
    int selected_item = -1;
    if (ds->launcher_selected >= 0 && ds->launcher_selected < ds->launcher_result_count) {
        selected_item = ds->launcher_results[ds->launcher_selected];
    }

    ds->launcher_result_count = 0;
    if (ds->launcher_query[0] == '\0') {
        ds->launcher_selected = -1;
        ds->launcher_scroll = 0;
        return;
    }

    for (int i = 0; i < ds->launcher_item_count; i++) {
        if (!launcher_item_matches(&ds->launcher_items[i], ds->launcher_query)) continue;
        ds->launcher_results[ds->launcher_result_count++] = i;
    }

    for (int i = 1; i < ds->launcher_result_count; i++) {
        int item_idx = ds->launcher_results[i];
        int j = i - 1;
        while (j >= 0 &&
               launcher_compare_items(&ds->launcher_items[item_idx],
                                      &ds->launcher_items[ds->launcher_results[j]]) < 0) {
            ds->launcher_results[j + 1] = ds->launcher_results[j];
            j--;
        }
        ds->launcher_results[j + 1] = item_idx;
    }

    if (ds->launcher_result_count <= 0) {
        ds->launcher_selected = -1;
        ds->launcher_scroll = 0;
        return;
    }

    int selected = 0;
    if (selected_item >= 0) {
        for (int i = 0; i < ds->launcher_result_count; i++) {
            if (ds->launcher_results[i] == selected_item) {
                selected = i;
                break;
            }
        }
    }

    ds->launcher_selected = gui_clamp(selected, 0, ds->launcher_result_count - 1);
    int max_scroll = launcher_scroll_max(ds);
    if (ds->launcher_scroll > max_scroll) ds->launcher_scroll = max_scroll;
    if (ds->launcher_selected < ds->launcher_scroll) {
        ds->launcher_scroll = ds->launcher_selected;
    }
    if (ds->launcher_selected >= ds->launcher_scroll + LAUNCHER_VISIBLE_ROWS) {
        ds->launcher_scroll = ds->launcher_selected - LAUNCHER_VISIBLE_ROWS + 1;
    }
}

static void desktop_move_launcher_selection(DesktopState* ds, int delta) {
    if (ds->launcher_result_count <= 0) return;

    if (ds->launcher_selected < 0) {
        ds->launcher_selected = 0;
    } else {
        ds->launcher_selected = gui_clamp(ds->launcher_selected + delta, 0, ds->launcher_result_count - 1);
    }

    if (ds->launcher_selected < ds->launcher_scroll) {
        ds->launcher_scroll = ds->launcher_selected;
    } else if (ds->launcher_selected >= ds->launcher_scroll + LAUNCHER_VISIBLE_ROWS) {
        ds->launcher_scroll = ds->launcher_selected - LAUNCHER_VISIBLE_ROWS + 1;
    }
}

static void launcher_activate_selected(DesktopState* ds) {
    if (ds->launcher_selected < 0 || ds->launcher_selected >= ds->launcher_result_count) return;

    int item_idx = ds->launcher_results[ds->launcher_selected];
    if (item_idx < 0 || item_idx >= ds->launcher_item_count) return;

    LauncherItem* item = &ds->launcher_items[item_idx];
    switch (item->kind) {
    case LAUNCHER_ITEM_EXTERNAL_APP:
        if (item->ref_index >= 0 && item->ref_index < ds->external_app_count) {
            desktop_launch_external_app(ds, &ds->external_apps[item->ref_index]);
        }
        break;
    case LAUNCHER_ITEM_BUILTIN_FILES:
        open_filemanager(ds);
        break;
    case LAUNCHER_ITEM_COMMAND_REBOOT:
        desktop_close_launcher(ds);
        montauk::reset();
        return;
    case LAUNCHER_ITEM_COMMAND_SHUTDOWN:
        desktop_close_launcher(ds);
        montauk::shutdown();
        return;
    case LAUNCHER_ITEM_SPECIAL_FOLDER:
        if (item->path[0] != '\0') {
            open_filemanager_path(ds, item->path);
        }
        break;
    }

    desktop_close_launcher(ds);
}

static void launcher_update_super_state(DesktopState* ds, const Montauk::KeyEvent& key) {
    uint8_t sc = key.scancode & 0x7F;
    if (sc == 0x5B) {
        ds->super_left_down = key.pressed;
    } else if (sc == 0x5C) {
        ds->super_right_down = key.pressed;
    }
}

static const char* fit_trailing_text(const char* text, int max_w) {
    const char* visible = text;
    while (*visible && text_width(visible) > max_w) visible++;
    return visible;
}

static void draw_rounded_panel(Framebuffer& fb, int x, int y, int w, int h,
                               int radius, Color border, Color bg) {
    fill_rounded_rect(fb, x, y, w, h, radius, border);
    if (w > 2 && h > 2) {
        int inner_radius = radius > 0 ? radius - 1 : 0;
        fill_rounded_rect(fb, x + 1, y + 1, w - 2, h - 2, inner_radius, bg);
    }
}

void desktop_init_launcher(DesktopState* ds) {
    ds->launcher_open = false;
    ds->launcher_query[0] = '\0';
    ds->launcher_query_len = 0;
    ds->launcher_item_count = 0;
    ds->launcher_result_count = 0;
    ds->launcher_selected = -1;
    ds->launcher_scroll = 0;
    ds->super_left_down = false;
    ds->super_right_down = false;
}

void desktop_open_launcher(DesktopState* ds) {
    desktop_build_launcher_items(ds);
    ds->launcher_open = true;
    ds->launcher_query[0] = '\0';
    ds->launcher_query_len = 0;
    ds->launcher_selected = -1;
    ds->launcher_scroll = 0;
    ds->app_menu_open = false;
    ds->ctx_menu_open = false;
    ds->net_popup_open = false;
    ds->vol_popup_open = false;
    ds->vol_dragging = false;
    desktop_update_launcher_results(ds);
}

void desktop_close_launcher(DesktopState* ds) {
    ds->launcher_open = false;
    ds->launcher_scroll = 0;
}

bool desktop_handle_launcher_keyboard(DesktopState* ds, const Montauk::KeyEvent& key) {
    launcher_update_super_state(ds, key);
    uint8_t sc = key.scancode & 0x7F;

    if (sc == 0x5B || sc == 0x5C) {
        return true;
    }

    if (ds->screen_locked) {
        return false;
    }

    if (key.pressed && sc == 0x39 && launcher_super_down(ds)) {
        if (ds->launcher_open) {
            desktop_close_launcher(ds);
        } else {
            desktop_open_launcher(ds);
        }
        return true;
    }

    if (!ds->launcher_open) {
        return false;
    }

    if (!key.pressed) return true;

    if (key.ascii == '\n' || key.ascii == '\r' || sc == 0x1C) {
        launcher_activate_selected(ds);
        return true;
    }

    if (sc == 0x01) {
        desktop_close_launcher(ds);
        return true;
    }

    if (key.ascii == '\b' || sc == 0x0E) {
        if (ds->launcher_query_len > 0) {
            ds->launcher_query_len--;
            ds->launcher_query[ds->launcher_query_len] = '\0';
            desktop_update_launcher_results(ds);
        }
        return true;
    }

    if (sc == 0x48) {
        desktop_move_launcher_selection(ds, -1);
        return true;
    }
    if (sc == 0x50) {
        desktop_move_launcher_selection(ds, 1);
        return true;
    }
    if (sc == 0x49) {
        desktop_move_launcher_selection(ds, -LAUNCHER_VISIBLE_ROWS);
        return true;
    }
    if (sc == 0x51) {
        desktop_move_launcher_selection(ds, LAUNCHER_VISIBLE_ROWS);
        return true;
    }
    if (sc == 0x47) {
        if (ds->launcher_result_count > 0) {
            ds->launcher_selected = 0;
            ds->launcher_scroll = 0;
        }
        return true;
    }
    if (sc == 0x4F) {
        if (ds->launcher_result_count > 0) {
            ds->launcher_selected = ds->launcher_result_count - 1;
            ds->launcher_scroll = launcher_scroll_max(ds);
        }
        return true;
    }

    if (key.ascii >= 0x20 && key.ascii < 0x7F && ds->launcher_query_len < 63) {
        ds->launcher_query[ds->launcher_query_len++] = key.ascii;
        ds->launcher_query[ds->launcher_query_len] = '\0';
        desktop_update_launcher_results(ds);
    }
    return true;
}

void desktop_handle_launcher_mouse(DesktopState* ds) {
    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    uint8_t buttons = ds->mouse.buttons;
    uint8_t prev = ds->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);
    bool right_pressed = (buttons & 0x02) && !(prev & 0x02);

    Rect bounds = launcher_bounds(ds);
    if (right_pressed) {
        desktop_close_launcher(ds);
        return;
    }

    if (!bounds.contains(mx, my)) {
        if (left_pressed) {
            desktop_close_launcher(ds);
        }
        return;
    }

    int visible = launcher_visible_result_count(ds);
    for (int i = 0; i < visible; i++) {
        Rect row = launcher_row_rect(ds, i);
        if (!row.contains(mx, my)) continue;

        ds->launcher_selected = ds->launcher_scroll + i;
        if (left_pressed) {
            launcher_activate_selected(ds);
        }
        return;
    }
}

void desktop_draw_launcher(DesktopState* ds) {
    Framebuffer& fb = ds->fb;
    int sfh = system_font_height();
    bool show_results = launcher_show_results(ds);
    bool cursor_on = ((montauk::get_milliseconds() / LAUNCHER_CURSOR_BLINK_MS) & 1) == 0;

    Rect bounds = launcher_bounds(ds);
    Rect search = launcher_search_rect(ds);
    Rect list = launcher_results_rect(ds);

    draw_rounded_panel(fb, bounds.x, bounds.y, bounds.w, bounds.h, 16, LAUNCHER_BORDER, LAUNCHER_FIELD_BG);

    int count_w = 0;
    int count_x = search.x + search.w - LAUNCHER_TEXT_PAD;
    if (show_results) {
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d result%s",
                 ds->launcher_result_count,
                 ds->launcher_result_count == 1 ? "" : "s");
        count_w = text_width(count_str);
        count_x -= count_w;
        draw_text(fb, count_x, search.y + (search.h - sfh) / 2, count_str, LAUNCHER_MUTED);
    }

    int query_x = search.x + LAUNCHER_TEXT_PAD;
    int query_y = search.y + (search.h - sfh) / 2;
    int query_max_w = search.w - LAUNCHER_TEXT_PAD * 2 - (show_results ? (count_w + 12) : 0);
    if (query_max_w < 0) query_max_w = 0;
    if (ds->launcher_query_len > 0) {
        const char* visible = fit_trailing_text(ds->launcher_query, query_max_w);
        draw_text(fb, query_x, query_y, visible, colors::TEXT_COLOR);
        if (cursor_on) {
            int cx = query_x + text_width(visible);
            fb.fill_rect(cx + 1, query_y, 2, sfh, ds->settings.accent_color);
        }
    } else {
        if (cursor_on) {
            fb.fill_rect(query_x + 1, query_y, 2, sfh, ds->settings.accent_color);
        }
        draw_text(fb, query_x + 8, query_y, "Search apps, folders, and commands", LAUNCHER_PLACEHOLDER);
    }

    if (show_results) {
        fb.fill_rect(bounds.x + 2, search.y + search.h - 2, bounds.w - 4, 2, ds->settings.accent_color);
    }

    if (!show_results) {
        return;
    }

    if (ds->launcher_result_count <= 0) {
        const char* empty = "No matching results";
        const char* hint = "Try a different name, folder, or command";
        int empty_w = text_width(empty);
        int hint_w = text_width(hint);
        int cy = list.y + (list.h - sfh * 2 - 8) / 2;
        draw_text(fb, list.x + (list.w - empty_w) / 2, cy, empty, colors::TEXT_COLOR);
        draw_text(fb, list.x + (list.w - hint_w) / 2, cy + sfh + 8, hint, LAUNCHER_MUTED);
        return;
    }

    int visible = launcher_visible_result_count(ds);
    for (int i = 0; i < visible; i++) {
        int result_idx = ds->launcher_scroll + i;
        if (result_idx < 0 || result_idx >= ds->launcher_result_count) continue;

        Rect row = launcher_row_rect(ds, i);
        int item_idx = ds->launcher_results[result_idx];
        if (item_idx < 0 || item_idx >= ds->launcher_item_count) continue;

        LauncherItem* item = &ds->launcher_items[item_idx];
        bool selected = (result_idx == ds->launcher_selected);
        bool hovered = row.contains(ds->mouse.x, ds->mouse.y);
        if (selected) {
            fill_rounded_rect(fb, row.x, row.y, row.w, row.h, 8, LAUNCHER_SELECTED_BG);
        } else if (hovered) {
            fill_rounded_rect(fb, row.x, row.y, row.w, row.h, 8, LAUNCHER_HOVER_BG);
        }

        if (i > 0) {
            fb.fill_rect(row.x + 8, row.y, row.w - 16, 1, Color::from_rgb(0xE3, 0xE8, 0xEF));
        }

        SvgIcon* icon = item->icon ? item->icon : &ds->icon_exec;
        if (icon && icon->pixels) {
            int ix = row.x + 12;
            int iy = row.y + (row.h - icon->height) / 2;
            fb.blit_alpha(ix, iy, icon->width, icon->height, icon->pixels);
        }

        int text_x = row.x + 46;
        int name_y = row.y + 9;
        int cat_y = row.y + 27;
        draw_text(fb, text_x, name_y, item->name, colors::TEXT_COLOR);
        draw_text(fb, text_x, cat_y, item->category, LAUNCHER_MUTED);
    }
}

uint64_t desktop_launcher_blink_token(const DesktopState* ds, uint64_t now) {
    return ds->launcher_open ? (now / LAUNCHER_CURSOR_BLINK_MS) : ~0ull;
}
