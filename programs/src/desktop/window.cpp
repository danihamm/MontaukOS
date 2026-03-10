/*
    * window.cpp
    * Window management: create, close, raise, draw
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

int gui::desktop_create_window(DesktopState* ds, const char* title, int x, int y, int w, int h) {
    if (ds->window_count >= MAX_WINDOWS) return -1;

    int idx = ds->window_count;
    Window* win = &ds->windows[idx];
    montauk::memset(win, 0, sizeof(Window));

    montauk::strncpy(win->title, title, MAX_TITLE_LEN);
    win->frame = {x, y, w, h};
    win->state = WIN_NORMAL;
    win->z_order = idx;
    win->focused = true;
    win->dirty = true;
    win->dragging = false;
    win->resizing = false;
    win->saved_frame = win->frame;

    // Allocate content buffer
    Rect cr = win->content_rect();
    win->content_w = cr.w;
    win->content_h = cr.h;
    int buf_size = cr.w * cr.h * 4;
    win->content = (uint32_t*)montauk::alloc(buf_size);
    montauk::memset(win->content, 0xFF, buf_size);

    win->on_draw = nullptr;
    win->on_mouse = nullptr;
    win->on_key = nullptr;
    win->on_close = nullptr;
    win->on_poll = nullptr;
    win->app_data = nullptr;
    win->external = false;
    win->ext_win_id = -1;

    // Unfocus previous window
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        ds->windows[ds->focused_window].focused = false;
    }
    ds->focused_window = idx;
    ds->window_count++;

    return idx;
}

void gui::desktop_close_window(DesktopState* ds, int idx) {
    if (idx < 0 || idx >= ds->window_count) return;

    Window* win = &ds->windows[idx];

    // For external windows, send a close event instead of freeing the buffer.
    // Track the ID so the poll loop won't re-create it at the default position
    // before the owning process has finished destroying it.
    if (win->external) {
        if (ds->closing_ext_count < DesktopState::MAX_CLOSING) {
            ds->closing_ext_ids[ds->closing_ext_count++] = win->ext_win_id;
        }
        Montauk::WinEvent ev;
        montauk::memset(&ev, 0, sizeof(ev));
        ev.type = 3; // close
        montauk::win_sendevent(win->ext_win_id, &ev);
    }

    if (win->on_close) win->on_close(win);

    // Free content buffer (skip for external windows — shared memory)
    if (win->content && !win->external) {
        montauk::free(win->content);
        win->content = nullptr;
    }

    // Shift remaining windows down
    for (int i = idx; i < ds->window_count - 1; i++) {
        ds->windows[i] = ds->windows[i + 1];
    }
    ds->window_count--;

    // Fix focused window index
    if (ds->focused_window == idx) {
        ds->focused_window = ds->window_count > 0 ? ds->window_count - 1 : -1;
    } else if (ds->focused_window > idx) {
        ds->focused_window--;
    }

    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        ds->windows[ds->focused_window].focused = true;
    }
}

void gui::desktop_raise_window(DesktopState* ds, int idx) {
    if (idx < 0 || idx >= ds->window_count) return;
    if (idx == ds->window_count - 1) {
        // Already on top, just focus
        if (ds->focused_window >= 0 && ds->focused_window < ds->window_count)
            ds->windows[ds->focused_window].focused = false;
        ds->focused_window = idx;
        ds->windows[idx].focused = true;
        return;
    }

    // Unfocus current
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        ds->windows[ds->focused_window].focused = false;
    }

    // Move window to end (top of stack)
    Window tmp = ds->windows[idx];
    for (int i = idx; i < ds->window_count - 1; i++) {
        ds->windows[i] = ds->windows[i + 1];
    }
    ds->windows[ds->window_count - 1] = tmp;

    ds->focused_window = ds->window_count - 1;
    ds->windows[ds->focused_window].focused = true;
}

void gui::desktop_draw_window(DesktopState* ds, int idx) {
    if (idx < 0 || idx >= ds->window_count) return;
    Window* win = &ds->windows[idx];
    if (win->state == WIN_MINIMIZED || win->state == WIN_CLOSED) return;

    Framebuffer& fb = ds->fb;
    int x = win->frame.x;
    int y = win->frame.y;
    int w = win->frame.w;
    int h = win->frame.h;

    // Draw shadow
    if (ds->settings.show_shadows)
        draw_shadow(fb, x, y, w, h, SHADOW_SIZE, colors::SHADOW);

    // Draw window body
    fb.fill_rect(x, y, w, h, colors::WINDOW_BG);

    // Draw titlebar
    Color tb_bg = win->focused ? colors::TITLEBAR_BG : Color::from_rgb(0xE8, 0xE8, 0xE8);
    fb.fill_rect(x, y, w, TITLEBAR_HEIGHT, tb_bg);

    // Draw border
    draw_rect(fb, x, y, w, h, colors::BORDER);
    // Titlebar bottom separator
    draw_hline(fb, x, y + TITLEBAR_HEIGHT - 1, w, colors::BORDER);

    // Draw window buttons (macOS style: close, minimize, maximize)
    Rect close_r = win->close_btn_rect();
    Rect min_r = win->min_btn_rect();
    Rect max_r = win->max_btn_rect();

    fill_circle(fb, close_r.x + BTN_RADIUS, close_r.y + BTN_RADIUS, BTN_RADIUS, colors::CLOSE_BTN);
    fill_circle(fb, min_r.x + BTN_RADIUS, min_r.y + BTN_RADIUS, BTN_RADIUS, colors::MIN_BTN);
    fill_circle(fb, max_r.x + BTN_RADIUS, max_r.y + BTN_RADIUS, BTN_RADIUS, colors::MAX_BTN);

    // Draw title text centered in titlebar (after buttons)
    int title_x = x + 12 + 44 + BTN_RADIUS * 2 + 12; // after buttons
    int title_y = y + (TITLEBAR_HEIGHT - system_font_height()) / 2;
    int title_w = text_width(win->title);
    // Center in remaining space
    int remaining_w = w - (title_x - x) - 12;
    if (remaining_w > title_w) {
        title_x += (remaining_w - title_w) / 2;
    }
    draw_text(fb, title_x, title_y, win->title, colors::TEXT_COLOR);

    // Call app draw callback to render content (skip during resize — buffer is old size)
    if (win->on_draw && !win->resizing) {
        win->on_draw(win, fb);
    }

    // Blit content buffer to framebuffer (clip to actual buffer size during resize)
    Rect cr = win->content_rect();
    if (win->content) {
        if (win->external && (cr.w != win->content_w || cr.h != win->content_h)) {
            // Nearest-neighbor scale for external windows (fixed-size shared buffer)
            int src_w = win->content_w;
            int src_h = win->content_h;
            int dst_w = cr.w;
            int dst_h = cr.h;
            uint32_t* buf = fb.buffer();
            int pitch = fb.pitch();
            for (int y = 0; y < dst_h; y++) {
                int dy = cr.y + y;
                if (dy < 0 || dy >= fb.height()) continue;
                int sy = y * src_h / dst_h;
                uint32_t* dst_row = (uint32_t*)((uint8_t*)buf + dy * pitch);
                uint32_t* src_row = win->content + sy * src_w;
                for (int x = 0; x < dst_w; x++) {
                    int dx = cr.x + x;
                    if (dx < 0 || dx >= fb.width()) continue;
                    dst_row[dx] = src_row[x * src_w / dst_w];
                }
            }
        } else {
            int blit_w = cr.w < win->content_w ? cr.w : win->content_w;
            int blit_h = cr.h < win->content_h ? cr.h : win->content_h;
            fb.blit(cr.x, cr.y, blit_w, blit_h, win->content);
        }
    }
}

gui::ResizeEdge hit_test_resize_edge(const gui::Rect& f, int mx, int my) {
    using namespace gui;
    int G = RESIZE_GRAB;
    if (!f.contains(mx, my)) return RESIZE_NONE;

    bool near_left   = mx < f.x + G;
    bool near_right  = mx >= f.x + f.w - G;
    bool near_top    = my < f.y + G;
    bool near_bottom = my >= f.y + f.h - G;

    // Corners first
    if (near_top && near_left)     return RESIZE_TOP_LEFT;
    if (near_top && near_right)    return RESIZE_TOP_RIGHT;
    if (near_bottom && near_left)  return RESIZE_BOTTOM_LEFT;
    if (near_bottom && near_right) return RESIZE_BOTTOM_RIGHT;

    // Edges
    if (near_top)    return RESIZE_TOP;
    if (near_bottom) return RESIZE_BOTTOM;
    if (near_left)   return RESIZE_LEFT;
    if (near_right)  return RESIZE_RIGHT;

    return RESIZE_NONE;
}

gui::CursorStyle cursor_for_edge(gui::ResizeEdge edge) {
    using namespace gui;
    switch (edge) {
    case RESIZE_LEFT: case RESIZE_RIGHT:
        return CURSOR_RESIZE_H;
    case RESIZE_TOP: case RESIZE_BOTTOM:
        return CURSOR_RESIZE_V;
    case RESIZE_TOP_LEFT: case RESIZE_BOTTOM_RIGHT:
        return CURSOR_RESIZE_NWSE;
    case RESIZE_TOP_RIGHT: case RESIZE_BOTTOM_LEFT:
        return CURSOR_RESIZE_NESW;
    default:
        return CURSOR_ARROW;
    }
}
