/*
    * events.cpp
    * Mouse and keyboard handling for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"

namespace filemanager {

void filemanager_on_mouse(Window* win, MouseEvent& ev) {
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

    // ---- Context menu interaction ----
    if (fm->ctx_open) {
        int cmx = fm->ctx_x;
        int cmy = fm->ctx_y;
        int cmh = fm->ctx_item_count * CTX_ITEM_H + 8;
        if (cmx + CTX_MENU_W > cw) cmx = cw - CTX_MENU_W;
        if (cmy + cmh > win->content_h) cmy = win->content_h - cmh;
        if (cmx < 0) cmx = 0;
        if (cmy < 0) cmy = 0;

        // Update hover
        if (local_x >= cmx && local_x < cmx + CTX_MENU_W &&
            local_y >= cmy + 4 && local_y < cmy + 4 + fm->ctx_item_count * CTX_ITEM_H) {
            fm->ctx_hover = (local_y - cmy - 4) / CTX_ITEM_H;
        } else {
            fm->ctx_hover = -1;
        }

        if (ev.left_pressed()) {
            if (fm->ctx_hover >= 0 && fm->ctx_hover < fm->ctx_item_count) {
                int action = fm->ctx_items[fm->ctx_hover];
                int target = fm->ctx_target_idx;
                filemanager_close_ctx_menu(fm);

                // Select target if needed
                if (target >= 0 && target < fm->entry_count)
                    fm->selected = target;

                switch (action) {
                case CTX_OPEN:       filemanager_open_entry(fm, target); break;
                case CTX_COPY:       filemanager_do_copy(fm); break;
                case CTX_CUT:        filemanager_do_cut(fm); break;
                case CTX_PASTE:      filemanager_do_paste(fm); break;
                case CTX_RENAME:     filemanager_start_rename(fm); break;
                case CTX_DELETE:     filemanager_delete_selected(fm); break;
                case CTX_NEW_FOLDER: filemanager_new_folder(fm); break;
                }
            } else {
                filemanager_close_ctx_menu(fm);
            }
            return;
        }
        if (ev.right_pressed()) {
            filemanager_close_ctx_menu(fm);
            return;
        }
        return;
    }

    // ---- Cancel rename on click outside rename area ----
    if (fm->rename_active && ev.left_pressed()) {
        filemanager_finish_rename(fm);
    }

    if (ev.left_pressed()) {
        // Click on path bar area
        if (local_y >= FM_TOOLBAR_H && local_y < FM_TOOLBAR_H + FM_PATHBAR_H) {
            if (!fm->pathbar_editing)
                filemanager_start_pathbar(fm);
            return;
        }

        // Clicking anywhere else while pathbar is editing commits the path
        if (fm->pathbar_editing) {
            filemanager_commit_pathbar(fm);
        }

        // Toolbar button clicks
        if (local_y < FM_TOOLBAR_H) {
            // Navigation buttons
            if (local_x >= 4 && local_x < 28)       filemanager_go_back(fm);
            else if (local_x >= 32 && local_x < 56)  filemanager_go_forward(fm);
            else if (local_x >= 60 && local_x < 84)  filemanager_go_up(fm);
            else if (local_x >= 88 && local_x < 112)  filemanager_go_home(fm);
            else if (local_x >= 120 && local_x < 144) {
                fm->grid_view = !fm->grid_view;
                fm->scrollbar.scroll_offset = 0;
            }
            // Action buttons
            else if (local_x >= 160 && local_x < 184) filemanager_do_copy(fm);
            else if (local_x >= 188 && local_x < 212) filemanager_do_cut(fm);
            else if (local_x >= 216 && local_x < 240) filemanager_do_paste(fm);
            else if (local_x >= 244 && local_x < 268) filemanager_start_rename(fm);
            else if (local_x >= 272 && local_x < 296) filemanager_new_folder(fm);
            else if (local_x >= 300 && local_x < 324) filemanager_delete_selected(fm);
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
                        filemanager_open_entry(fm, clicked_idx);
                        fm->last_click_item = -1;
                        fm->last_click_time = 0;
                    } else {
                        fm->selected = clicked_idx;
                        fm->last_click_item = clicked_idx;
                        fm->last_click_time = now;
                    }
                } else {
                    fm->selected = -1;
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
                        filemanager_open_entry(fm, clicked_idx);
                        fm->last_click_item = -1;
                        fm->last_click_time = 0;
                    } else {
                        fm->selected = clicked_idx;
                        fm->last_click_item = clicked_idx;
                        fm->last_click_time = now;
                    }
                } else {
                    fm->selected = -1;
                }
            }
        }
    }

    // ---- Right-click: open context menu ----
    if (ev.right_pressed()) {
        filemanager_cancel_rename(fm);

        // Determine what was right-clicked
        int target_idx = -1;

        if (local_y >= FM_TOOLBAR_H + FM_PATHBAR_H) {
            if (fm->grid_view) {
                int list_y = FM_TOOLBAR_H + FM_PATHBAR_H;
                if (local_x < cw - FM_SCROLLBAR_W) {
                    int cols = (cw - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
                    if (cols < 1) cols = 1;
                    int col = local_x / FM_GRID_CELL_W;
                    int row = (local_y - list_y + fm->scrollbar.scroll_offset) / FM_GRID_CELL_H;
                    int idx = row * cols + col;
                    if (idx >= 0 && idx < fm->entry_count && col < cols) {
                        target_idx = idx;
                        fm->selected = idx;
                    }
                }
            } else {
                int list_y = FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
                if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
                    int rel_y = local_y - list_y + fm->scrollbar.scroll_offset;
                    int idx = rel_y / FM_ITEM_H;
                    if (idx >= 0 && idx < fm->entry_count) {
                        target_idx = idx;
                        fm->selected = idx;
                    }
                }
            }
        }

        filemanager_open_ctx_menu(fm, local_x, local_y, target_idx);
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

void filemanager_on_key(Window* win, const Montauk::KeyEvent& key) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm || !key.pressed) return;

    // ---- Path bar editing key handling ----
    if (fm->pathbar_editing) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            filemanager_commit_pathbar(fm);
        } else if (key.scancode == 0x01) {
            filemanager_cancel_pathbar(fm);
        } else if (key.ascii == '\b' || key.scancode == 0x0E) {
            if (fm->pathbar_cursor > 0) {
                for (int i = fm->pathbar_cursor - 1; i < fm->pathbar_len - 1; i++)
                    fm->pathbar_buf[i] = fm->pathbar_buf[i + 1];
                fm->pathbar_len--;
                fm->pathbar_cursor--;
                fm->pathbar_buf[fm->pathbar_len] = '\0';
            }
        } else if (key.scancode == 0x53) {
            if (fm->pathbar_cursor < fm->pathbar_len) {
                for (int i = fm->pathbar_cursor; i < fm->pathbar_len - 1; i++)
                    fm->pathbar_buf[i] = fm->pathbar_buf[i + 1];
                fm->pathbar_len--;
                fm->pathbar_buf[fm->pathbar_len] = '\0';
            }
        } else if (key.scancode == 0x4B) {
            if (fm->pathbar_cursor > 0) fm->pathbar_cursor--;
        } else if (key.scancode == 0x4D) {
            if (fm->pathbar_cursor < fm->pathbar_len) fm->pathbar_cursor++;
        } else if (key.scancode == 0x47) {
            fm->pathbar_cursor = 0;
        } else if (key.scancode == 0x4F) {
            fm->pathbar_cursor = fm->pathbar_len;
        } else if (key.ascii >= 32 && key.ascii < 127 && fm->pathbar_len < 254) {
            for (int i = fm->pathbar_len; i > fm->pathbar_cursor; i--)
                fm->pathbar_buf[i] = fm->pathbar_buf[i - 1];
            fm->pathbar_buf[fm->pathbar_cursor] = key.ascii;
            fm->pathbar_cursor++;
            fm->pathbar_len++;
            fm->pathbar_buf[fm->pathbar_len] = '\0';
        }
        return;
    }

    // ---- Rename mode key handling ----
    if (fm->rename_active) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            filemanager_finish_rename(fm);
        } else if (key.scancode == 0x01) {
            // Escape
            filemanager_cancel_rename(fm);
        } else if (key.ascii == '\b' || key.scancode == 0x0E) {
            // Backspace
            if (fm->rename_cursor > 0) {
                for (int i = fm->rename_cursor - 1; i < fm->rename_len - 1; i++)
                    fm->rename_buf[i] = fm->rename_buf[i + 1];
                fm->rename_len--;
                fm->rename_cursor--;
                fm->rename_buf[fm->rename_len] = '\0';
            }
        } else if (key.scancode == 0x53) {
            // Delete key
            if (fm->rename_cursor < fm->rename_len) {
                for (int i = fm->rename_cursor; i < fm->rename_len - 1; i++)
                    fm->rename_buf[i] = fm->rename_buf[i + 1];
                fm->rename_len--;
                fm->rename_buf[fm->rename_len] = '\0';
            }
        } else if (key.scancode == 0x4B) {
            // Left arrow
            if (fm->rename_cursor > 0) fm->rename_cursor--;
        } else if (key.scancode == 0x4D) {
            // Right arrow
            if (fm->rename_cursor < fm->rename_len) fm->rename_cursor++;
        } else if (key.scancode == 0x47) {
            // Home
            fm->rename_cursor = 0;
        } else if (key.scancode == 0x4F) {
            // End
            fm->rename_cursor = fm->rename_len;
        } else if (key.ascii >= 32 && key.ascii < 127) {
            // Printable character (reject / and other FS-unsafe chars)
            if (key.ascii != '/' && key.ascii != '\\' && fm->rename_len < 62) {
                for (int i = fm->rename_len; i > fm->rename_cursor; i--)
                    fm->rename_buf[i] = fm->rename_buf[i - 1];
                fm->rename_buf[fm->rename_cursor] = key.ascii;
                fm->rename_cursor++;
                fm->rename_len++;
                fm->rename_buf[fm->rename_len] = '\0';
            }
        }
        return;
    }

    // ---- Context menu: Escape to close ----
    if (fm->ctx_open && key.scancode == 0x01) {
        filemanager_close_ctx_menu(fm);
        return;
    }

    // ---- Keyboard shortcuts ----
    if (key.ctrl) {
        if (key.ascii == 'c' || key.ascii == 'C') {
            filemanager_do_copy(fm);
            return;
        }
        if (key.ascii == 'x' || key.ascii == 'X') {
            filemanager_do_cut(fm);
            return;
        }
        if (key.ascii == 'v' || key.ascii == 'V') {
            filemanager_do_paste(fm);
            return;
        }
    }

    // F2 = rename
    if (key.scancode == 0x3C) {
        filemanager_start_rename(fm);
        return;
    }

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        filemanager_go_up(fm);
    } else if (key.scancode == 0x48) {
        // Up arrow
        if (fm->grid_view) {
            Rect cr_r = win->content_rect();
            int cols = (cr_r.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
            if (cols < 1) cols = 1;
            if (fm->selected >= cols) fm->selected -= cols;
        } else {
            if (fm->selected > 0) fm->selected--;
        }
    } else if (key.scancode == 0x50) {
        // Down arrow
        if (fm->grid_view) {
            Rect cr_r = win->content_rect();
            int cols = (cr_r.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
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
        if (fm->selected >= 0 && fm->selected < fm->entry_count)
            filemanager_open_entry(fm, fm->selected);
    } else if (key.scancode == 0x53) {
        // Delete key
        filemanager_delete_selected(fm);
    } else if (key.alt && key.scancode == 0x4B) {
        // Alt+Left: go back
        filemanager_go_back(fm);
    } else if (key.alt && key.scancode == 0x4D) {
        // Alt+Right: go forward
        filemanager_go_forward(fm);
    }
}

void filemanager_on_close(Window* win) {
    if (win->app_data) {
        FileManagerState* fm = (FileManagerState*)win->app_data;
        filemanager_free_app_icons(fm);
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

} // namespace filemanager
