/*
    * compose.cpp
    * Desktop composition: background, windows, overlays, cursor
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

// ============================================================================
// Lock Screen Drawing
// ============================================================================

static constexpr Color LOCK_CARD_BG   = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color LOCK_FIELD_BG  = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color LOCK_BORDER    = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color LOCK_ACCENT    = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color LOCK_TEXT      = Color::from_rgb(0x33, 0x33, 0x33);
static constexpr Color LOCK_DIM       = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color LOCK_ERROR     = Color::from_rgb(0xE0, 0x40, 0x40);
static constexpr Color LOCK_BTN_TEXT  = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr int   LOCK_CARD_W    = 360;
static constexpr int   LOCK_FIELD_H   = 36;
static constexpr int   LOCK_BTN_H     = 40;

void desktop_draw_lock_screen(DesktopState* ds) {
    Framebuffer& fb = ds->fb;
    int sw = ds->screen_w;
    int sh = ds->screen_h;
    int sfh = system_font_height();

    // Dark overlay on top of background
    fb.fill_rect_alpha(0, 0, sw, sh, Color::from_rgba(0, 0, 0, 0x80));

    // Clock display above card
    Montauk::DateTime dt;
    montauk::gettime(&dt);
    char time_str[12];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", (int)dt.Hour, (int)dt.Minute);

    // Calculate card dimensions
    int error_h = ds->lock_show_error ? sfh + 8 : 0;
    int card_h = 20 + sfh + 16 + sfh + 12 + sfh + 4 + LOCK_FIELD_H + 16 + LOCK_BTN_H + error_h + 20;
    int card_x = (sw - LOCK_CARD_W) / 2;
    int card_y = (sh - card_h) / 2;

    // Draw time above card
    int time_w = text_width(time_str);
    draw_text(fb, (sw - time_w) / 2, card_y - sfh * 3, time_str, Color::from_rgb(0xFF, 0xFF, 0xFF));

    // Date below time
    {
        int month_idx = dt.Month > 0 && dt.Month <= 12 ? dt.Month - 1 : 0;
        char date_str[32];
        snprintf(date_str, sizeof(date_str), "%s %d", month_names[month_idx], (int)dt.Day);
        int dw = text_width(date_str);
        draw_text(fb, (sw - dw) / 2, card_y - sfh * 2 + 4, date_str, Color::from_rgb(0xCC, 0xCC, 0xCC));
    }

    // Card background with rounded corners
    fill_rounded_rect(fb, card_x, card_y, LOCK_CARD_W, card_h, 12, LOCK_CARD_BG);

    int x = card_x + 24;
    int content_w = LOCK_CARD_W - 48;
    int y = card_y + 20;

    // Title (with optional lock icon beside it)
    {
        const char* title = "Locked";
        int tw = text_width(title);
        int icon_w = ds->icon_lock.pixels ? 20 : 0;
        int gap = icon_w ? 8 : 0;
        int total = icon_w + gap + tw;
        int tx = card_x + (LOCK_CARD_W - total) / 2;

        if (ds->icon_lock.pixels) {
            fb.blit_alpha(tx, y + (sfh - 20) / 2, ds->icon_lock.width, ds->icon_lock.height, ds->icon_lock.pixels);
        }
        draw_text(fb, tx + icon_w + gap, y, title, LOCK_TEXT);
        y += sfh + 16;
    }

    // Username display (cached at lock time)
    {
        int nw = text_width(ds->lock_display_name);
        draw_text(fb, card_x + (LOCK_CARD_W - nw) / 2, y, ds->lock_display_name, LOCK_DIM);
        y += sfh + 12;
    }

    // Password label
    draw_text(fb, x, y, "Password", LOCK_DIM);
    y += sfh + 4;

    // Password field
    {
        fb.fill_rect(x, y, content_w, LOCK_FIELD_H, LOCK_FIELD_BG);

        // 2px accent border (always active)
        for (int i = 0; i < content_w; i++) { fb.put_pixel(x + i, y, LOCK_ACCENT); fb.put_pixel(x + i, y + 1, LOCK_ACCENT); }
        for (int i = 0; i < content_w; i++) { fb.put_pixel(x + i, y + LOCK_FIELD_H - 1, LOCK_ACCENT); fb.put_pixel(x + i, y + LOCK_FIELD_H - 2, LOCK_ACCENT); }
        for (int i = 0; i < LOCK_FIELD_H; i++) { fb.put_pixel(x, y + i, LOCK_ACCENT); fb.put_pixel(x + 1, y + i, LOCK_ACCENT); }
        for (int i = 0; i < LOCK_FIELD_H; i++) { fb.put_pixel(x + content_w - 1, y + i, LOCK_ACCENT); fb.put_pixel(x + content_w - 2, y + i, LOCK_ACCENT); }

        // Masked password text (clipped to field width)
        char masked[65];
        int len = ds->lock_password_len;
        if (len > 64) len = 64;
        int max_text_w = content_w - 10 - 10 - 2; // left pad, right pad, cursor
        // Show only the trailing portion that fits
        int vis = len;
        {
            char tmp[65];
            for (int i = 0; i < len; i++) tmp[i] = '*';
            tmp[len] = '\0';
            while (vis > 0 && text_width(tmp + (len - vis)) > max_text_w)
                vis--;
        }
        for (int i = 0; i < vis; i++) masked[i] = '*';
        masked[vis] = '\0';

        int ty = y + (LOCK_FIELD_H - sfh) / 2;
        draw_text(fb, x + 10, ty, masked, LOCK_TEXT);

        // Cursor
        int cx = x + 10 + text_width(masked);
        fb.fill_rect(cx, ty, 2, sfh, LOCK_ACCENT);

        y += LOCK_FIELD_H + 16;
    }

    // Unlock button
    {
        fill_rounded_rect(fb, x, y, content_w, LOCK_BTN_H, 6, LOCK_ACCENT);
        const char* label = "Unlock";
        int tw = text_width(label);
        int ty = y + (LOCK_BTN_H - sfh) / 2;
        draw_text(fb, x + (content_w - tw) / 2, ty, label, LOCK_BTN_TEXT);
        y += LOCK_BTN_H;
    }

    // Error message
    if (ds->lock_show_error) {
        y += 8;
        int ew = text_width(ds->lock_error);
        draw_text(fb, card_x + (LOCK_CARD_W - ew) / 2, y, ds->lock_error, LOCK_ERROR);
    }
}

// ============================================================================
// Desktop Composition
// ============================================================================

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

    // Lock screen: draw overlay and card, then cursor, and return early
    if (ds->screen_locked) {
        desktop_draw_lock_screen(ds);
        draw_cursor(fb, ds->mouse.x, ds->mouse.y);
        return;
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

    // Draw volume popup if open
    if (ds->vol_popup_open) {
        desktop_draw_vol_popup(ds);
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
