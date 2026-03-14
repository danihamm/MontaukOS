/*
    * input.cpp
    * Mouse and keyboard event handling
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"
#include <montauk/user.h>

// ============================================================================
// Lock Screen Input
// ============================================================================

static void lock_screen(DesktopState* ds) {
    ds->screen_locked = true;
    ds->lock_password[0] = '\0';
    ds->lock_password_len = 0;
    ds->lock_error[0] = '\0';
    ds->lock_show_error = false;
    ds->app_menu_open = false;
    ds->ctx_menu_open = false;
    ds->net_popup_open = false;
    ds->vol_popup_open = false;

    // Cache display name for lock screen rendering
    montauk::strncpy(ds->lock_display_name, ds->current_user, sizeof(ds->lock_display_name));
    montauk::user::UserInfo users[16];
    int count = montauk::user::load_users(users, 16);
    for (int i = 0; i < count; i++) {
        if (montauk::streq(users[i].username, ds->current_user)) {
            if (users[i].display_name[0])
                montauk::strncpy(ds->lock_display_name, users[i].display_name, sizeof(ds->lock_display_name));
            break;
        }
    }
}

static bool try_unlock(DesktopState* ds) {
    ds->lock_show_error = false;

    if (montauk::user::authenticate(ds->current_user, ds->lock_password)) {
        ds->screen_locked = false;
        ds->lock_password[0] = '\0';
        ds->lock_password_len = 0;
        return true;
    }

    montauk::strcpy(ds->lock_error, "Incorrect password");
    ds->lock_show_error = true;
    ds->lock_password[0] = '\0';
    ds->lock_password_len = 0;
    return false;
}

static void handle_lock_mouse(DesktopState* ds) {
    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    uint8_t buttons = ds->mouse.buttons;
    uint8_t prev = ds->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);

    if (!left_pressed) return;

    int sfh = system_font_height();
    int card_w = 360;
    int content_w = card_w - 48;
    int field_h = 36;
    int btn_h = 40;

    // Calculate card layout to find button position
    int error_h = ds->lock_show_error ? sfh + 8 : 0;
    int card_h = 20 + sfh + 16 + sfh + 12 + sfh + 4 + field_h + 16 + btn_h + error_h + 20;
    int card_x = (ds->screen_w - card_w) / 2;
    int card_y = (ds->screen_h - card_h) / 2;
    int x = card_x + 24;

    // Walk through the layout to find the Unlock button y position
    int y = card_y + 20;
    y += sfh + 16;  // title
    y += sfh + 12;  // username
    y += sfh + 4;   // "Password" label
    y += field_h + 16; // field + gap

    // Check Unlock button
    if (mx >= x && mx < x + content_w && my >= y && my < y + btn_h) {
        try_unlock(ds);
        return;
    }

    // Check password field click (just keep focus, nothing else to switch to)
    int field_y = y - field_h - 16;
    if (mx >= x && mx < x + content_w && my >= field_y && my < field_y + field_h) {
        ds->lock_show_error = false;
    }
}

static void handle_lock_keyboard(DesktopState* ds, const Montauk::KeyEvent& key) {
    if (!key.pressed) return;

    // Enter submits
    if (key.ascii == '\n' || key.ascii == '\r') {
        try_unlock(ds);
        return;
    }

    // Backspace
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        if (ds->lock_password_len > 0) {
            ds->lock_password_len--;
            ds->lock_password[ds->lock_password_len] = '\0';
        }
        return;
    }

    // Printable characters
    if (key.ascii >= 0x20 && key.ascii < 0x7F) {
        if (ds->lock_password_len < 63) {
            ds->lock_password[ds->lock_password_len] = key.ascii;
            ds->lock_password_len++;
            ds->lock_password[ds->lock_password_len] = '\0';
        }
    }
}

// ============================================================================
// Desktop Mouse Handling
// ============================================================================

void gui::desktop_handle_mouse(DesktopState* ds) {
    if (ds->screen_locked) {
        handle_lock_mouse(ds);
        return;
    }

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
            for (int i = 0; i < menu_row_count; i++) {
                const MenuRow& row = menu_rows[i];
                if (row.is_category) cur_cat++;
                if (!menu_row_visible(i)) continue;
                int row_h = menu_row_height(row);
                if (my >= iy && my < iy + row_h) {
                    if (row.is_category && row.label[0] && cur_cat >= 0 && cur_cat < MENU_NUM_CATS - 1) {
                        // Toggle category expand/collapse
                        menu_cat_expanded[cur_cat] = !menu_cat_expanded[cur_cat];
                    } else if (!row.is_category) {
                        if (row.external) {
                            // Launch external app with user's home dir
                            montauk::spawn(row.binary_path, ds->home_dir);
                        } else {
                            // Dispatch embedded app
                            switch (row.app_id) {
                            case 0:  open_terminal(ds); break;
                            case 1:  open_filemanager(ds); break;
                            case 2:  open_sysinfo(ds); break;
                            case 3:  open_calculator(ds); break;
                            case 4:  open_texteditor(ds); break;
                            case 5:  open_klog(ds); break;
                            case 6:  open_procmgr(ds); break;
                            case 7:  open_mandelbrot(ds); break;
                            case 11: open_settings(ds); break;
                            case 12: open_reboot_dialog(ds); break;
                            case 14: open_shutdown_dialog(ds); break;
                            case 15: open_wordprocessor(ds); break;
                            case 16: montauk::exit(0); break;  // Log Out
                            case 17: lock_screen(ds); break;   // Lock Screen
                            }
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

    // Handle volume popup interaction
    if (ds->vol_popup_open) {
        int popup_x = ds->vol_icon_rect.x + ds->vol_icon_rect.w - 200;
        int popup_y = PANEL_HEIGHT + 2;
        if (popup_x < 4) popup_x = 4;
        Rect vol_rect = {popup_x, popup_y, 200, 120};

        // Handle drag continuity
        if (ds->vol_dragging) {
            if (left_held) {
                int slider_abs_x = popup_x + 16;
                int v = ((mx - slider_abs_x) * 100) / (200 - 32);
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                ds->vol_muted = false;
                ds->vol_level = v;
                montauk::audio_set_volume(0, v);
                return;
            }
            if (left_released) {
                ds->vol_dragging = false;
            }
        }

        if (left_pressed) {
            if (vol_rect.contains(mx, my)) {
                // Slider area (y = popup_y + 56, height = 8, with generous hit zone)
                int slider_abs_x = popup_x + 16;
                int slider_abs_y = popup_y + 56;
                int slider_w = 200 - 32;
                if (my >= slider_abs_y - 10 && my <= slider_abs_y + 8 + 10 &&
                    mx >= slider_abs_x - 8 && mx <= slider_abs_x + slider_w + 8) {
                    int v = ((mx - slider_abs_x) * 100) / slider_w;
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    ds->vol_muted = false;
                    ds->vol_level = v;
                    montauk::audio_set_volume(0, v);
                    ds->vol_dragging = true;
                    return;
                }

                // Buttons (y = popup_y + 78, h = 24)
                int btn_h = 24;
                int btn_y = popup_y + 78;
                int minus_w = 36, plus_w = 36, mute_w = 50, gap = 8;
                int total_w = minus_w + plus_w + mute_w + gap * 2;
                int bx = popup_x + (200 - total_w) / 2;

                if (my >= btn_y && my < btn_y + btn_h) {
                    // [-]
                    if (mx >= bx && mx < bx + minus_w) {
                        ds->vol_muted = false;
                        int v = ds->vol_level - 5;
                        if (v < 0) v = 0;
                        ds->vol_level = v;
                        montauk::audio_set_volume(0, v);
                        return;
                    }
                    bx += minus_w + gap;
                    // [+]
                    if (mx >= bx && mx < bx + plus_w) {
                        ds->vol_muted = false;
                        int v = ds->vol_level + 5;
                        if (v > 100) v = 100;
                        ds->vol_level = v;
                        montauk::audio_set_volume(0, v);
                        return;
                    }
                    bx += plus_w + gap;
                    // [Mute]
                    if (mx >= bx && mx < bx + mute_w) {
                        if (ds->vol_muted) {
                            ds->vol_muted = false;
                            montauk::audio_set_volume(0, ds->vol_pre_mute);
                            ds->vol_level = ds->vol_pre_mute;
                        } else {
                            ds->vol_pre_mute = ds->vol_level;
                            ds->vol_muted = true;
                            montauk::audio_set_volume(0, 0);
                        }
                        return;
                    }
                }
                return; // click inside popup but not on any control
            } else if (!ds->vol_icon_rect.contains(mx, my)) {
                ds->vol_popup_open = false;
                ds->vol_dragging = false;
            }
        }
    }

    // Handle net popup clicks
    if (ds->net_popup_open && left_pressed) {
        int popup_w = 220;
        int fh_net = system_font_height();
        int row_h_net = fh_net + 8;
        int popup_h = (row_h_net + 8) + row_h_net * 5 + 12;
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
            ds->vol_popup_open = false;
            ds->ctx_menu_open = false;
            return;
        }

        // Volume icon
        if (ds->vol_icon_rect.w > 0 && ds->vol_icon_rect.contains(mx, my)) {
            ds->vol_popup_open = !ds->vol_popup_open;
            ds->vol_dragging = false;
            ds->app_menu_open = false;
            ds->net_popup_open = false;
            ds->ctx_menu_open = false;
            return;
        }

        // Network icon
        if (ds->net_icon_rect.w > 0 && ds->net_icon_rect.contains(mx, my)) {
            ds->net_popup_open = !ds->net_popup_open;
            ds->app_menu_open = false;
            ds->vol_popup_open = false;
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
        ds->vol_popup_open = false;
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
            ds->vol_popup_open = false;
        }
    }
}

void gui::desktop_handle_keyboard(DesktopState* ds, const Montauk::KeyEvent& key) {
    if (ds->screen_locked) {
        handle_lock_keyboard(ds, key);
        return;
    }

    // Global shortcuts (only on key press)
    if (key.pressed && key.ctrl && key.alt) {
        if (key.ascii == 'l' || key.ascii == 'L') {
            lock_screen(ds);
            return;
        }
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
