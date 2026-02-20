/*
    * app_filemanager.cpp
    * ZenithOS Desktop - Enhanced File Manager application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// File Manager state
// ============================================================================

struct FileManagerState {
    char current_path[256];
    char history[16][256];
    int history_pos;
    int history_count;
    char entry_names[64][64];
    int entry_types[64];   // 0=file, 1=directory, 2=executable
    int entry_sizes[64];
    int entry_count;
    int selected;
    int scroll_offset;
    bool is_dir[64];
    int last_click_item;
    uint64_t last_click_time;
    Scrollbar scrollbar;
    DesktopState* desktop;
};

static constexpr int FM_TOOLBAR_H = 32;
static constexpr int FM_PATHBAR_H = 24;
static constexpr int FM_HEADER_H  = 20;
static constexpr int FM_ITEM_H    = 24;
static constexpr int FM_SCROLLBAR_W = 12;

// ============================================================================
// File type detection
// ============================================================================

static bool str_ends_with(const char* s, const char* suffix) {
    int slen = zenith::slen(s);
    int suflen = zenith::slen(suffix);
    if (suflen > slen) return false;
    for (int i = 0; i < suflen; i++) {
        char sc = s[slen - suflen + i];
        char ec = suffix[i];
        if (sc >= 'A' && sc <= 'Z') sc += 32;
        if (ec >= 'A' && ec <= 'Z') ec += 32;
        if (sc != ec) return false;
    }
    return true;
}

static int detect_file_type(const char* name, bool is_dir) {
    if (is_dir) return 1;
    if (str_ends_with(name, ".elf")) return 2;
    return 0;
}

// ============================================================================
// Directory reading with sorting and file sizes
// ============================================================================

static void filemanager_read_dir(FileManagerState* fm) {
    const char* names[64];
    fm->entry_count = zenith::readdir(fm->current_path, names, 64);
    if (fm->entry_count < 0) fm->entry_count = 0;

    // readdir returns full paths from the VFS (e.g. "man/fetch.1" instead
    // of just "fetch.1").  Compute the prefix to strip so we get basenames.
    const char* after_drive = fm->current_path;
    for (int k = 0; after_drive[k]; k++) {
        if (after_drive[k] == ':' && after_drive[k + 1] == '/') {
            after_drive += k + 2;
            break;
        }
    }
    char prefix[256] = {0};
    int prefix_len = 0;
    if (after_drive[0] != '\0') {
        zenith::strcpy(prefix, after_drive);
        prefix_len = zenith::slen(prefix);
        if (prefix_len > 0 && prefix[prefix_len - 1] != '/') {
            prefix[prefix_len++] = '/';
            prefix[prefix_len] = '\0';
        }
    }

    for (int i = 0; i < fm->entry_count; i++) {
        const char* raw = names[i];
        // Strip directory prefix if it matches
        if (prefix_len > 0) {
            bool match = true;
            for (int k = 0; k < prefix_len; k++) {
                if (raw[k] != prefix[k]) { match = false; break; }
            }
            if (match) raw += prefix_len;
        }
        zenith::strncpy(fm->entry_names[i], raw, 63);
        int len = zenith::slen(fm->entry_names[i]);

        // Detect directory
        if (len > 0 && fm->entry_names[i][len - 1] == '/') {
            fm->is_dir[i] = true;
            fm->entry_names[i][len - 1] = '\0';
        } else {
            bool has_dot = false;
            for (int j = 0; j < len; j++) {
                if (fm->entry_names[i][j] == '.') { has_dot = true; break; }
            }
            fm->is_dir[i] = !has_dot;
        }

        fm->entry_types[i] = detect_file_type(fm->entry_names[i], fm->is_dir[i]);

        // Get file size
        fm->entry_sizes[i] = 0;
        if (!fm->is_dir[i]) {
            char fullpath[512];
            zenith::strcpy(fullpath, fm->current_path);
            int plen = zenith::slen(fullpath);
            if (plen > 0 && fullpath[plen - 1] != '/') {
                str_append(fullpath, "/", 512);
            }
            str_append(fullpath, fm->entry_names[i], 512);
            int fd = zenith::open(fullpath);
            if (fd >= 0) {
                fm->entry_sizes[i] = (int)zenith::getsize(fd);
                zenith::close(fd);
            }
        }
    }

    // Sort: directories first, then alphabetical (case-insensitive)
    for (int i = 1; i < fm->entry_count; i++) {
        char tmp_name[64];
        int tmp_type = fm->entry_types[i];
        int tmp_size = fm->entry_sizes[i];
        bool tmp_isdir = fm->is_dir[i];
        zenith::strcpy(tmp_name, fm->entry_names[i]);

        int j = i - 1;
        while (j >= 0) {
            bool swap = false;
            if (tmp_isdir && !fm->is_dir[j]) {
                swap = true;
            } else if (tmp_isdir == fm->is_dir[j]) {
                if (str_compare_ci(tmp_name, fm->entry_names[j]) < 0) {
                    swap = true;
                }
            }
            if (!swap) break;

            zenith::strcpy(fm->entry_names[j + 1], fm->entry_names[j]);
            fm->entry_types[j + 1] = fm->entry_types[j];
            fm->entry_sizes[j + 1] = fm->entry_sizes[j];
            fm->is_dir[j + 1] = fm->is_dir[j];
            j--;
        }
        zenith::strcpy(fm->entry_names[j + 1], tmp_name);
        fm->entry_types[j + 1] = tmp_type;
        fm->entry_sizes[j + 1] = tmp_size;
        fm->is_dir[j + 1] = tmp_isdir;
    }

    fm->selected = -1;
    fm->scroll_offset = 0;
    fm->last_click_item = -1;
    fm->last_click_time = 0;
}

// ============================================================================
// History management
// ============================================================================

static void filemanager_push_history(FileManagerState* fm) {
    // Don't push if same as current position
    if (fm->history_count > 0 && fm->history_pos >= 0) {
        if (zenith::streq(fm->history[fm->history_pos], fm->current_path)) return;
    }
    fm->history_pos++;
    if (fm->history_pos >= 16) fm->history_pos = 15;
    zenith::strcpy(fm->history[fm->history_pos], fm->current_path);
    fm->history_count = fm->history_pos + 1;
}

static void filemanager_navigate(FileManagerState* fm, const char* name) {
    int path_len = zenith::slen(fm->current_path);
    if (path_len > 0 && fm->current_path[path_len - 1] != '/') {
        str_append(fm->current_path, "/", 256);
    }
    str_append(fm->current_path, name, 256);
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

static void filemanager_go_up(FileManagerState* fm) {
    int len = zenith::slen(fm->current_path);
    if (len <= 3) return; // "0:/" is root

    if (len > 0 && fm->current_path[len - 1] == '/') {
        fm->current_path[len - 1] = '\0';
        len--;
    }

    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (fm->current_path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash >= 0) {
        fm->current_path[last_slash + 1] = '\0';
    }
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

static void filemanager_go_back(FileManagerState* fm) {
    if (fm->history_pos <= 0) return;
    fm->history_pos--;
    zenith::strcpy(fm->current_path, fm->history[fm->history_pos]);
    filemanager_read_dir(fm);
}

static void filemanager_go_forward(FileManagerState* fm) {
    if (fm->history_pos >= fm->history_count - 1) return;
    fm->history_pos++;
    zenith::strcpy(fm->current_path, fm->history[fm->history_pos]);
    filemanager_read_dir(fm);
}

static void filemanager_go_home(FileManagerState* fm) {
    zenith::strcpy(fm->current_path, "0:/");
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

// ============================================================================
// Drawing
// ============================================================================

static void filemanager_on_draw(Window* win, Framebuffer& fb) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Rect cr = win->content_rect();
    int cw = cr.w;
    int ch = cr.h;
    uint32_t* pixels = win->content;

    uint32_t bg_px = colors::WINDOW_BG.to_pixel();
    for (int i = 0; i < cw * ch; i++) pixels[i] = bg_px;

    uint32_t text_px = colors::TEXT_COLOR.to_pixel();
    uint32_t sep_px = colors::BORDER.to_pixel();
    uint32_t toolbar_px = Color::from_rgb(0xF5, 0xF5, 0xF5).to_pixel();

    DesktopState* ds = fm->desktop;

    // ---- Toolbar (32px) ----
    for (int y = 0; y < FM_TOOLBAR_H && y < ch; y++)
        for (int x = 0; x < cw; x++)
            pixels[y * cw + x] = toolbar_px;

    // Toolbar buttons: Back, Forward, Up, Home
    struct ToolBtn { int x; SvgIcon* icon; };
    ToolBtn btns[4] = {
        {  4, ds ? &ds->icon_go_back    : nullptr },
        { 32, ds ? &ds->icon_go_forward : nullptr },
        { 60, ds ? &ds->icon_go_up      : nullptr },
        { 88, ds ? &ds->icon_home       : nullptr },
    };

    for (int i = 0; i < 4; i++) {
        int bx = btns[i].x;
        int by = 4;
        // Button background
        uint32_t btn_px = Color::from_rgb(0xE8, 0xE8, 0xE8).to_pixel();
        for (int dy = 0; dy < 24 && by + dy < ch; dy++)
            for (int dx = 0; dx < 24 && bx + dx < cw; dx++)
                pixels[(by + dy) * cw + (bx + dx)] = btn_px;

        if (btns[i].icon) {
            int ix = bx + (24 - btns[i].icon->width) / 2;
            int iy = by + (24 - btns[i].icon->height) / 2;
            blit_icon_to_pixels(pixels, cw, ch, ix, iy, *btns[i].icon);
        }
    }

    // Toolbar separator
    int sep_y = FM_TOOLBAR_H - 1;
    if (sep_y < ch) {
        for (int x = 0; x < cw; x++)
            pixels[sep_y * cw + x] = sep_px;
    }

    // ---- Path bar ----
    int pathbar_y = FM_TOOLBAR_H;
    uint32_t pathbar_px = Color::from_rgb(0xF0, 0xF0, 0xF0).to_pixel();
    for (int y = pathbar_y; y < pathbar_y + FM_PATHBAR_H && y < ch; y++)
        for (int x = 0; x < cw; x++)
            pixels[y * cw + x] = pathbar_px;

    draw_text_to_pixels(pixels, cw, ch, 8, pathbar_y + 4, fm->current_path, text_px);

    // Path bar separator
    sep_y = pathbar_y + FM_PATHBAR_H - 1;
    if (sep_y < ch) {
        for (int x = 0; x < cw; x++)
            pixels[sep_y * cw + x] = sep_px;
    }

    // ---- Column headers ----
    int header_y = FM_TOOLBAR_H + FM_PATHBAR_H;
    uint32_t header_px = Color::from_rgb(0xF8, 0xF8, 0xF8).to_pixel();
    for (int y = header_y; y < header_y + FM_HEADER_H && y < ch; y++)
        for (int x = 0; x < cw; x++)
            pixels[y * cw + x] = header_px;

    uint32_t dim_px = Color::from_rgb(0x88, 0x88, 0x88).to_pixel();
    int name_col_x = 8;
    int size_col_x = cw - FM_SCROLLBAR_W - 120;
    int type_col_x = cw - FM_SCROLLBAR_W - 60;

    draw_text_to_pixels(pixels, cw, ch, name_col_x, header_y + 2, "Name", dim_px);
    if (size_col_x > 100)
        draw_text_to_pixels(pixels, cw, ch, size_col_x, header_y + 2, "Size", dim_px);
    if (type_col_x > 160)
        draw_text_to_pixels(pixels, cw, ch, type_col_x, header_y + 2, "Type", dim_px);

    // Header separator
    sep_y = header_y + FM_HEADER_H - 1;
    if (sep_y < ch) {
        for (int x = 0; x < cw; x++)
            pixels[sep_y * cw + x] = sep_px;
    }

    // Column separator lines
    if (size_col_x > 100) {
        for (int y = header_y; y < ch; y++)
            if (size_col_x - 4 >= 0 && size_col_x - 4 < cw)
                pixels[y * cw + (size_col_x - 4)] = sep_px;
    }

    // ---- File entries ----
    int list_y = header_y + FM_HEADER_H;
    int list_h = ch - list_y;
    int visible_items = list_h / FM_ITEM_H;
    int content_h = fm->entry_count * FM_ITEM_H;

    // Update scrollbar
    fm->scrollbar.bounds = {cw - FM_SCROLLBAR_W, list_y, FM_SCROLLBAR_W, list_h};
    fm->scrollbar.content_height = content_h;
    fm->scrollbar.view_height = list_h;

    int scroll_items = fm->scrollbar.scroll_offset / FM_ITEM_H;

    for (int i = scroll_items; i < fm->entry_count && (i - scroll_items) < visible_items + 1; i++) {
        int iy = list_y + (i - scroll_items) * FM_ITEM_H - (fm->scrollbar.scroll_offset % FM_ITEM_H);
        if (iy + FM_ITEM_H <= list_y || iy >= ch) continue;

        // Highlight selected
        if (i == fm->selected) {
            uint32_t sel_px = colors::MENU_HOVER.to_pixel();
            for (int y = gui_max(iy, list_y); y < gui_min(iy + FM_ITEM_H, ch); y++)
                for (int x = 0; x < cw - FM_SCROLLBAR_W; x++)
                    pixels[y * cw + x] = sel_px;
        }

        // Icon
        int icon_x = 8;
        int icon_y = iy + (FM_ITEM_H - 16) / 2;
        if (ds && fm->entry_types[i] == 1 && ds->icon_folder.pixels) {
            blit_icon_to_pixels(pixels, cw, ch, icon_x, icon_y, ds->icon_folder);
        } else if (ds && fm->entry_types[i] == 2 && ds->icon_exec.pixels) {
            blit_icon_to_pixels(pixels, cw, ch, icon_x, icon_y, ds->icon_exec);
        } else if (ds && ds->icon_file.pixels) {
            blit_icon_to_pixels(pixels, cw, ch, icon_x, icon_y, ds->icon_file);
        } else {
            uint32_t icon_px = fm->is_dir[i]
                ? Color::from_rgb(0xFF, 0xBD, 0x2E).to_pixel()
                : Color::from_rgb(0x90, 0x90, 0x90).to_pixel();
            for (int dy = 0; dy < 16 && icon_y + dy < ch && icon_y + dy >= list_y; dy++)
                for (int dx = 0; dx < 16 && icon_x + dx < cw; dx++)
                    pixels[(icon_y + dy) * cw + (icon_x + dx)] = icon_px;
        }

        // Name
        int tx = 30;
        int ty = iy + (FM_ITEM_H - FONT_HEIGHT) / 2;
        if (ty >= list_y && ty + FONT_HEIGHT <= ch)
            draw_text_to_pixels(pixels, cw, ch, tx, ty, fm->entry_names[i], text_px);

        // Size
        if (size_col_x > 100 && !fm->is_dir[i] && ty >= list_y && ty + FONT_HEIGHT <= ch) {
            char size_str[16];
            format_size(size_str, fm->entry_sizes[i]);
            draw_text_to_pixels(pixels, cw, ch, size_col_x, ty, size_str, dim_px);
        }

        // Type
        if (type_col_x > 160 && ty >= list_y && ty + FONT_HEIGHT <= ch) {
            const char* type_str = "File";
            if (fm->entry_types[i] == 1) type_str = "Dir";
            else if (fm->entry_types[i] == 2) type_str = "Exec";
            draw_text_to_pixels(pixels, cw, ch, type_col_x, ty, type_str, dim_px);
        }
    }

    // ---- Scrollbar ----
    // Draw scrollbar directly to pixels
    if (fm->scrollbar.content_height > fm->scrollbar.view_height) {
        uint32_t sb_bg = colors::SCROLLBAR_BG.to_pixel();
        uint32_t sb_fg = (fm->scrollbar.hovered || fm->scrollbar.dragging)
            ? fm->scrollbar.hover_fg.to_pixel() : fm->scrollbar.fg.to_pixel();

        int sbx = fm->scrollbar.bounds.x;
        int sby = fm->scrollbar.bounds.y;
        int sbw = fm->scrollbar.bounds.w;
        int sbh = fm->scrollbar.bounds.h;

        for (int y = sby; y < sby + sbh && y < ch; y++)
            for (int x = sbx; x < sbx + sbw && x < cw; x++)
                pixels[y * cw + x] = sb_bg;

        int th = fm->scrollbar.thumb_height();
        int tty = fm->scrollbar.thumb_y();
        for (int y = tty; y < tty + th && y < sby + sbh && y < ch; y++)
            for (int x = sbx + 1; x < sbx + sbw - 1 && x < cw; x++)
                pixels[y * cw + x] = sb_fg;
    }
}

// ============================================================================
// Mouse handling
// ============================================================================

static void filemanager_on_mouse(Window* win, MouseEvent& ev) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Rect cr = win->content_rect();
    int local_x = ev.x - cr.x;
    int local_y = ev.y - cr.y;
    int cw = cr.w;

    // Scrollbar interaction
    MouseEvent local_ev = ev;
    local_ev.x = local_x;
    local_ev.y = local_y;
    fm->scrollbar.handle_mouse(local_ev);

    if (ev.left_pressed()) {
        // Toolbar button clicks
        if (local_y < FM_TOOLBAR_H) {
            if (local_x >= 4 && local_x < 28)  filemanager_go_back(fm);
            else if (local_x >= 32 && local_x < 56) filemanager_go_forward(fm);
            else if (local_x >= 60 && local_x < 84) filemanager_go_up(fm);
            else if (local_x >= 88 && local_x < 112) filemanager_go_home(fm);
            return;
        }

        // File list clicks
        int list_y = FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
        if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
            int rel_y = local_y - list_y + fm->scrollbar.scroll_offset;
            int clicked_idx = rel_y / FM_ITEM_H;

            if (clicked_idx >= 0 && clicked_idx < fm->entry_count) {
                uint64_t now = zenith::get_milliseconds();

                // Double-click detection
                if (fm->last_click_item == clicked_idx &&
                    (now - fm->last_click_time) < 400) {
                    if (fm->is_dir[clicked_idx]) {
                        filemanager_navigate(fm, fm->entry_names[clicked_idx]);
                    } else {
                        // Open file in text editor
                        char fullpath[512];
                        zenith::strcpy(fullpath, fm->current_path);
                        int plen = zenith::slen(fullpath);
                        if (plen > 0 && fullpath[plen - 1] != '/') {
                            str_append(fullpath, "/", 512);
                        }
                        str_append(fullpath, fm->entry_names[clicked_idx], 512);
                        if (fm->desktop) {
                            open_texteditor_with_file(fm->desktop, fullpath);
                        }
                    }
                    fm->last_click_item = -1;
                    fm->last_click_time = 0;
                } else {
                    fm->selected = clicked_idx;
                    fm->last_click_item = clicked_idx;
                    fm->last_click_time = now;
                }
            }
        }
    }

    // Scroll handling
    if (ev.scroll != 0) {
        int list_y_start = FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
        if (local_y >= list_y_start) {
            fm->scrollbar.scroll_offset -= ev.scroll * FM_ITEM_H;
            int ms = fm->scrollbar.max_scroll();
            if (fm->scrollbar.scroll_offset < 0) fm->scrollbar.scroll_offset = 0;
            if (fm->scrollbar.scroll_offset > ms) fm->scrollbar.scroll_offset = ms;
        }
    }
}

// ============================================================================
// Keyboard handling
// ============================================================================

static void filemanager_on_key(Window* win, const Zenith::KeyEvent& key) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm || !key.pressed) return;

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        filemanager_go_up(fm);
    } else if (key.scancode == 0x48) {
        // Up arrow
        if (fm->selected > 0) fm->selected--;
    } else if (key.scancode == 0x50) {
        // Down arrow
        if (fm->selected < fm->entry_count - 1) fm->selected++;
    } else if (key.ascii == '\n' || key.ascii == '\r') {
        if (fm->selected >= 0 && fm->selected < fm->entry_count) {
            if (fm->is_dir[fm->selected]) {
                filemanager_navigate(fm, fm->entry_names[fm->selected]);
            }
        }
    } else if (key.alt && key.scancode == 0x4B) {
        // Alt+Left: go back
        filemanager_go_back(fm);
    } else if (key.alt && key.scancode == 0x4D) {
        // Alt+Right: go forward
        filemanager_go_forward(fm);
    }
}

static void filemanager_on_close(Window* win) {
    if (win->app_data) {
        zenith::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// File Manager launcher
// ============================================================================

void open_filemanager(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Files", 150, 120, 560, 420);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    FileManagerState* fm = (FileManagerState*)zenith::malloc(sizeof(FileManagerState));
    zenith::memset(fm, 0, sizeof(FileManagerState));
    zenith::strcpy(fm->current_path, "0:/");
    fm->selected = -1;
    fm->last_click_item = -1;
    fm->history_pos = -1;
    fm->history_count = 0;
    fm->desktop = ds;

    fm->scrollbar.init(0, 0, FM_SCROLLBAR_W, 100);

    filemanager_push_history(fm);
    filemanager_read_dir(fm);

    win->app_data = fm;
    win->on_draw = filemanager_on_draw;
    win->on_mouse = filemanager_on_mouse;
    win->on_key = filemanager_on_key;
    win->on_close = filemanager_on_close;
}
