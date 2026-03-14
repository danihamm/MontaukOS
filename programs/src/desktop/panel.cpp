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

    // Volume icon (to the left of the date)
    uint64_t now = montauk::get_milliseconds();
    if (now - ds->vol_last_poll > 5000) {
        int v = montauk::audio_get_volume(0);
        if (v >= 0 && !ds->vol_muted) ds->vol_level = v;
        ds->vol_last_poll = now;
    }

    int vol_icon_x = date_x - 16 - 12;
    int vol_icon_y = (PANEL_HEIGHT - 16) / 2;
    ds->vol_icon_rect = {vol_icon_x, vol_icon_y, 16, 16};

    if (ds->icon_volume.pixels) {
        if (ds->vol_muted) {
            // Tint red-ish when muted
            uint32_t* src = ds->icon_volume.pixels;
            int npx = 16 * 16;
            uint32_t tinted[256];
            for (int p = 0; p < npx; p++) {
                uint32_t px = src[p];
                uint8_t a = (px >> 24) & 0xFF;
                tinted[p] = ((uint32_t)a << 24) | 0x00CC3333;
            }
            fb.blit_alpha(vol_icon_x, vol_icon_y, 16, 16, tinted);
        } else {
            fb.blit_alpha(vol_icon_x, vol_icon_y, ds->icon_volume.width, ds->icon_volume.height, ds->icon_volume.pixels);
        }
    }

    // Network icon (to the left of the volume icon)
    if (now - ds->net_cfg_last_poll > 5000) {
        montauk::get_netcfg(&ds->cached_net_cfg);
        ds->net_cfg_last_poll = now;
    }

    int net_icon_x = vol_icon_x - 16 - 10;
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
    Montauk::NetCfg& nc = ds->cached_net_cfg;
    bool connected = nc.ipAddress != 0;

    int popup_w = 220;
    int fh = system_font_height();
    int row_h = fh + 8;
    int header_h = row_h + 8;       // title + status row + padding
    int body_rows = 5;               // IP, Subnet, Gateway, DNS, MAC
    int popup_h = header_h + row_h * body_rows + 12;
    int popup_x = ds->net_icon_rect.x + ds->net_icon_rect.w - popup_w;
    int popup_y = PANEL_HEIGHT + 2;
    if (popup_x < 4) popup_x = 4;

    draw_shadow(fb, popup_x, popup_y, popup_w, popup_h, 4, colors::SHADOW);
    fill_rounded_rect(fb, popup_x, popup_y, popup_w, popup_h, 8, colors::MENU_BG);
    draw_rect(fb, popup_x, popup_y, popup_w, popup_h, colors::BORDER);

    int lx = popup_x + 14;
    int ty = popup_y + 10;

    // Header: "Ethernet" + status dot
    draw_text(fb, lx, ty, "Ethernet", colors::TEXT_COLOR);

    // Status dot + label (right-aligned in header)
    Color dot_color = connected
        ? Color::from_rgb(0x4C, 0xAF, 0x50)   // green
        : Color::from_rgb(0xCC, 0x33, 0x33);   // red
    const char* status_str = connected ? "Connected" : "Disconnected";
    int sw = text_width(status_str);
    int dot_r = 4;
    int status_x = popup_x + popup_w - 14 - sw;
    int dot_x = status_x - dot_r * 2 - 5;
    int dot_cy = ty + fh / 2;
    for (int dy = -dot_r; dy <= dot_r; dy++)
        for (int dx = -dot_r; dx <= dot_r; dx++)
            if (dx * dx + dy * dy <= dot_r * dot_r)
                fb.put_pixel(dot_x + dot_r + dx, dot_cy + dy, dot_color);
    Color dim = Color::from_rgb(0x66, 0x66, 0x66);
    draw_text(fb, status_x, ty, status_str, dim);

    ty += row_h + 2;

    // Separator line
    for (int sx = popup_x + 10; sx < popup_x + popup_w - 10; sx++)
        fb.put_pixel(sx, ty, colors::BORDER);
    ty += 6;

    // Body rows: label (dim) + value (dark), two-column
    int val_x = popup_x + 76;  // fixed column for values

    struct NetRow { const char* label; char value[24]; };
    NetRow rows[5];

    rows[0].label = "IP";
    if (connected) format_ip(rows[0].value, nc.ipAddress);
    else montauk::strcpy(rows[0].value, "\xE2\x80\x94");  // em dash

    rows[1].label = "Subnet";
    format_ip(rows[1].value, nc.subnetMask);

    rows[2].label = "Gateway";
    format_ip(rows[2].value, nc.gateway);

    rows[3].label = "DNS";
    format_ip(rows[3].value, nc.dnsServer);

    rows[4].label = "MAC";
    format_mac(rows[4].value, nc.macAddress);

    for (int i = 0; i < body_rows; i++) {
        draw_text(fb, lx, ty, rows[i].label, dim);
        draw_text(fb, val_x, ty, rows[i].value, colors::TEXT_COLOR);
        ty += row_h;
    }
}

