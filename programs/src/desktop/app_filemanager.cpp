/*
    * app_filemanager.cpp
    * MontaukOS Desktop - Enhanced File Manager application
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
    bool grid_view;
};

static constexpr int FM_TOOLBAR_H = 32;
static constexpr int FM_PATHBAR_H = 24;
static constexpr int FM_HEADER_H  = 20;
static constexpr int FM_ITEM_H    = 24;
static constexpr int FM_SCROLLBAR_W = 12;
static constexpr int FM_GRID_CELL_W = 80;
static constexpr int FM_GRID_CELL_H = 80;
static constexpr int FM_GRID_ICON   = 48;
static constexpr int FM_GRID_PAD    = 4;

// ============================================================================
// File type detection
// ============================================================================

static bool str_ends_with(const char* s, const char* suffix) {
    int slen = montauk::slen(s);
    int suflen = montauk::slen(suffix);
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

static bool is_image_file(const char* name) {
    return str_ends_with(name, ".jpg") || str_ends_with(name, ".jpeg");
}

static bool is_font_file(const char* name) {
    return str_ends_with(name, ".ttf");
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
    fm->entry_count = montauk::readdir(fm->current_path, names, 64);
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
        montauk::strcpy(prefix, after_drive);
        prefix_len = montauk::slen(prefix);
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
        montauk::strncpy(fm->entry_names[i], raw, 63);
        int len = montauk::slen(fm->entry_names[i]);

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
            montauk::strcpy(fullpath, fm->current_path);
            int plen = montauk::slen(fullpath);
            if (plen > 0 && fullpath[plen - 1] != '/') {
                str_append(fullpath, "/", 512);
            }
            str_append(fullpath, fm->entry_names[i], 512);
            int fd = montauk::open(fullpath);
            if (fd >= 0) {
                fm->entry_sizes[i] = (int)montauk::getsize(fd);
                montauk::close(fd);
            }
        }
    }

    // Sort: directories first, then alphabetical (case-insensitive)
    for (int i = 1; i < fm->entry_count; i++) {
        char tmp_name[64];
        int tmp_type = fm->entry_types[i];
        int tmp_size = fm->entry_sizes[i];
        bool tmp_isdir = fm->is_dir[i];
        montauk::strcpy(tmp_name, fm->entry_names[i]);

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

            montauk::strcpy(fm->entry_names[j + 1], fm->entry_names[j]);
            fm->entry_types[j + 1] = fm->entry_types[j];
            fm->entry_sizes[j + 1] = fm->entry_sizes[j];
            fm->is_dir[j + 1] = fm->is_dir[j];
            j--;
        }
        montauk::strcpy(fm->entry_names[j + 1], tmp_name);
        fm->entry_types[j + 1] = tmp_type;
        fm->entry_sizes[j + 1] = tmp_size;
        fm->is_dir[j + 1] = tmp_isdir;
    }

    fm->selected = -1;
    fm->scroll_offset = 0;
    fm->scrollbar.scroll_offset = 0;
    fm->last_click_item = -1;
    fm->last_click_time = 0;
}

// ============================================================================
// History management
// ============================================================================

static void filemanager_push_history(FileManagerState* fm) {
    // Don't push if same as current position
    if (fm->history_count > 0 && fm->history_pos >= 0) {
        if (montauk::streq(fm->history[fm->history_pos], fm->current_path)) return;
    }
    fm->history_pos++;
    if (fm->history_pos >= 16) fm->history_pos = 15;
    montauk::strcpy(fm->history[fm->history_pos], fm->current_path);
    fm->history_count = fm->history_pos + 1;
}

static void filemanager_navigate(FileManagerState* fm, const char* name) {
    int path_len = montauk::slen(fm->current_path);
    if (path_len > 0 && fm->current_path[path_len - 1] != '/') {
        str_append(fm->current_path, "/", 256);
    }
    str_append(fm->current_path, name, 256);
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

static void filemanager_go_up(FileManagerState* fm) {
    int len = montauk::slen(fm->current_path);
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
    montauk::strcpy(fm->current_path, fm->history[fm->history_pos]);
    filemanager_read_dir(fm);
}

static void filemanager_go_forward(FileManagerState* fm) {
    if (fm->history_pos >= fm->history_count - 1) return;
    fm->history_pos++;
    montauk::strcpy(fm->current_path, fm->history[fm->history_pos]);
    filemanager_read_dir(fm);
}

static void filemanager_go_home(FileManagerState* fm) {
    montauk::strcpy(fm->current_path, "0:/");
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

// ============================================================================
// Drawing
// ============================================================================

static void filemanager_draw_header(Canvas& c, FileManagerState* fm,
                                    Color toolbar_color, Color btn_bg) {
    DesktopState* ds = fm->desktop;

    // ---- Toolbar (32px) ----
    c.fill_rect(0, 0, c.w, FM_TOOLBAR_H, toolbar_color);

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
        c.fill_rect(bx, by, 24, 24, btn_bg);

        if (btns[i].icon) {
            int ix = bx + (24 - btns[i].icon->width) / 2;
            int iy = by + (24 - btns[i].icon->height) / 2;
            c.icon(ix, iy, *btns[i].icon);
        }
    }

    // Toggle view button (5th toolbar button)
    {
        int bx = 120, by = 4;
        c.fill_rect(bx, by, 24, 24, btn_bg);
        if (fm->grid_view) {
            for (int r = 0; r < 2; r++)
                for (int cc = 0; cc < 2; cc++)
                    c.fill_rect(bx + 5 + cc * 8, by + 5 + r * 8, 6, 6, colors::TEXT_COLOR);
        } else {
            for (int r = 0; r < 3; r++)
                c.fill_rect(bx + 5, by + 5 + r * 5, 14, 2, colors::TEXT_COLOR);
        }
    }

    // Toolbar separator
    c.hline(0, FM_TOOLBAR_H - 1, c.w, colors::BORDER);

    // ---- Path bar ----
    int pathbar_y = FM_TOOLBAR_H;
    c.fill_rect(0, pathbar_y, c.w, FM_PATHBAR_H, Color::from_rgb(0xF0, 0xF0, 0xF0));
    c.text(8, pathbar_y + 4, fm->current_path, colors::TEXT_COLOR);

    // Path bar separator
    c.hline(0, pathbar_y + FM_PATHBAR_H - 1, c.w, colors::BORDER);
}

static void filemanager_on_draw(Window* win, Framebuffer& fb) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    Color toolbar_color = Color::from_rgb(0xF5, 0xF5, 0xF5);
    Color btn_bg = Color::from_rgb(0xE8, 0xE8, 0xE8);
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    DesktopState* ds = fm->desktop;

    // Draw header (toolbar + path bar)
    filemanager_draw_header(c, fm, toolbar_color, btn_bg);

    if (fm->grid_view) {
        // ---- Grid View ----
        int list_y = FM_TOOLBAR_H + FM_PATHBAR_H;
        int list_h = c.h - list_y;
        int cols = (c.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
        if (cols < 1) cols = 1;
        int rows = (fm->entry_count + cols - 1) / cols;
        int content_h = rows * FM_GRID_CELL_H;

        // Update scrollbar
        fm->scrollbar.bounds = {c.w - FM_SCROLLBAR_W, list_y, FM_SCROLLBAR_W, list_h};
        fm->scrollbar.content_height = content_h;
        fm->scrollbar.view_height = list_h;

        for (int i = 0; i < fm->entry_count; i++) {
            int col = i % cols;
            int row = i / cols;
            int cell_x = col * FM_GRID_CELL_W;
            int cell_y = list_y + row * FM_GRID_CELL_H - fm->scrollbar.scroll_offset;

            // Skip if entirely off-screen
            if (cell_y + FM_GRID_CELL_H <= list_y || cell_y >= c.h) continue;

            // Selection highlight
            if (i == fm->selected) {
                int sy = gui_max(cell_y, list_y);
                int sh = gui_min(cell_y + FM_GRID_CELL_H, c.h) - sy;
                int sw = gui_min(FM_GRID_CELL_W, c.w - FM_SCROLLBAR_W - cell_x);
                if (sh > 0 && sw > 0)
                    c.fill_rect(cell_x, sy, sw, sh, colors::MENU_HOVER);
            }

            // Large icon centered horizontally
            int icon_x = cell_x + (FM_GRID_CELL_W - FM_GRID_ICON) / 2;
            int icon_y = cell_y + FM_GRID_PAD;
            if (ds && fm->entry_types[i] == 1 && ds->icon_folder_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_folder_lg);
            } else if (ds && fm->entry_types[i] == 2 && ds->icon_exec_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_exec_lg);
            } else if (ds && ds->icon_file_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_file_lg);
            } else {
                Color icon_c = fm->is_dir[i]
                    ? Color::from_rgb(0xFF, 0xBD, 0x2E)
                    : Color::from_rgb(0x90, 0x90, 0x90);
                int iy_clip = gui_max(icon_y, list_y);
                int ih_clip = FM_GRID_ICON - (iy_clip - icon_y);
                if (ih_clip > 0)
                    c.fill_rect(icon_x, iy_clip, FM_GRID_ICON, ih_clip, icon_c);
            }

            // Filename centered below icon, truncated if needed
            char label[16];
            int nlen = montauk::slen(fm->entry_names[i]);
            if (nlen > 9) {
                for (int k = 0; k < 9; k++) label[k] = fm->entry_names[i][k];
                label[9] = '.';
                label[10] = '.';
                label[11] = '\0';
            } else {
                montauk::strncpy(label, fm->entry_names[i], 15);
            }
            int tw = text_width(label);
            int tx = cell_x + (FM_GRID_CELL_W - tw) / 2;
            if (tx < cell_x) tx = cell_x;
            int ty = icon_y + FM_GRID_ICON + 2;
            if (ty >= list_y && ty + system_font_height() <= c.h)
                c.text(tx, ty, label, colors::TEXT_COLOR);
        }
    } else {
        // ---- List View ----

        // ---- Column headers ----
        int header_y = FM_TOOLBAR_H + FM_PATHBAR_H;
        c.fill_rect(0, header_y, c.w, FM_HEADER_H, Color::from_rgb(0xF8, 0xF8, 0xF8));

        int name_col_x = 8;
        int size_col_x = c.w - FM_SCROLLBAR_W - 120;
        int type_col_x = c.w - FM_SCROLLBAR_W - 60;

        c.text(name_col_x, header_y + 2, "Name", dim);
        if (size_col_x > 100)
            c.text(size_col_x, header_y + 2, "Size", dim);
        if (type_col_x > 160)
            c.text(type_col_x, header_y + 2, "Type", dim);

        // Header separator
        c.hline(0, header_y + FM_HEADER_H - 1, c.w, colors::BORDER);

        // Column separator lines
        if (size_col_x > 100) {
            c.vline(size_col_x - 4, header_y, c.h - header_y, colors::BORDER);
        }

        // ---- File entries ----
        int list_y = header_y + FM_HEADER_H;
        int list_h = c.h - list_y;
        int visible_items = list_h / FM_ITEM_H;
        int content_h = fm->entry_count * FM_ITEM_H;

        // Update scrollbar
        fm->scrollbar.bounds = {c.w - FM_SCROLLBAR_W, list_y, FM_SCROLLBAR_W, list_h};
        fm->scrollbar.content_height = content_h;
        fm->scrollbar.view_height = list_h;

        int scroll_items = fm->scrollbar.scroll_offset / FM_ITEM_H;

        for (int i = scroll_items; i < fm->entry_count && (i - scroll_items) < visible_items + 1; i++) {
            int iy = list_y + (i - scroll_items) * FM_ITEM_H - (fm->scrollbar.scroll_offset % FM_ITEM_H);
            if (iy + FM_ITEM_H <= list_y || iy >= c.h) continue;

            // Highlight selected
            if (i == fm->selected) {
                int sy = gui_max(iy, list_y);
                int sh = gui_min(iy + FM_ITEM_H, c.h) - sy;
                if (sh > 0)
                    c.fill_rect(0, sy, c.w - FM_SCROLLBAR_W, sh, colors::MENU_HOVER);
            }

            // Icon
            int ico_x = 8;
            int ico_y = iy + (FM_ITEM_H - 16) / 2;
            if (ds && fm->entry_types[i] == 1 && ds->icon_folder.pixels) {
                c.icon(ico_x, ico_y, ds->icon_folder);
            } else if (ds && fm->entry_types[i] == 2 && ds->icon_exec.pixels) {
                c.icon(ico_x, ico_y, ds->icon_exec);
            } else if (ds && ds->icon_file.pixels) {
                c.icon(ico_x, ico_y, ds->icon_file);
            } else {
                Color icon_c = fm->is_dir[i]
                    ? Color::from_rgb(0xFF, 0xBD, 0x2E)
                    : Color::from_rgb(0x90, 0x90, 0x90);
                int iy_clip = gui_max(ico_y, list_y);
                int ih_clip = 16 - (iy_clip - ico_y);
                if (ih_clip > 0)
                    c.fill_rect(ico_x, iy_clip, 16, ih_clip, icon_c);
            }

            // Name
            int tx = 30;
            int fm_sfh = system_font_height();
            int ty = iy + (FM_ITEM_H - fm_sfh) / 2;
            if (ty >= list_y && ty + fm_sfh <= c.h)
                c.text(tx, ty, fm->entry_names[i], colors::TEXT_COLOR);

            // Size
            if (size_col_x > 100 && !fm->is_dir[i] && ty >= list_y && ty + fm_sfh <= c.h) {
                char size_str[16];
                format_size(size_str, fm->entry_sizes[i]);
                c.text(size_col_x, ty, size_str, dim);
            }

            // Type
            if (type_col_x > 160 && ty >= list_y && ty + fm_sfh <= c.h) {
                const char* type_str = "File";
                if (fm->entry_types[i] == 1) type_str = "Dir";
                else if (fm->entry_types[i] == 2) type_str = "Exec";
                c.text(type_col_x, ty, type_str, dim);
            }
        }
    }

    // ---- Scrollbar ----
    if (fm->scrollbar.content_height > fm->scrollbar.view_height) {
        Color sb_fg_color = (fm->scrollbar.hovered || fm->scrollbar.dragging)
            ? fm->scrollbar.hover_fg : fm->scrollbar.fg;

        int sbx = fm->scrollbar.bounds.x;
        int sby = fm->scrollbar.bounds.y;
        int sbw = fm->scrollbar.bounds.w;
        int sbh = fm->scrollbar.bounds.h;

        c.fill_rect(sbx, sby, sbw, sbh, colors::SCROLLBAR_BG);

        int th = fm->scrollbar.thumb_height();
        int tty = fm->scrollbar.thumb_y();
        c.fill_rect(sbx + 1, tty, sbw - 2, th, sb_fg_color);
    }

    // Repaint header on top of content to clip scrolled icon overflow
    if (fm->scrollbar.scroll_offset > 0)
        filemanager_draw_header(c, fm, toolbar_color, btn_bg);
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
            else if (local_x >= 120 && local_x < 144) {
                fm->grid_view = !fm->grid_view;
                fm->scrollbar.scroll_offset = 0;
            }
            return;
        }

        // File clicks (grid vs list)
        if (fm->grid_view) {
            int list_y = FM_TOOLBAR_H + FM_PATHBAR_H;
            if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
                int cols = (cw - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
                if (cols < 1) cols = 1;
                int col = local_x / FM_GRID_CELL_W;
                int row = (local_y - list_y + fm->scrollbar.scroll_offset) / FM_GRID_CELL_H;
                int clicked_idx = row * cols + col;

                if (clicked_idx >= 0 && clicked_idx < fm->entry_count && col < cols) {
                    uint64_t now = montauk::get_milliseconds();

                    if (fm->last_click_item == clicked_idx &&
                        (now - fm->last_click_time) < 400) {
                        if (fm->is_dir[clicked_idx]) {
                            filemanager_navigate(fm, fm->entry_names[clicked_idx]);
                        } else {
                            char fullpath[512];
                            montauk::strcpy(fullpath, fm->current_path);
                            int plen = montauk::slen(fullpath);
                            if (plen > 0 && fullpath[plen - 1] != '/') {
                                str_append(fullpath, "/", 512);
                            }
                            str_append(fullpath, fm->entry_names[clicked_idx], 512);
                            if (is_image_file(fm->entry_names[clicked_idx])) {
                                montauk::spawn("0:/os/imageviewer.elf", fullpath);
                            } else if (is_font_file(fm->entry_names[clicked_idx])) {
                                montauk::spawn("0:/os/fontpreview.elf", fullpath);
                            } else if (fm->desktop) {
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
        } else {
            // List view clicks
            int list_y = FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
            if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
                int rel_y = local_y - list_y + fm->scrollbar.scroll_offset;
                int clicked_idx = rel_y / FM_ITEM_H;

                if (clicked_idx >= 0 && clicked_idx < fm->entry_count) {
                    uint64_t now = montauk::get_milliseconds();

                    // Double-click detection
                    if (fm->last_click_item == clicked_idx &&
                        (now - fm->last_click_time) < 400) {
                        if (fm->is_dir[clicked_idx]) {
                            filemanager_navigate(fm, fm->entry_names[clicked_idx]);
                        } else {
                            // Open file in appropriate viewer
                            char fullpath[512];
                            montauk::strcpy(fullpath, fm->current_path);
                            int plen = montauk::slen(fullpath);
                            if (plen > 0 && fullpath[plen - 1] != '/') {
                                str_append(fullpath, "/", 512);
                            }
                            str_append(fullpath, fm->entry_names[clicked_idx], 512);
                            if (is_image_file(fm->entry_names[clicked_idx])) {
                                montauk::spawn("0:/os/imageviewer.elf", fullpath);
                            } else if (is_font_file(fm->entry_names[clicked_idx])) {
                                montauk::spawn("0:/os/fontpreview.elf", fullpath);
                            } else if (fm->desktop) {
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
    }

    // Scroll handling
    if (ev.scroll != 0) {
        int list_y_start = fm->grid_view
            ? FM_TOOLBAR_H + FM_PATHBAR_H
            : FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
        int scroll_step = fm->grid_view ? FM_GRID_CELL_H : FM_ITEM_H;
        if (local_y >= list_y_start) {
            fm->scrollbar.scroll_offset -= ev.scroll * scroll_step;
            int ms = fm->scrollbar.max_scroll();
            if (fm->scrollbar.scroll_offset < 0) fm->scrollbar.scroll_offset = 0;
            if (fm->scrollbar.scroll_offset > ms) fm->scrollbar.scroll_offset = ms;
        }
    }
}

// ============================================================================
// Keyboard handling
// ============================================================================

static void filemanager_on_key(Window* win, const Montauk::KeyEvent& key) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm || !key.pressed) return;

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        filemanager_go_up(fm);
    } else if (key.scancode == 0x48) {
        // Up arrow
        if (fm->grid_view) {
            Rect cr = win->content_rect();
            int cols = (cr.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
            if (cols < 1) cols = 1;
            if (fm->selected >= cols) fm->selected -= cols;
        } else {
            if (fm->selected > 0) fm->selected--;
        }
    } else if (key.scancode == 0x50) {
        // Down arrow
        if (fm->grid_view) {
            Rect cr = win->content_rect();
            int cols = (cr.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
            if (cols < 1) cols = 1;
            if (fm->selected + cols < fm->entry_count) fm->selected += cols;
        } else {
            if (fm->selected < fm->entry_count - 1) fm->selected++;
        }
    } else if (key.scancode == 0x4B && !key.alt && fm->grid_view) {
        // Left arrow (grid view only)
        if (fm->selected > 0) fm->selected--;
    } else if (key.scancode == 0x4D && !key.alt && fm->grid_view) {
        // Right arrow (grid view only)
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
        montauk::mfree(win->app_data);
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
    FileManagerState* fm = (FileManagerState*)montauk::malloc(sizeof(FileManagerState));
    montauk::memset(fm, 0, sizeof(FileManagerState));
    montauk::strcpy(fm->current_path, "0:/");
    fm->selected = -1;
    fm->last_click_item = -1;
    fm->history_pos = -1;
    fm->history_count = 0;
    fm->desktop = ds;
    fm->grid_view = true;

    fm->scrollbar.init(0, 0, FM_SCROLLBAR_W, 100);

    filemanager_push_history(fm);
    filemanager_read_dir(fm);

    win->app_data = fm;
    win->on_draw = filemanager_on_draw;
    win->on_mouse = filemanager_on_mouse;
    win->on_key = filemanager_on_key;
    win->on_close = filemanager_on_close;
}
