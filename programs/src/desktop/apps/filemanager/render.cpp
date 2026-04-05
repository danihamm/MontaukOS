/*
    * render.cpp
    * Drawing code for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"

namespace filemanager {

void filemanager_draw_header(Canvas& c, FileManagerState* fm, Color toolbar_color, Color btn_bg) {
    DesktopState* ds = fm->desktop;

    // ---- Toolbar (32px) ----
    c.fill_rect(0, 0, c.w, FM_TOOLBAR_H, toolbar_color);

    // Navigation buttons: Back, Forward, Up, Home
    struct ToolBtn { int x; SvgIcon* icon; };
    ToolBtn nav_btns[4] = {
        {  4, ds ? &ds->icon_go_back    : nullptr },
        { 32, ds ? &ds->icon_go_forward : nullptr },
        { 60, ds ? &ds->icon_go_up      : nullptr },
        { 88, ds ? &ds->icon_home       : nullptr },
    };

    for (int i = 0; i < 4; i++) {
        int bx = nav_btns[i].x;
        int by = 4;
        c.fill_rect(bx, by, 24, 24, btn_bg);
        if (nav_btns[i].icon) {
            int ix = bx + (24 - nav_btns[i].icon->width) / 2;
            int iy = by + (24 - nav_btns[i].icon->height) / 2;
            c.icon(ix, iy, *nav_btns[i].icon);
        }
    }

    // View toggle button
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

    // Separator line between nav group and action group
    c.vline(152, 6, 20, colors::BORDER);

    // Action buttons: Copy, Cut, Paste, Rename, New Folder, Delete
    bool has_sel = fm->selected >= 0 && fm->selected < fm->entry_count
                   && !fm->at_drives_root && !fm->at_apps_view
                   && fm->entry_types[fm->selected] != 3;
    bool has_clip = fm->clipboard_has_data && !fm->at_drives_root && !fm->at_apps_view;
    Color dim_bg = Color::from_rgb(0xF0, 0xF0, 0xF0);

    // Icon toolbar buttons: Copy, Cut, Paste, Rename, New Folder, Delete
    struct ActionBtn { int x; SvgIcon* icon; bool enabled; };
    ActionBtn action_btns[] = {
        { 160, ds ? &ds->icon_copy       : nullptr, has_sel },
        { 188, ds ? &ds->icon_cut        : nullptr, has_sel },
        { 216, ds ? &ds->icon_paste      : nullptr, has_clip },
        { 244, ds ? &ds->icon_rename     : nullptr, has_sel },
        { 272, ds ? &ds->icon_folder_new : nullptr, !fm->at_drives_root && !fm->at_apps_view },
        { 300, ds ? &ds->icon_delete     : nullptr, has_sel },
    };

    for (int i = 0; i < 6; i++) {
        int bx = action_btns[i].x;
        int by = 4;
        c.fill_rect(bx, by, 24, 24, action_btns[i].enabled ? btn_bg : dim_bg);
        if (action_btns[i].icon && action_btns[i].icon->pixels) {
            int ix = bx + (24 - action_btns[i].icon->width) / 2;
            int iy = by + (24 - action_btns[i].icon->height) / 2;
            c.icon(ix, iy, *action_btns[i].icon);
        }
    }

    // Toolbar separator
    c.hline(0, FM_TOOLBAR_H - 1, c.w, colors::BORDER);

    // ---- Path bar ----
    int pathbar_y = FM_TOOLBAR_H;
    if (fm->pathbar_editing) {
        // Editable text input
        c.fill_rect(0, pathbar_y, c.w, FM_PATHBAR_H, colors::WHITE);
        c.rect(0, pathbar_y, c.w, FM_PATHBAR_H, colors::ACCENT);
        int fh = system_font_height();
        int ty = pathbar_y + (FM_PATHBAR_H - fh) / 2;
        c.text(8, ty, fm->pathbar_buf, colors::TEXT_COLOR);
        // Cursor
        char prefix[256];
        int cpos = fm->pathbar_cursor;
        if (cpos > 255) cpos = 255;
        for (int k = 0; k < cpos; k++) prefix[k] = fm->pathbar_buf[k];
        prefix[cpos] = '\0';
        int cx = 8 + text_width(prefix);
        c.vline(cx, ty, fh, colors::ACCENT);
    } else {
        c.fill_rect(0, pathbar_y, c.w, FM_PATHBAR_H, Color::from_rgb(0xF0, 0xF0, 0xF0));
        const char* pathbar_text = fm->current_path;
        if (fm->at_drives_root) pathbar_text = "Computer";
        else if (fm->at_apps_view) pathbar_text = "Apps";
        c.text(8, pathbar_y + 4, pathbar_text, colors::TEXT_COLOR);
    }

    // Path bar separator
    c.hline(0, pathbar_y + FM_PATHBAR_H - 1, c.w, colors::BORDER);
}

void filemanager_on_draw(Window* win, Framebuffer& fb) {
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
            // Check for special folder icon (type 7 in Computer, or type 1 dir with matching name)
            int sfi = -1;
            if (fm->entry_types[i] == 7) sfi = fm->drive_indices[i];
            else if (ds && fm->entry_types[i] == 1) sfi = special_folder_index(fm->entry_names[i]);

            if (sfi >= 0 && ds && ds->icon_special_folder_lg[sfi].pixels) {
                c.icon(icon_x, icon_y, ds->icon_special_folder_lg[sfi]);
            } else if (fm->entry_types[i] == 6 && i < fm->app_icon_count && fm->app_icons_lg[i].pixels) {
                c.icon(icon_x, icon_y, fm->app_icons_lg[i]);
            } else if (ds && fm->entry_types[i] == 5 && ds->icon_apps_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_apps_lg);
            } else if (ds && fm->entry_types[i] == 4 && ds->icon_home_folder_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_home_folder_lg);
            } else if (ds && fm->entry_types[i] == 3 && ds->icon_drive_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_drive_lg);
            } else if (ds && fm->entry_types[i] == 1 && ds->icon_folder_lg.pixels) {
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
            // If renaming this entry, draw the rename textbox instead
            if (fm->rename_active && fm->rename_idx == i) {
                int ty = icon_y + FM_GRID_ICON + 2;
                int tw = FM_GRID_CELL_W - 4;
                if (ty >= list_y && ty + system_font_height() + 4 <= c.h) {
                    c.fill_rect(cell_x + 2, ty - 1, tw, system_font_height() + 2, colors::WHITE);
                    c.rect(cell_x + 2, ty - 1, tw, system_font_height() + 2, colors::ACCENT);
                    // Draw rename text (truncated to cell width)
                    char display[16];
                    int rlen = fm->rename_len;
                    if (rlen > 10) rlen = 10;
                    for (int k = 0; k < rlen; k++) display[k] = fm->rename_buf[k];
                    display[rlen] = '\0';
                    c.text(cell_x + 4, ty, display, colors::TEXT_COLOR);
                    // Cursor
                    char prefix[64];
                    int cpos = fm->rename_cursor < rlen ? fm->rename_cursor : rlen;
                    for (int k = 0; k < cpos; k++) prefix[k] = fm->rename_buf[k];
                    prefix[cpos] = '\0';
                    int cx = cell_x + 4 + text_width(prefix);
                    c.vline(cx, ty, system_font_height(), colors::ACCENT);
                }
            } else {
                char label[64];
                int nlen = montauk::slen(fm->entry_names[i]);
                if (nlen > 60) {
                    for (int k = 0; k < 60; k++) label[k] = fm->entry_names[i][k];
                    label[60] = '.';
                    label[61] = '.';
                    label[62] = '\0';
                } else {
                    montauk::strncpy(label, fm->entry_names[i], 63);
                }
                int tw = text_width(label);
                int tx = cell_x + (FM_GRID_CELL_W - tw) / 2;
                if (tx < cell_x) tx = cell_x;
                int ty = icon_y + FM_GRID_ICON + 2;
                if (ty >= list_y && ty + system_font_height() <= c.h)
                    c.text(tx, ty, label, colors::TEXT_COLOR);
            }
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

            // Icon (skip if it would bleed above the list area)
            int ico_x = 8;
            int ico_y = iy + (FM_ITEM_H - 16) / 2;
            // Check for special folder icon
            int sfi_sm = -1;
            if (fm->entry_types[i] == 7) sfi_sm = fm->drive_indices[i];
            else if (ds && fm->entry_types[i] == 1) sfi_sm = special_folder_index(fm->entry_names[i]);

            if (ico_y < list_y) { /* clipped by header repaint */ }
            else if (sfi_sm >= 0 && ds && ds->icon_special_folder[sfi_sm].pixels) {
                c.icon(ico_x, ico_y, ds->icon_special_folder[sfi_sm]);
            } else if (fm->entry_types[i] == 6 && i < fm->app_icon_count && fm->app_icons_sm[i].pixels) {
                c.icon(ico_x, ico_y, fm->app_icons_sm[i]);
            } else if (ds && fm->entry_types[i] == 5 && ds->icon_apps.pixels) {
                c.icon(ico_x, ico_y, ds->icon_apps);
            } else if (ds && fm->entry_types[i] == 4 && ds->icon_home_folder.pixels) {
                c.icon(ico_x, ico_y, ds->icon_home_folder);
            } else if (ds && fm->entry_types[i] == 3 && ds->icon_drive.pixels) {
                c.icon(ico_x, ico_y, ds->icon_drive);
            } else if (ds && fm->entry_types[i] == 1 && ds->icon_folder.pixels) {
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

            // Name (or rename textbox)
            int tx = 30;
            int fm_sfh = system_font_height();
            int ty = iy + (FM_ITEM_H - fm_sfh) / 2;

            if (fm->rename_active && fm->rename_idx == i && ty >= list_y && ty + fm_sfh <= c.h) {
                // Rename textbox inline
                int rw = (size_col_x > 100 ? size_col_x - 8 : c.w - FM_SCROLLBAR_W - 8) - tx;
                c.fill_rect(tx - 2, ty - 1, rw, fm_sfh + 2, colors::WHITE);
                c.rect(tx - 2, ty - 1, rw, fm_sfh + 2, colors::ACCENT);
                c.text(tx, ty, fm->rename_buf, colors::TEXT_COLOR);
                // Cursor
                char prefix[64];
                int cpos = fm->rename_cursor;
                if (cpos > 63) cpos = 63;
                for (int k = 0; k < cpos; k++) prefix[k] = fm->rename_buf[k];
                prefix[cpos] = '\0';
                int cx = tx + text_width(prefix);
                c.vline(cx, ty, fm_sfh, colors::ACCENT);
            } else {
                if (ty >= list_y && ty + fm_sfh <= c.h)
                    c.text(tx, ty, fm->entry_names[i], colors::TEXT_COLOR);
            }

            // Size
            if (size_col_x > 100 && !fm->is_dir[i] && ty >= list_y && ty + fm_sfh <= c.h) {
                char size_str[16];
                format_size(size_str, fm->entry_sizes[i]);
                c.text(size_col_x, ty, size_str, dim);
            }

            // Type
            if (type_col_x > 160 && ty >= list_y && ty + fm_sfh <= c.h) {
                const char* type_str = "File";
                if (fm->entry_types[i] == 6) type_str = "App";
                else if (fm->entry_types[i] == 5) type_str = "Apps";
                else if (fm->entry_types[i] == 4) type_str = "Home";
                else if (fm->entry_types[i] == 3) type_str = "Drive";
                else if (fm->entry_types[i] == 1) type_str = "Dir";
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

    // ---- Context menu overlay ----
    if (fm->ctx_open && fm->ctx_item_count > 0) {
        int cmx = fm->ctx_x;
        int cmy = fm->ctx_y;
        int cmh = fm->ctx_item_count * CTX_ITEM_H + 8;

        // Clamp to window bounds
        if (cmx + CTX_MENU_W > c.w) cmx = c.w - CTX_MENU_W;
        if (cmy + cmh > c.h) cmy = c.h - cmh;
        if (cmx < 0) cmx = 0;
        if (cmy < 0) cmy = 0;

        // Shadow
        c.fill_rect(cmx + 2, cmy + 2, CTX_MENU_W, cmh, Color::from_rgb(0x80, 0x80, 0x80));
        // Background
        c.fill_rounded_rect(cmx, cmy, CTX_MENU_W, cmh, 4, colors::WHITE);
        c.rect(cmx, cmy, CTX_MENU_W, cmh, colors::BORDER);

        for (int i = 0; i < fm->ctx_item_count; i++) {
            int iy = cmy + 4 + i * CTX_ITEM_H;

            // Hover highlight
            if (i == fm->ctx_hover)
                c.fill_rect(cmx + 2, iy, CTX_MENU_W - 4, CTX_ITEM_H, colors::MENU_HOVER);

            int fh = system_font_height();
            int ty = iy + (CTX_ITEM_H - fh) / 2;
            c.text(cmx + 12, ty, ctx_label(fm->ctx_items[i]), colors::TEXT_COLOR);
        }
    }
}

} // namespace filemanager