// ============================================================================
// Volume Popup
// ============================================================================

static constexpr int VOL_POPUP_W = 200;
static constexpr int VOL_POPUP_H = 120;
static constexpr int VOL_SLIDER_X = 16;
static constexpr int VOL_SLIDER_W = VOL_POPUP_W - 32;
static constexpr int VOL_SLIDER_H = 8;
static constexpr int VOL_KNOB_R   = 8;

void desktop_draw_vol_popup(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    int popup_x = ds->vol_icon_rect.x + ds->vol_icon_rect.w - VOL_POPUP_W;
    int popup_y = PANEL_HEIGHT + 2;
    if (popup_x < 4) popup_x = 4;

    draw_shadow(fb, popup_x, popup_y, VOL_POPUP_W, VOL_POPUP_H, 4, colors::SHADOW);
    fill_rounded_rect(fb, popup_x, popup_y, VOL_POPUP_W, VOL_POPUP_H, 8, colors::MENU_BG);
    draw_rect(fb, popup_x, popup_y, VOL_POPUP_W, VOL_POPUP_H, colors::BORDER);

    int display_vol = ds->vol_muted ? 0 : ds->vol_level;

    // Volume percentage label
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", display_vol);
    int vw = text_width(vol_str);
    Color vol_color = ds->vol_muted ? Color::from_rgb(0xCC, 0x33, 0x33) : ds->settings.accent_color;
    draw_text(fb, popup_x + (VOL_POPUP_W - vw) / 2, popup_y + 12, vol_str, vol_color);

    // "Muted" sub-label
    if (ds->vol_muted) {
        const char* ml = "Muted";
        int mw = text_width(ml);
        draw_text(fb, popup_x + (VOL_POPUP_W - mw) / 2,
                  popup_y + 12 + system_font_height() + 2, ml,
                  Color::from_rgb(0xCC, 0x33, 0x33));
    }

    // Slider track
    int slider_abs_x = popup_x + VOL_SLIDER_X;
    int slider_abs_y = popup_y + 56;
    fill_rounded_rect(fb, slider_abs_x, slider_abs_y, VOL_SLIDER_W, VOL_SLIDER_H, 4,
                      Color::from_rgb(0xDD, 0xDD, 0xDD));

    // Filled portion
    int fill_w = (display_vol * VOL_SLIDER_W) / 100;
    if (fill_w > 0)
        fill_rounded_rect(fb, slider_abs_x, slider_abs_y, fill_w, VOL_SLIDER_H, 4,
                          ds->settings.accent_color);

    // Knob
    int knob_cx = slider_abs_x + fill_w;
    int knob_cy = slider_abs_y + VOL_SLIDER_H / 2;
    // Draw filled circle for knob
    for (int dy = -VOL_KNOB_R; dy <= VOL_KNOB_R; dy++) {
        for (int dx = -VOL_KNOB_R; dx <= VOL_KNOB_R; dx++) {
            if (dx * dx + dy * dy <= VOL_KNOB_R * VOL_KNOB_R) {
                int px = knob_cx + dx;
                int py = knob_cy + dy;
                if (px >= 0 && px < ds->screen_w && py >= 0 && py < ds->screen_h)
                    fb.put_pixel(px, py, ds->settings.accent_color);
            }
        }
    }
    // White center
    int inner_r = VOL_KNOB_R - 3;
    for (int dy = -inner_r; dy <= inner_r; dy++) {
        for (int dx = -inner_r; dx <= inner_r; dx++) {
            if (dx * dx + dy * dy <= inner_r * inner_r) {
                int px = knob_cx + dx;
                int py = knob_cy + dy;
                if (px >= 0 && px < ds->screen_w && py >= 0 && py < ds->screen_h)
                    fb.put_pixel(px, py, Color::from_rgb(0xFF, 0xFF, 0xFF));
            }
        }
    }

    // Buttons: [-] [+] [Mute]
    int btn_h = 24;
    int btn_y = popup_y + 78;
    int btn_rad = 6;

    int minus_w = 36;
    int plus_w = 36;
    int mute_w = 50;
    int gap = 8;
    int total_w = minus_w + plus_w + mute_w + gap * 2;
    int bx = popup_x + (VOL_POPUP_W - total_w) / 2;

    int mmx = ds->mouse.x;
    int mmy = ds->mouse.y;

    // [-] button
    Color minus_bg = Color::from_rgb(0xE0, 0xE0, 0xE0);
    Rect minus_r = {bx, btn_y, minus_w, btn_h};
    if (minus_r.contains(mmx, mmy)) minus_bg = Color::from_rgb(0xD0, 0xD0, 0xD0);
    fill_rounded_rect(fb, bx, btn_y, minus_w, btn_h, btn_rad, minus_bg);
    int tw = text_width("-");
    draw_text(fb, bx + (minus_w - tw) / 2, btn_y + (btn_h - system_font_height()) / 2, "-", colors::TEXT_COLOR);
    bx += minus_w + gap;

    // [+] button
    Color plus_bg = Color::from_rgb(0xE0, 0xE0, 0xE0);
    Rect plus_r = {bx, btn_y, plus_w, btn_h};
    if (plus_r.contains(mmx, mmy)) plus_bg = Color::from_rgb(0xD0, 0xD0, 0xD0);
    fill_rounded_rect(fb, bx, btn_y, plus_w, btn_h, btn_rad, plus_bg);
    tw = text_width("+");
    draw_text(fb, bx + (plus_w - tw) / 2, btn_y + (btn_h - system_font_height()) / 2, "+", colors::TEXT_COLOR);
    bx += plus_w + gap;

    // [Mute] button
    Color mute_bg = ds->vol_muted ? Color::from_rgb(0xCC, 0x33, 0x33) : Color::from_rgb(0xE0, 0xE0, 0xE0);
    Color mute_fg = ds->vol_muted ? Color::from_rgb(0xFF, 0xFF, 0xFF) : colors::TEXT_COLOR;
    Rect mute_r = {bx, btn_y, mute_w, btn_h};
    if (mute_r.contains(mmx, mmy)) {
        if (ds->vol_muted) mute_bg = Color::from_rgb(0xAA, 0x22, 0x22);
        else mute_bg = Color::from_rgb(0xD0, 0xD0, 0xD0);
    }
    fill_rounded_rect(fb, bx, btn_y, mute_w, btn_h, btn_rad, mute_bg);
    tw = text_width("Mute");
    draw_text(fb, bx + (mute_w - tw) / 2, btn_y + (btn_h - system_font_height()) / 2, "Mute", mute_fg);
}
