/*
    * compose.cpp
    * Desktop composition: background, windows, overlays, cursor
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

void gui::desktop_compose(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    // Desktop background gradient
    int sw = ds->screen_w;
    int sh = ds->screen_h;
    int grad_start = PANEL_HEIGHT;
    int grad_range = sh - grad_start;
    if (grad_range < 1) grad_range = 1;

    uint32_t* buf = fb.buffer();
    int pitch = fb.pitch();

    for (int y = 0; y < sh; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)buf + y * pitch);
        if (ds->settings.bg_image && ds->settings.bg_wallpaper) {
            // Wallpaper covers the entire screen; panel draws on top
            uint32_t* wp = ds->settings.bg_wallpaper + y * ds->settings.bg_wallpaper_w;
            int cw = sw < ds->settings.bg_wallpaper_w ? sw : ds->settings.bg_wallpaper_w;
            for (int x = 0; x < cw; x++) row[x] = wp[x];
        } else if (y < grad_start) {
            // Panel area - will be overwritten by panel drawing
            uint32_t px = ds->settings.panel_color.to_pixel();
            for (int x = 0; x < sw; x++) row[x] = px;
        } else if (ds->settings.bg_gradient) {
            int t = y - grad_start;
            Color top = ds->settings.bg_grad_top;
            Color bot = ds->settings.bg_grad_bottom;
            uint8_t r = top.r - (top.r - bot.r) * t / grad_range;
            uint8_t g = top.g - (top.g - bot.g) * t / grad_range;
            uint8_t b = top.b - (top.b - bot.b) * t / grad_range;
            uint32_t px = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            for (int x = 0; x < sw; x++) row[x] = px;
        } else {
            uint32_t px = ds->settings.bg_solid.to_pixel();
            for (int x = 0; x < sw; x++) row[x] = px;
        }
    }

    // Draw windows from bottom to top
    for (int i = 0; i < ds->window_count; i++) {
        if (ds->windows[i].state != WIN_MINIMIZED && ds->windows[i].state != WIN_CLOSED) {
            desktop_draw_window(ds, i);
        }
    }

    // Draw panel on top
    desktop_draw_panel(ds);

    // Draw app menu if open
    if (ds->app_menu_open) {
        desktop_draw_app_menu(ds);
    }

    // Draw network popup if open
    if (ds->net_popup_open) {
        desktop_draw_net_popup(ds);
    }

    // Draw right-click context menu if open
    if (ds->ctx_menu_open) {
        static constexpr int CTX_MENU_W = 180;
        static constexpr int CTX_ITEM_H = 36;
        static constexpr int CTX_ITEM_COUNT = 5;
        int cmx = ds->ctx_menu_x;
        int cmy = ds->ctx_menu_y;
        int cmh = CTX_ITEM_H * CTX_ITEM_COUNT + 8;

        // Clamp to screen
        if (cmx + CTX_MENU_W > sw) cmx = sw - CTX_MENU_W;
        if (cmy + cmh > sh) cmy = sh - cmh;

        draw_shadow(fb, cmx, cmy, CTX_MENU_W, cmh, 4, colors::SHADOW);
        fill_rounded_rect(fb, cmx, cmy, CTX_MENU_W, cmh, 8, colors::MENU_BG);
        draw_rect(fb, cmx, cmy, CTX_MENU_W, cmh, colors::BORDER);

        struct CtxItem { const char* label; SvgIcon* icon; };
        CtxItem ctx_items[CTX_ITEM_COUNT] = {
            { "Terminal", &ds->icon_terminal },
            { "Files",    &ds->icon_filemanager },
            { "About",    &ds->icon_settings },
            { "Reboot",   &ds->icon_reboot },
            { "Shutdown", &ds->icon_shutdown },
        };

        int mmx = ds->mouse.x;
        int mmy = ds->mouse.y;

        for (int i = 0; i < CTX_ITEM_COUNT; i++) {
            int iy = cmy + 4 + i * CTX_ITEM_H;
            Rect item_r = {cmx + 4, iy, CTX_MENU_W - 8, CTX_ITEM_H};

            if (item_r.contains(mmx, mmy)) {
                fill_rounded_rect(fb, item_r.x, item_r.y, item_r.w, item_r.h, 4, colors::MENU_HOVER);
            }

            int icon_x = item_r.x + 8;
            int icon_y = item_r.y + (CTX_ITEM_H - 20) / 2;
            if (ctx_items[i].icon && ctx_items[i].icon->pixels) {
                fb.blit_alpha(icon_x, icon_y, ctx_items[i].icon->width, ctx_items[i].icon->height, ctx_items[i].icon->pixels);
            }

            int tx = icon_x + 28;
            int ty = item_r.y + (CTX_ITEM_H - system_font_height()) / 2;
            draw_text(fb, tx, ty, ctx_items[i].label, colors::TEXT_COLOR);
        }
    }

    // Draw snap preview overlay while dragging to screen edge
    for (int i = 0; i < ds->window_count; i++) {
        Window* win = &ds->windows[i];
        if (win->dragging) {
            int dmx = ds->mouse.x;
            if (dmx <= 0) {
                fb.fill_rect_alpha(0, PANEL_HEIGHT, sw / 2, sh - PANEL_HEIGHT,
                    Color::from_rgba(0x33, 0x77, 0xCC, 0x30));
            } else if (dmx >= sw - 1) {
                fb.fill_rect_alpha(sw / 2, PANEL_HEIGHT, sw / 2, sh - PANEL_HEIGHT,
                    Color::from_rgba(0x33, 0x77, 0xCC, 0x30));
            }
            break;
        }
    }

    // Determine cursor style based on resize hover or active resize
    CursorStyle cur_style = CURSOR_ARROW;
    for (int i = ds->window_count - 1; i >= 0; i--) {
        Window* win = &ds->windows[i];
        if (win->resizing) {
            cur_style = cursor_for_edge(win->resize_edge);
            break;
        }
        if (win->state == WIN_MINIMIZED || win->state == WIN_CLOSED || win->state == WIN_MAXIMIZED)
            continue;
        if (win->frame.contains(ds->mouse.x, ds->mouse.y)) {
            ResizeEdge edge = hit_test_resize_edge(win->frame, ds->mouse.x, ds->mouse.y);
            if (edge != RESIZE_NONE) {
                cur_style = cursor_for_edge(edge);
            }
            break;
        }
    }

    // Check if focused external window requests a cursor style
    if (cur_style == CURSOR_ARROW && ds->focused_window >= 0) {
        Window* fwin = &ds->windows[ds->focused_window];
        if (fwin->external && fwin->ext_cursor > 0) {
            Rect cr = fwin->content_rect();
            if (cr.contains(ds->mouse.x, ds->mouse.y)) {
                if (fwin->ext_cursor == 1) cur_style = CURSOR_RESIZE_H;
                else if (fwin->ext_cursor == 2) cur_style = CURSOR_RESIZE_V;
            }
        }
    }

    // Draw cursor last
    draw_cursor(fb, ds->mouse.x, ds->mouse.y, cur_style);
}
