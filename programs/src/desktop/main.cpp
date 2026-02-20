/*
    * main.cpp
    * ZenithOS Desktop Environment - window manager, compositor, and run loop
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Desktop Implementation
// ============================================================================

static const char* month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

void gui::desktop_init(DesktopState* ds) {
    ds->screen_w = ds->fb.width();
    ds->screen_h = ds->fb.height();

    // Immediately clear the screen to hide flanterm boot text
    ds->fb.clear(colors::DESKTOP_BG);
    ds->fb.flip();

    ds->window_count = 0;
    ds->focused_window = -1;
    ds->prev_buttons = 0;
    ds->app_menu_open = false;

    zenith::memset(&ds->mouse, 0, sizeof(Zenith::MouseState));
    zenith::set_mouse_bounds(ds->screen_w - 1, ds->screen_h - 1);

    // Load SVG icons
    ds->icon_terminal    = svg_load("0:/icons/utilities-terminal-symbolic.svg",        20, 20, colors::ICON_COLOR);
    ds->icon_filemanager = svg_load("0:/icons/system-file-manager-symbolic.svg",       20, 20, colors::ICON_COLOR);
    ds->icon_sysinfo     = svg_load("0:/icons/preferences-desktop-apps-symbolic.svg",  20, 20, colors::ICON_COLOR);
    ds->icon_appmenu     = svg_load("0:/icons/view-app-grid-symbolic.svg",             20, 20, colors::PANEL_TEXT);
    ds->icon_folder      = svg_load("0:/icons/folder-symbolic.svg",                    16, 16, Color::from_rgb(0xFF, 0xBD, 0x2E));
    ds->icon_file        = svg_load("0:/icons/text-x-generic-symbolic.svg",            16, 16, colors::ICON_COLOR);
    ds->icon_computer    = svg_load("0:/icons/computer-symbolic.svg",                  20, 20, colors::ICON_COLOR);
    ds->icon_network     = svg_load("0:/icons/network-wired-symbolic.svg",             16, 16, colors::PANEL_TEXT);
    ds->icon_calculator  = svg_load("0:/icons/accessories-calculator-symbolic.svg",    20, 20, colors::ICON_COLOR);
    ds->icon_texteditor  = svg_load("0:/icons/accessories-text-editor-symbolic.svg",   20, 20, colors::ICON_COLOR);
    ds->icon_go_up       = svg_load("0:/icons/go-up-symbolic.svg",                    16, 16, colors::ICON_COLOR);
    ds->icon_go_back     = svg_load("0:/icons/go-previous-symbolic.svg",              16, 16, colors::ICON_COLOR);
    ds->icon_go_forward  = svg_load("0:/icons/go-next-symbolic.svg",                  16, 16, colors::ICON_COLOR);
    ds->icon_save        = svg_load("0:/icons/document-save-symbolic.svg",            16, 16, colors::ICON_COLOR);
    ds->icon_home        = svg_load("0:/icons/user-home-symbolic.svg",                16, 16, colors::ICON_COLOR);
    ds->icon_exec        = svg_load("0:/icons/application-x-executable-symbolic.svg", 16, 16, Color::from_rgb(0x4E, 0x9A, 0x06));

    ds->net_popup_open = false;
    zenith::get_netcfg(&ds->cached_net_cfg);
    ds->net_cfg_last_poll = zenith::get_milliseconds();
    ds->net_icon_rect = {0, 0, 0, 0};

}

int gui::desktop_create_window(DesktopState* ds, const char* title, int x, int y, int w, int h) {
    if (ds->window_count >= MAX_WINDOWS) return -1;

    int idx = ds->window_count;
    Window* win = &ds->windows[idx];
    zenith::memset(win, 0, sizeof(Window));

    zenith::strncpy(win->title, title, MAX_TITLE_LEN);
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
    win->content = (uint32_t*)zenith::alloc(buf_size);
    zenith::memset(win->content, 0xFF, buf_size);

    win->on_draw = nullptr;
    win->on_mouse = nullptr;
    win->on_key = nullptr;
    win->on_close = nullptr;
    win->on_poll = nullptr;
    win->app_data = nullptr;

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
    if (win->on_close) win->on_close(win);

    // Free content buffer
    if (win->content) {
        zenith::free(win->content);
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
    int title_y = y + (TITLEBAR_HEIGHT - FONT_HEIGHT) / 2;
    int title_w = text_width(win->title);
    // Center in remaining space
    int remaining_w = w - (title_x - x) - 12;
    if (remaining_w > title_w) {
        title_x += (remaining_w - title_w) / 2;
    }
    draw_text(fb, title_x, title_y, win->title, colors::TEXT_COLOR);

    // Call app draw callback to render content
    if (win->on_draw) {
        win->on_draw(win, fb);
    }

    // Blit content buffer to framebuffer
    Rect cr = win->content_rect();
    if (win->content) {
        fb.blit(cr.x, cr.y, cr.w, cr.h, win->content);
    }
}

void gui::desktop_draw_panel(DesktopState* ds) {
    Framebuffer& fb = ds->fb;
    int sw = ds->screen_w;

    // Panel gradient background (slightly lighter at top)
    for (int y = 0; y < PANEL_HEIGHT; y++) {
        int t = y * 255 / PANEL_HEIGHT;
        uint8_t r = colors::PANEL_BG.r + (10 - t * 10 / 255);
        uint8_t g = colors::PANEL_BG.g + (10 - t * 10 / 255);
        uint8_t b = colors::PANEL_BG.b + (10 - t * 10 / 255);
        fb.fill_rect(0, y, sw, 1, Color::from_rgb(r, g, b));
    }

    // Bottom highlight line
    fb.fill_rect(0, PANEL_HEIGHT - 1, sw, 1, Color::from_rgba(0xFF, 0xFF, 0xFF, 0x10));

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
            fb.fill_rect(indicator_x + 4, 26, iw - 8, 2, colors::ACCENT);
        }

        // Truncate title if too long
        char short_title[20];
        zenith::strncpy(short_title, win->title, 18);

        int tx = indicator_x + pad;
        int ty = 4 + (24 - FONT_HEIGHT) / 2;
        draw_text(fb, tx, ty, short_title, colors::PANEL_TEXT);

        indicator_x += iw + 4;
    }

    // Date + Clock (right side)
    Zenith::DateTime dt;
    zenith::gettime(&dt);

    char clock_str[8];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d", (int)dt.Hour, (int)dt.Minute);
    int clock_w = text_width(clock_str);
    int clock_x = sw - clock_w - 12;
    int clock_y = (PANEL_HEIGHT - FONT_HEIGHT) / 2;
    draw_text(fb, clock_x, clock_y, clock_str, colors::PANEL_TEXT);

    // Date before clock
    char date_str[12];
    int month_idx = dt.Month > 0 && dt.Month <= 12 ? dt.Month - 1 : 0;
    snprintf(date_str, sizeof(date_str), "%s %d", month_names[month_idx], (int)dt.Day);
    int date_w = text_width(date_str);
    int date_x = clock_x - date_w - 16;
    draw_text(fb, date_x, clock_y, date_str, colors::PANEL_TEXT);

    // Network icon (to the left of the date)
    uint64_t now = zenith::get_milliseconds();
    if (now - ds->net_cfg_last_poll > 5000) {
        zenith::get_netcfg(&ds->cached_net_cfg);
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
// App Menu (5 items with separator and rounded corners)
// ============================================================================

static constexpr int MENU_ITEM_COUNT = 6;
static constexpr int MENU_W = 220;
static constexpr int MENU_ITEM_H = 40;

static void desktop_draw_app_menu(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    int menu_x = 4;
    int menu_y = PANEL_HEIGHT + 2;
    int menu_h = MENU_ITEM_H * MENU_ITEM_COUNT + 10; // +10 for padding + separator

    // Menu shadow
    draw_shadow(fb, menu_x, menu_y, MENU_W, menu_h, 4, colors::SHADOW);

    // Menu background with rounded corners
    fill_rounded_rect(fb, menu_x, menu_y, MENU_W, menu_h, 8, colors::MENU_BG);
    draw_rect(fb, menu_x, menu_y, MENU_W, menu_h, colors::BORDER);

    // Menu items
    struct MenuItem {
        const char* label;
        SvgIcon* icon;
    };
    MenuItem items[MENU_ITEM_COUNT] = {
        { "Terminal",     &ds->icon_terminal },
        { "Files",        &ds->icon_filemanager },
        { "System Info",  &ds->icon_sysinfo },
        { "Calculator",   &ds->icon_calculator },
        { "Text Editor",  &ds->icon_texteditor },
        { "Kernel Log",   &ds->icon_terminal },
    };

    int mx = ds->mouse.x;
    int my = ds->mouse.y;

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int iy = menu_y + 4 + i * MENU_ITEM_H;

        // Thin separator line after System Info (before utility apps)
        if (i == 3) {
            int sep_y = iy - 1;
            for (int sx = menu_x + 8; sx < menu_x + MENU_W - 8; sx++)
                fb.put_pixel(sx, sep_y, colors::BORDER);
            iy += 1;
        }

        Rect item_rect = {menu_x + 4, iy, MENU_W - 8, MENU_ITEM_H};

        // Hover highlight
        if (item_rect.contains(mx, my)) {
            fill_rounded_rect(fb, item_rect.x, item_rect.y, item_rect.w, item_rect.h, 4, colors::MENU_HOVER);
        }

        // Icon
        int icon_x = item_rect.x + 8;
        int icon_y = item_rect.y + (MENU_ITEM_H - 20) / 2;
        if (items[i].icon && items[i].icon->pixels) {
            fb.blit_alpha(icon_x, icon_y, items[i].icon->width, items[i].icon->height, items[i].icon->pixels);
        }

        // Label
        int tx = icon_x + 28;
        int ty = item_rect.y + (MENU_ITEM_H - FONT_HEIGHT) / 2;
        draw_text(fb, tx, ty, items[i].label, colors::TEXT_COLOR);
    }
}

static void desktop_draw_net_popup(DesktopState* ds) {
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
    int line_h = FONT_HEIGHT + 6;
    char line[64];

    Zenith::NetCfg& nc = ds->cached_net_cfg;

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
        if (y < grad_start) {
            // Panel area - will be overwritten by panel drawing
            uint32_t px = colors::PANEL_BG.to_pixel();
            for (int x = 0; x < sw; x++) row[x] = px;
        } else {
            int t = y - grad_start;
            uint8_t r = 0xD0 - (0xD0 - 0xA0) * t / grad_range;
            uint8_t g = 0xD8 - (0xD8 - 0xA8) * t / grad_range;
            uint8_t b = 0xE8 - (0xE8 - 0xB8) * t / grad_range;
            uint32_t px = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
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

    // Draw cursor last
    draw_cursor(fb, ds->mouse.x, ds->mouse.y);
}

void gui::desktop_handle_mouse(DesktopState* ds) {
    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    uint8_t buttons = ds->mouse.buttons;
    uint8_t prev = ds->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);
    bool left_held = (buttons & 0x01);
    bool left_released = !(buttons & 0x01) && (prev & 0x01);

    MouseEvent ev;
    ev.x = mx;
    ev.y = my;
    ev.buttons = buttons;
    ev.prev_buttons = prev;
    ev.scroll = ds->mouse.scrollDelta;

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
            }
            return;
        }
    }

    // Handle app menu clicks
    if (ds->app_menu_open && left_pressed) {
        int menu_x = 4;
        int menu_y = PANEL_HEIGHT + 2;
        int menu_h = MENU_ITEM_H * MENU_ITEM_COUNT + 10;
        Rect menu_rect = {menu_x, menu_y, MENU_W, menu_h};

        if (menu_rect.contains(mx, my)) {
            int rel_y = my - menu_y - 4;
            int item_idx = rel_y / MENU_ITEM_H;
            if (item_idx >= 0 && item_idx < MENU_ITEM_COUNT) {
                switch (item_idx) {
                case 0: open_terminal(ds); break;
                case 1: open_filemanager(ds); break;
                case 2: open_sysinfo(ds); break;
                case 3: open_calculator(ds); break;
                case 4: open_texteditor(ds); break;
                case 5: open_klog(ds); break;
                }
                ds->app_menu_open = false;
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
            return;
        }

        // Network icon
        if (ds->net_icon_rect.w > 0 && ds->net_icon_rect.contains(mx, my)) {
            ds->net_popup_open = !ds->net_popup_open;
            ds->app_menu_open = false;
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
                Rect cr = win->content_rect();
                if (cr.w != win->content_w || cr.h != win->content_h) {
                    if (win->content) zenith::free(win->content);
                    win->content_w = cr.w;
                    win->content_h = cr.h;
                    win->content = (uint32_t*)zenith::alloc(cr.w * cr.h * 4);
                    zenith::memset(win->content, 0xFF, cr.w * cr.h * 4);
                }
                desktop_raise_window(ds, i);
                return;
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
                if (ds->windows[new_idx].on_mouse) {
                    ev.x = mx;
                    ev.y = my;
                    ds->windows[new_idx].on_mouse(&ds->windows[new_idx], ev);
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
    }

    // Handle scroll events on focused window
    if (ev.scroll != 0 && ds->focused_window >= 0) {
        Window* win = &ds->windows[ds->focused_window];
        Rect cr = win->content_rect();
        if (cr.contains(mx, my) && win->on_mouse) {
            win->on_mouse(win, ev);
        }
    }
}

void gui::desktop_handle_keyboard(DesktopState* ds, const Zenith::KeyEvent& key) {
    if (!key.pressed) return;

    // Global shortcuts
    if (key.ctrl && key.alt) {
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
        if (key.ascii == 'k' || key.ascii == 'K') {
            open_klog(ds);
            return;
        }
    }

    // Dispatch to focused window
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        Window* win = &ds->windows[ds->focused_window];
        if (win->on_key) {
            win->on_key(win, key);
        }
    }
}

void gui::desktop_run(DesktopState* ds) {
    for (;;) {
        // Poll mouse state
        ds->prev_buttons = ds->mouse.buttons;
        zenith::mouse_state(&ds->mouse);

        // Poll keyboard events
        while (zenith::is_key_available()) {
            Zenith::KeyEvent key;
            zenith::getkey(&key);
            desktop_handle_keyboard(ds, key);
        }

        // Poll windows that have a poll callback
        for (int i = 0; i < ds->window_count; i++) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_CLOSED) continue;
            if (win->on_poll) {
                win->on_poll(win);
            }
        }

        // Handle mouse events
        desktop_handle_mouse(ds);

        // Compose and present
        desktop_compose(ds);
        ds->fb.flip();

        // Target ~60fps
        zenith::sleep_ms(16);
    }
}

// ============================================================================
// Entry Point
// ============================================================================

static DesktopState* g_desktop;

extern "C" void _start() {
    DesktopState* ds = (DesktopState*)zenith::malloc(sizeof(DesktopState));
    zenith::memset(ds, 0, sizeof(DesktopState));

    // Placement-new the Framebuffer since it has a constructor
    new (&ds->fb) Framebuffer();

    g_desktop = ds;

    desktop_init(ds);
    desktop_run(ds);

    zenith::exit(0);
}
