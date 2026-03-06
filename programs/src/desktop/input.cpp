/*
    * input.cpp
    * Mouse and keyboard event handling
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

void gui::desktop_handle_mouse(DesktopState* ds) {
    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    uint8_t buttons = ds->mouse.buttons;
    uint8_t prev = ds->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);
    bool left_held = (buttons & 0x01);
    bool left_released = !(buttons & 0x01) && (prev & 0x01);
    bool right_pressed = (buttons & 0x02) && !(prev & 0x02);

    MouseEvent ev;
    ev.x = mx;
    ev.y = my;
    ev.buttons = buttons;
    ev.prev_buttons = prev;
    ev.scroll = ds->mouse.scrollDelta;

    // Handle context menu clicks
    if (ds->ctx_menu_open) {
        if (left_pressed) {
            static constexpr int CTX_MENU_W = 180;
            static constexpr int CTX_ITEM_H = 36;
            static constexpr int CTX_ITEM_COUNT = 5;
            int cmx = ds->ctx_menu_x;
            int cmy = ds->ctx_menu_y;
            int cmh = CTX_ITEM_H * CTX_ITEM_COUNT + 8;
            if (cmx + CTX_MENU_W > ds->screen_w) cmx = ds->screen_w - CTX_MENU_W;
            if (cmy + cmh > ds->screen_h) cmy = ds->screen_h - cmh;

            Rect ctx_rect = {cmx, cmy, CTX_MENU_W, cmh};
            if (ctx_rect.contains(mx, my)) {
                int rel_y = my - cmy - 4;
                int item_idx = rel_y / CTX_ITEM_H;
                if (item_idx >= 0 && item_idx < CTX_ITEM_COUNT) {
                    ds->ctx_menu_open = false;
                    switch (item_idx) {
                    case 0: open_terminal(ds); break;
                    case 1: open_filemanager(ds); break;
                    case 2: open_settings(ds); break;
                    case 3: open_reboot_dialog(ds); break;
                    case 4: open_shutdown_dialog(ds); break;
                    }
                    return;
                }
            }
            ds->ctx_menu_open = false;
            return;
        }
        if (right_pressed) {
            ds->ctx_menu_open = false;
            return;
        }
    }

    // Check for ongoing window drags first
    for (int i = 0; i < ds->window_count; i++) {
        Window* win = &ds->windows[i];
        if (win->dragging) {
            if (left_held) {
                win->frame.x = mx - win->drag_offset_x;
                win->frame.y = my - win->drag_offset_y;
                if (win->frame.x < -win->frame.w + 50) win->frame.x = -win->frame.w + 50;
                if (win->frame.y < 0) win->frame.y = 0;
                if (win->frame.x > ds->screen_w - 50) win->frame.x = ds->screen_w - 50;
                if (win->frame.y > ds->screen_h - 50) win->frame.y = ds->screen_h - 50;
            }
            if (left_released) {
                win->dragging = false;
                // Window edge snapping
                if (mx <= 0) {
                    win->saved_frame = win->frame;
                    win->frame = {0, PANEL_HEIGHT, ds->screen_w / 2, ds->screen_h - PANEL_HEIGHT};
                    win->state = WIN_MAXIMIZED;
                    if (!win->external) {
                        Rect cr = win->content_rect();
                        if (cr.w != win->content_w || cr.h != win->content_h) {
                            if (win->content) montauk::free(win->content);
                            win->content_w = cr.w;
                            win->content_h = cr.h;
                            win->content = (uint32_t*)montauk::alloc(cr.w * cr.h * 4);
                            montauk::memset(win->content, 0xFF, cr.w * cr.h * 4);
                        }
                    } else {
                        Rect cr = win->content_rect();
                        Montauk::WinEvent rev;
                        montauk::memset(&rev, 0, sizeof(rev));
                        rev.type = 2;
                        rev.resize.w = cr.w;
                        rev.resize.h = cr.h;
                        montauk::win_sendevent(win->ext_win_id, &rev);
                    }
                } else if (mx >= ds->screen_w - 1) {
                    win->saved_frame = win->frame;
                    win->frame = {ds->screen_w / 2, PANEL_HEIGHT, ds->screen_w / 2, ds->screen_h - PANEL_HEIGHT};
                    win->state = WIN_MAXIMIZED;
                    if (!win->external) {
                        Rect cr = win->content_rect();
                        if (cr.w != win->content_w || cr.h != win->content_h) {
                            if (win->content) montauk::free(win->content);
                            win->content_w = cr.w;
                            win->content_h = cr.h;
                            win->content = (uint32_t*)montauk::alloc(cr.w * cr.h * 4);
                            montauk::memset(win->content, 0xFF, cr.w * cr.h * 4);
                        }
                    } else {
                        Rect cr = win->content_rect();
                        Montauk::WinEvent rev;
                        montauk::memset(&rev, 0, sizeof(rev));
                        rev.type = 2;
                        rev.resize.w = cr.w;
                        rev.resize.h = cr.h;
                        montauk::win_sendevent(win->ext_win_id, &rev);
                    }
                }
            }
            return;
        }
    }

    // Check for ongoing window resizes
    for (int i = 0; i < ds->window_count; i++) {
        Window* win = &ds->windows[i];
        if (win->resizing) {
            if (left_held) {
                int dx = mx - win->resize_start_mx;
                int dy = my - win->resize_start_my;
                Rect sf = win->resize_start_frame;
                ResizeEdge edge = win->resize_edge;

                int new_x = sf.x, new_y = sf.y, new_w = sf.w, new_h = sf.h;

                if (edge == RESIZE_RIGHT || edge == RESIZE_TOP_RIGHT || edge == RESIZE_BOTTOM_RIGHT)
                    new_w = sf.w + dx;
                if (edge == RESIZE_BOTTOM || edge == RESIZE_BOTTOM_LEFT || edge == RESIZE_BOTTOM_RIGHT)
                    new_h = sf.h + dy;
                if (edge == RESIZE_LEFT || edge == RESIZE_TOP_LEFT || edge == RESIZE_BOTTOM_LEFT) {
                    new_x = sf.x + dx;
                    new_w = sf.w - dx;
                }
                if (edge == RESIZE_TOP || edge == RESIZE_TOP_LEFT || edge == RESIZE_TOP_RIGHT) {
                    new_y = sf.y + dy;
                    new_h = sf.h - dy;
                }

                // Enforce minimum size
                if (new_w < MIN_WINDOW_W) {
                    if (edge == RESIZE_LEFT || edge == RESIZE_TOP_LEFT || edge == RESIZE_BOTTOM_LEFT)
                        new_x = sf.x + sf.w - MIN_WINDOW_W;
                    new_w = MIN_WINDOW_W;
                }
                if (new_h < MIN_WINDOW_H) {
                    if (edge == RESIZE_TOP || edge == RESIZE_TOP_LEFT || edge == RESIZE_TOP_RIGHT)
                        new_y = sf.y + sf.h - MIN_WINDOW_H;
                    new_h = MIN_WINDOW_H;
                }

                win->frame = {new_x, new_y, new_w, new_h};
            }
            if (left_released) {
                win->resizing = false;
                // Reallocate content buffer if dimensions changed (skip for external)
                if (!win->external) {
                    Rect cr = win->content_rect();
                    if (cr.w != win->content_w || cr.h != win->content_h) {
                        if (win->content) montauk::free(win->content);
                        win->content_w = cr.w;
                        win->content_h = cr.h;
                        win->content = (uint32_t*)montauk::alloc(cr.w * cr.h * 4);
                        montauk::memset(win->content, 0xFF, cr.w * cr.h * 4);
                    }
                    win->dirty = true;
                } else {
                    Rect cr = win->content_rect();
                    Montauk::WinEvent rev;
                    montauk::memset(&rev, 0, sizeof(rev));
                    rev.type = 2;
                    rev.resize.w = cr.w;
                    rev.resize.h = cr.h;
                    montauk::win_sendevent(win->ext_win_id, &rev);
                }
            }
            return;
        }
    }

    // Handle app menu clicks
    if (ds->app_menu_open && left_pressed) {
        int menu_x = 4;
        int menu_y = PANEL_HEIGHT + 2;
        int menu_h = menu_total_height();
        Rect menu_rect = {menu_x, menu_y, MENU_W, menu_h};

        if (menu_rect.contains(mx, my)) {
            // Walk visible rows to find which one was clicked
            int iy = menu_y + 5;
            int cur_cat = -1;
            for (int i = 0; i < MENU_ROW_COUNT; i++) {
                const MenuRow& row = menu_rows[i];
                if (row.is_category) cur_cat++;
                if (!menu_row_visible(i)) continue;
                int row_h = menu_row_height(row);
                if (my >= iy && my < iy + row_h) {
                    if (row.is_category && row.label[0] && cur_cat >= 0 && cur_cat < MENU_NUM_CATS - 1) {
                        // Toggle category expand/collapse
                        menu_cat_expanded[cur_cat] = !menu_cat_expanded[cur_cat];
                    } else if (!row.is_category) {
                        switch (row.app_id) {
                        case 0: open_terminal(ds); break;
                        case 1: open_filemanager(ds); break;
                        case 2: open_sysinfo(ds); break;
                        case 3: open_calculator(ds); break;
                        case 4: open_texteditor(ds); break;
                        case 5: open_klog(ds); break;
                        case 6: open_procmgr(ds); break;
                        case 7: open_mandelbrot(ds); break;
                        case 8: open_devexplorer(ds); break;
                        case 9: open_wiki(ds); break;
                        case 10: open_doom(ds); break;
                        case 11: open_settings(ds); break;
                        case 12: open_reboot_dialog(ds); break;
                        case 13: open_weather(ds); break;
                        case 14: open_shutdown_dialog(ds); break;
                        case 15: open_wordprocessor(ds); break;
                        case 16: open_spreadsheet(ds); break;
                        }
                        ds->app_menu_open = false;
                    }
                    break;
                }
                iy += row_h;
            }
            return;
        } else {
            ds->app_menu_open = false;
        }
    }

    // Handle net popup clicks
    if (ds->net_popup_open && left_pressed) {
        int popup_w = 220;
        int popup_h = 130;
        int popup_x = ds->net_icon_rect.x + ds->net_icon_rect.w - popup_w;
        int popup_y = PANEL_HEIGHT + 2;
        if (popup_x < 4) popup_x = 4;
        Rect popup_rect = {popup_x, popup_y, popup_w, popup_h};

        if (popup_rect.contains(mx, my)) {
            return;
        } else if (!ds->net_icon_rect.contains(mx, my)) {
            ds->net_popup_open = false;
        }
    }

    // Panel click check
    if (left_pressed && my < PANEL_HEIGHT) {
        // App menu button
        if (mx < 36) {
            ds->app_menu_open = !ds->app_menu_open;
            ds->net_popup_open = false;
            ds->ctx_menu_open = false;
            return;
        }

        // Network icon
        if (ds->net_icon_rect.w > 0 && ds->net_icon_rect.contains(mx, my)) {
            ds->net_popup_open = !ds->net_popup_open;
            ds->app_menu_open = false;
            ds->ctx_menu_open = false;
            return;
        }
        // Window indicator buttons
        int indicator_x = 40;
        for (int i = 0; i < ds->window_count; i++) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_CLOSED) continue;

            int tw = text_width(win->title);
            int pad = 12;
            int iw = tw + pad * 2;
            if (iw > 150) iw = 150;

            Rect btn_rect = {indicator_x, 4, iw, 24};
            if (btn_rect.contains(mx, my)) {
                if (win->state == WIN_MINIMIZED) {
                    win->state = WIN_NORMAL;
                }
                desktop_raise_window(ds, i);
                return;
            }
            indicator_x += iw + 4;
        }
        return;
    }

    // Window interaction: check from top (last) to bottom (first)
    if (left_pressed) {
        for (int i = ds->window_count - 1; i >= 0; i--) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_MINIMIZED || win->state == WIN_CLOSED) continue;

            // Check close button
            Rect close_r = win->close_btn_rect();
            if (close_r.contains(mx, my)) {
                desktop_close_window(ds, i);
                return;
            }

            // Check minimize button
            Rect min_r = win->min_btn_rect();
            if (min_r.contains(mx, my)) {
                win->state = WIN_MINIMIZED;
                if (ds->focused_window == i) {
                    ds->focused_window = -1;
                    for (int j = ds->window_count - 1; j >= 0; j--) {
                        if (ds->windows[j].state == WIN_NORMAL || ds->windows[j].state == WIN_MAXIMIZED) {
                            ds->focused_window = j;
                            ds->windows[j].focused = true;
                            break;
                        }
                    }
                }
                return;
            }

            // Check maximize button
            Rect max_r = win->max_btn_rect();
            if (max_r.contains(mx, my)) {
                if (win->state == WIN_MAXIMIZED) {
                    win->frame = win->saved_frame;
                    win->state = WIN_NORMAL;
                } else {
                    win->saved_frame = win->frame;
                    win->frame = {0, PANEL_HEIGHT, ds->screen_w, ds->screen_h - PANEL_HEIGHT};
                    win->state = WIN_MAXIMIZED;
                }
                // Reallocate content buffer for local windows only
                if (!win->external) {
                    Rect cr = win->content_rect();
                    if (cr.w != win->content_w || cr.h != win->content_h) {
                        if (win->content) montauk::free(win->content);
                        win->content_w = cr.w;
                        win->content_h = cr.h;
                        win->content = (uint32_t*)montauk::alloc(cr.w * cr.h * 4);
                        montauk::memset(win->content, 0xFF, cr.w * cr.h * 4);
                    }
                } else {
                    Rect cr = win->content_rect();
                    Montauk::WinEvent rev;
                    montauk::memset(&rev, 0, sizeof(rev));
                    rev.type = 2;
                    rev.resize.w = cr.w;
                    rev.resize.h = cr.h;
                    montauk::win_sendevent(win->ext_win_id, &rev);
                }
                desktop_raise_window(ds, i);
                return;
            }

            // Check resize edges (before titlebar drag, so corner grabs work)
            if (win->state != WIN_MAXIMIZED) {
                ResizeEdge edge = hit_test_resize_edge(win->frame, mx, my);
                if (edge != RESIZE_NONE) {
                    win->resizing = true;
                    win->resize_edge = edge;
                    win->resize_start_frame = win->frame;
                    win->resize_start_mx = mx;
                    win->resize_start_my = my;
                    desktop_raise_window(ds, i);
                    int new_idx = ds->window_count - 1;
                    ds->windows[new_idx].resizing = true;
                    ds->windows[new_idx].resize_edge = edge;
                    ds->windows[new_idx].resize_start_frame = ds->windows[new_idx].frame;
                    ds->windows[new_idx].resize_start_mx = mx;
                    ds->windows[new_idx].resize_start_my = my;
                    return;
                }
            }

            // Check titlebar (start drag)
            Rect tb = win->titlebar_rect();
            if (tb.contains(mx, my)) {
                win->dragging = true;
                win->drag_offset_x = mx - win->frame.x;
                win->drag_offset_y = my - win->frame.y;
                desktop_raise_window(ds, i);
                int new_idx = ds->window_count - 1;
                ds->windows[new_idx].dragging = true;
                ds->windows[new_idx].drag_offset_x = mx - ds->windows[new_idx].frame.x;
                ds->windows[new_idx].drag_offset_y = my - ds->windows[new_idx].frame.y;
                return;
            }

            // Check content area
            Rect cr = win->content_rect();
            if (cr.contains(mx, my)) {
                desktop_raise_window(ds, i);
                int new_idx = ds->window_count - 1;
                Window* raised = &ds->windows[new_idx];
                if (raised->external) {
                    // Forward mouse event to external window
                    Montauk::WinEvent wev;
                    montauk::memset(&wev, 0, sizeof(wev));
                    wev.type = 1; // mouse
                    wev.mouse.x = mx - cr.x;
                    wev.mouse.y = my - cr.y;
                    wev.mouse.scroll = ev.scroll;
                    wev.mouse.buttons = buttons;
                    wev.mouse.prev_buttons = prev;
                    montauk::win_sendevent(raised->ext_win_id, &wev);
                } else if (raised->on_mouse) {
                    ev.x = mx;
                    ev.y = my;
                    raised->on_mouse(raised, ev);
                }
                return;
            }

            // Check full frame
            if (win->frame.contains(mx, my)) {
                desktop_raise_window(ds, i);
                return;
            }
        }

        ds->app_menu_open = false;
        ds->ctx_menu_open = false;
    }

    // Forward continuous mouse events to focused window (hover, drag, release)
    if (!left_pressed && ds->focused_window >= 0) {
        Window* win = &ds->windows[ds->focused_window];
        if (win->state != WIN_MINIMIZED && win->state != WIN_CLOSED) {
            Rect cr = win->content_rect();
            // Forward if mouse is over content (hover/move) or button is held/released (drag continuity)
            if (cr.contains(mx, my) || left_held || left_released) {
                if (win->external) {
                    Montauk::WinEvent wev;
                    montauk::memset(&wev, 0, sizeof(wev));
                    wev.type = 1; // mouse
                    wev.mouse.x = mx - cr.x;
                    wev.mouse.y = my - cr.y;
                    wev.mouse.scroll = 0;
                    wev.mouse.buttons = buttons;
                    wev.mouse.prev_buttons = prev;
                    montauk::win_sendevent(win->ext_win_id, &wev);
                } else if (win->on_mouse) {
                    ev.x = mx;
                    ev.y = my;
                    win->on_mouse(win, ev);
                }
            }
        }
    }

    // Handle scroll events on focused window
    if (ev.scroll != 0 && ds->focused_window >= 0) {
        Window* win = &ds->windows[ds->focused_window];
        Rect cr = win->content_rect();
        if (cr.contains(mx, my)) {
            if (win->external) {
                Montauk::WinEvent wev;
                montauk::memset(&wev, 0, sizeof(wev));
                wev.type = 1; // mouse
                wev.mouse.x = mx - cr.x;
                wev.mouse.y = my - cr.y;
                wev.mouse.scroll = ev.scroll;
                wev.mouse.buttons = buttons;
                wev.mouse.prev_buttons = prev;
                montauk::win_sendevent(win->ext_win_id, &wev);
            } else if (win->on_mouse) {
                win->on_mouse(win, ev);
            }
        }
    }

    // Right-click on desktop background opens context menu
    if (right_pressed && my >= PANEL_HEIGHT) {
        bool on_window = false;
        for (int i = ds->window_count - 1; i >= 0; i--) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_MINIMIZED || win->state == WIN_CLOSED) continue;
            if (win->frame.contains(mx, my)) {
                on_window = true;
                break;
            }
        }
        if (!on_window) {
            ds->ctx_menu_open = true;
            ds->ctx_menu_x = mx;
            ds->ctx_menu_y = my;
            ds->app_menu_open = false;
            ds->net_popup_open = false;
        }
    }
}

void gui::desktop_handle_keyboard(DesktopState* ds, const Montauk::KeyEvent& key) {
    // Global shortcuts (only on key press)
    if (key.pressed && key.ctrl && key.alt) {
        if (key.ascii == 't' || key.ascii == 'T') {
            open_terminal(ds);
            return;
        }
        if (key.ascii == 'f' || key.ascii == 'F') {
            open_filemanager(ds);
            return;
        }
        if (key.ascii == 'i' || key.ascii == 'I') {
            open_sysinfo(ds);
            return;
        }
        if (key.ascii == 'c' || key.ascii == 'C') {
            open_calculator(ds);
            return;
        }
        if (key.ascii == 'e' || key.ascii == 'E') {
            open_texteditor(ds);
            return;
        }
        if (key.ascii == 'w' || key.ascii == 'W') {
            open_wordprocessor(ds);
            return;
        }
        if (key.ascii == 'k' || key.ascii == 'K') {
            open_klog(ds);
            return;
        }
        if (key.ascii == 'd' || key.ascii == 'D') {
            open_doom(ds);
            return;
        }
    }

    // Dispatch to focused window
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        Window* win = &ds->windows[ds->focused_window];
        if (win->external) {
            // Forward key event to external window via syscall
            Montauk::WinEvent ev;
            montauk::memset(&ev, 0, sizeof(ev));
            ev.type = 0; // key
            ev.key = key;
            montauk::win_sendevent(win->ext_win_id, &ev);
        } else if (win->on_key) {
            win->on_key(win, key);
        }
    }
}
