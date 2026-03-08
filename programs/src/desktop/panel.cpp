/*
    * panel.cpp
    * Panel bar, app menu, and network popup drawing
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

void gui::desktop_draw_panel(DesktopState* ds) {
    Framebuffer& fb = ds->fb;
    int sw = ds->screen_w;

    // Panel gradient background (slightly lighter at top)
    Color pc = ds->settings.panel_color;
    for (int y = 0; y < PANEL_HEIGHT; y++) {
        int t = y * 255 / PANEL_HEIGHT;
        uint8_t r = pc.r + (10 - t * 10 / 255);
        uint8_t g = pc.g + (10 - t * 10 / 255);
        uint8_t b = pc.b + (10 - t * 10 / 255);
        fb.fill_rect(0, y, sw, 1, Color::from_rgb(r, g, b));
    }

    // Bottom highlight line (skip when wallpaper is set — the white bleeds through)
    if (!ds->settings.bg_image) {
        fb.fill_rect(0, PANEL_HEIGHT - 1, sw, 1, Color::from_rgba(0xFF, 0xFF, 0xFF, 0x10));
    }

    // App menu button (left side)
    int btn_x = 4;
    int btn_y = 2;
    int btn_w = 28;
    int btn_h = 28;

    if (ds->icon_appmenu.pixels) {
        int ix = btn_x + (btn_w - ds->icon_appmenu.width) / 2;
        int iy = btn_y + (btn_h - ds->icon_appmenu.height) / 2;
        fb.blit_alpha(ix, iy, ds->icon_appmenu.width, ds->icon_appmenu.height, ds->icon_appmenu.pixels);
    } else {
        for (int gr = 0; gr < 3; gr++) {
            for (int gc = 0; gc < 3; gc++) {
                int dx = btn_x + 6 + gc * 6;
                int dy = btn_y + 6 + gr * 6;
                fb.fill_rect(dx, dy, 3, 3, colors::PANEL_TEXT);
            }
        }
    }

    // Window indicator pills (center area)
    int indicator_x = 40;
    for (int i = 0; i < ds->window_count; i++) {
        Window* win = &ds->windows[i];
        if (win->state == WIN_CLOSED) continue;

        int tw = text_width(win->title);
        int pad = 12;
        int iw = tw + pad * 2;
        if (iw > 150) iw = 150;

        // Pre-blended indicator pill colors (opaque, blended against PANEL_BG)
        Color pill_bg = (i == ds->focused_window)
            ? colors::PANEL_INDICATOR_ACTIVE
            : colors::PANEL_INDICATOR_INACTIVE;

        fill_rounded_rect(fb, indicator_x, 4, iw, 24, 6, pill_bg);

        // Active window accent underline bar
        if (i == ds->focused_window) {
            fb.fill_rect(indicator_x + 4, 26, iw - 8, 2, ds->settings.accent_color);
        }

        // Truncate title if too long
        char short_title[20];
        montauk::strncpy(short_title, win->title, 18);

        int tx = indicator_x + pad;
        int ty = 4 + (24 - system_font_height()) / 2;
        draw_text(fb, tx, ty, short_title, colors::PANEL_TEXT);

        indicator_x += iw + 4;
    }

    // Date + Clock (right side)
    Montauk::DateTime dt;
    montauk::gettime(&dt);

    char clock_str[12];
    if (ds->settings.clock_24h) {
        snprintf(clock_str, sizeof(clock_str), "%02d:%02d", (int)dt.Hour, (int)dt.Minute);
    } else {
        int h12 = (int)dt.Hour % 12;
        if (h12 == 0) h12 = 12;
        const char* ampm = dt.Hour < 12 ? "AM" : "PM";
        snprintf(clock_str, sizeof(clock_str), "%d:%02d %s", h12, (int)dt.Minute, ampm);
    }
    int clock_w = text_width(clock_str);
    int clock_x = sw - clock_w - 12;
    int clock_y = (PANEL_HEIGHT - system_font_height()) / 2;
    draw_text(fb, clock_x, clock_y, clock_str, colors::PANEL_TEXT);

    // Date before clock
    char date_str[12];
    int month_idx = dt.Month > 0 && dt.Month <= 12 ? dt.Month - 1 : 0;
    snprintf(date_str, sizeof(date_str), "%s %d", month_names[month_idx], (int)dt.Day);
    int date_w = text_width(date_str);
    int date_x = clock_x - date_w - 10;
    draw_text(fb, date_x, clock_y, date_str, colors::PANEL_TEXT);

    // Network icon (to the left of the date)
    uint64_t now = montauk::get_milliseconds();
    if (now - ds->net_cfg_last_poll > 5000) {
        montauk::get_netcfg(&ds->cached_net_cfg);
        ds->net_cfg_last_poll = now;
    }

    int net_icon_x = date_x - 16 - 12;
    int net_icon_y = (PANEL_HEIGHT - 16) / 2;
    ds->net_icon_rect = {net_icon_x, net_icon_y, 16, 16};

    if (ds->icon_network.pixels) {
        if (ds->cached_net_cfg.ipAddress == 0) {
            uint32_t* src = ds->icon_network.pixels;
            int npx = 16 * 16;
            uint32_t tinted[256];
            for (int p = 0; p < npx; p++) {
                uint32_t px = src[p];
                uint8_t a = (px >> 24) & 0xFF;
                tinted[p] = ((uint32_t)a << 24) | 0x004444CC;
            }
            fb.blit_alpha(net_icon_x, net_icon_y, 16, 16, tinted);
        } else {
            fb.blit_alpha(net_icon_x, net_icon_y, ds->icon_network.width, ds->icon_network.height, ds->icon_network.pixels);
        }
    }
}

// ============================================================================
// App Menu
// ============================================================================

void desktop_draw_app_menu(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    int menu_x = 4;
    int menu_y = PANEL_HEIGHT + 2;
    int menu_h = menu_total_height();

    // Menu shadow
    draw_shadow(fb, menu_x, menu_y, MENU_W, menu_h, 4, colors::SHADOW);

    // Menu background with rounded corners
    fill_rounded_rect(fb, menu_x, menu_y, MENU_W, menu_h, 8, colors::MENU_BG);
    draw_rect(fb, menu_x, menu_y, MENU_W, menu_h, colors::BORDER);

    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    int iy = menu_y + 5;
    int cur_cat = -1;

    for (int i = 0; i < menu_row_count; i++) {
        const MenuRow& row = menu_rows[i];

        if (row.is_category) cur_cat++;
        if (!menu_row_visible(i)) continue;

        int row_h = menu_row_height(row);

        if (row.is_category) {
            // Separator line above (except first category)
            if (i > 0) {
                for (int sx = menu_x + 8; sx < menu_x + MENU_W - 8; sx++)
                    fb.put_pixel(sx, iy + 1, colors::BORDER);
            }

            // Category header with hover + expand/collapse indicator
            if (row.label[0]) {
                Rect cat_rect = {menu_x + 4, iy, MENU_W - 8, row_h};
                bool hovered = cat_rect.contains(mx, my);
                if (hovered)
                    fill_rounded_rect(fb, cat_rect.x, cat_rect.y, cat_rect.w, cat_rect.h, 4,
                                      Color::from_rgb(0xE8, 0xE8, 0xE8));

                // Arrow indicator: > for collapsed, v for expanded
                bool expanded = (cur_cat >= 0 && cur_cat < MENU_NUM_CATS) && menu_cat_expanded[cur_cat];
                int ax = menu_x + 10;
                int ay = iy + row_h / 2;
                Color arrow_color = Color::from_rgb(0x88, 0x88, 0x88);
                if (expanded) {
                    // Down arrow (v shape)
                    for (int d = 0; d < 4; d++) {
                        fb.put_pixel(ax + d, ay - 2 + d, arrow_color);
                        fb.put_pixel(ax + d + 1, ay - 2 + d, arrow_color);
                        fb.put_pixel(ax + 7 - d, ay - 2 + d, arrow_color);
                        fb.put_pixel(ax + 6 - d, ay - 2 + d, arrow_color);
                    }
                } else {
                    // Right arrow (> shape)
                    for (int d = 0; d < 4; d++) {
                        fb.put_pixel(ax + d, ay - 3 + d, arrow_color);
                        fb.put_pixel(ax + d, ay - 3 + d + 1, arrow_color);
                        fb.put_pixel(ax + d, ay + 3 - d, arrow_color);
                        fb.put_pixel(ax + d, ay + 3 - d - 1, arrow_color);
                    }
                }

                int tx = menu_x + 24;
                int ty = iy + (row_h - system_font_height()) / 2;
                draw_text(fb, tx, ty, row.label, Color::from_rgb(0x66, 0x66, 0x66));
            }
        } else {
            Rect item_rect = {menu_x + 4, iy, MENU_W - 8, row_h};

            // Hover highlight
            if (item_rect.contains(mx, my)) {
                fill_rounded_rect(fb, item_rect.x, item_rect.y, item_rect.w, item_rect.h, 4, colors::MENU_HOVER);
            }

            // Icon
            int icon_x = item_rect.x + 8;
            int icon_y = item_rect.y + (row_h - 20) / 2;
            if (row.icon && row.icon->pixels) {
                fb.blit_alpha(icon_x, icon_y, row.icon->width, row.icon->height, row.icon->pixels);
            }

            // Label
            int tx = icon_x + 28;
            int ty = item_rect.y + (row_h - system_font_height()) / 2;
            draw_text(fb, tx, ty, row.label, colors::TEXT_COLOR);
        }

        iy += row_h;
    }
}

// ============================================================================
// Network Popup
// ============================================================================

void desktop_draw_net_popup(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    int popup_w = 220;
    int popup_h = 130;
    int popup_x = ds->net_icon_rect.x + ds->net_icon_rect.w - popup_w;
    int popup_y = PANEL_HEIGHT + 2;
    if (popup_x < 4) popup_x = 4;

    draw_shadow(fb, popup_x, popup_y, popup_w, popup_h, 4, colors::SHADOW);
    fb.fill_rect(popup_x, popup_y, popup_w, popup_h, colors::MENU_BG);
    draw_rect(fb, popup_x, popup_y, popup_w, popup_h, colors::BORDER);

    int tx = popup_x + 12;
    int ty = popup_y + 10;
    int line_h = system_font_height() + 6;
    char line[64];

    Montauk::NetCfg& nc = ds->cached_net_cfg;

    if (nc.ipAddress != 0) {
        char ipbuf[20];
        format_ip(ipbuf, nc.ipAddress);
        snprintf(line, sizeof(line), "IP:      %s", ipbuf);
    } else {
        snprintf(line, sizeof(line), "IP:      Not connected");
    }
    draw_text(fb, tx, ty, line, colors::TEXT_COLOR);
    ty += line_h;

    char buf[20];
    format_ip(buf, nc.subnetMask);
    snprintf(line, sizeof(line), "Subnet:  %s", buf);
    draw_text(fb, tx, ty, line, colors::TEXT_COLOR);
    ty += line_h;

    format_ip(buf, nc.gateway);
    snprintf(line, sizeof(line), "Gateway: %s", buf);
    draw_text(fb, tx, ty, line, colors::TEXT_COLOR);
    ty += line_h;

    format_ip(buf, nc.dnsServer);
    snprintf(line, sizeof(line), "DNS:     %s", buf);
    draw_text(fb, tx, ty, line, colors::TEXT_COLOR);
    ty += line_h;

    format_mac(buf, nc.macAddress);
    snprintf(line, sizeof(line), "MAC:     %s", buf);
    draw_text(fb, tx, ty, line, colors::TEXT_COLOR);
}
