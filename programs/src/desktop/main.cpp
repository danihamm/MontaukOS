/*
    * main.cpp
    * MontaukOS Desktop Environment - window manager, compositor, and run loop
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"
#include "wallpaper.hpp"

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

    // Load TrueType fonts
    fonts::init();

    ds->window_count = 0;
    ds->focused_window = -1;
    ds->prev_buttons = 0;
    ds->app_menu_open = false;

    montauk::memset(&ds->mouse, 0, sizeof(Montauk::MouseState));
    montauk::set_mouse_bounds(ds->screen_w - 1, ds->screen_h - 1);

    // Load SVG icons — scalable (colorful) for app menu, symbolic for toolbar/panel
    Color defColor = colors::ICON_COLOR;
    ds->icon_terminal    = svg_load("0:/icons/utilities-terminal.svg",        20, 20, defColor);
    ds->icon_filemanager = svg_load("0:/icons/system-file-manager.svg",      20, 20, defColor);
    ds->icon_sysinfo     = svg_load("0:/icons/preferences-desktop-apps.svg", 20, 20, defColor);
    ds->icon_appmenu     = svg_load("0:/icons/view-app-grid-symbolic.svg",   20, 20, colors::PANEL_TEXT);
    ds->icon_folder      = svg_load("0:/icons/folder.svg",                   16, 16, defColor);
    ds->icon_file        = svg_load("0:/icons/text-x-generic.svg",           16, 16, defColor);
    ds->icon_computer    = svg_load("0:/icons/computer.svg",                 20, 20, defColor);
    ds->icon_network     = svg_load("0:/icons/network-wired-symbolic.svg",   16, 16, colors::PANEL_TEXT);
    ds->icon_calculator  = svg_load("0:/icons/accessories-calculator.svg",   20, 20, defColor);
    ds->icon_texteditor  = svg_load("0:/icons/accessories-text-editor.svg",  20, 20, defColor);
    ds->icon_go_up       = svg_load("0:/icons/go-up-symbolic.svg",           16, 16, defColor);
    ds->icon_go_back     = svg_load("0:/icons/go-previous-symbolic.svg",     16, 16, defColor);
    ds->icon_go_forward  = svg_load("0:/icons/go-next-symbolic.svg",         16, 16, defColor);
    ds->icon_save        = svg_load("0:/icons/document-save-symbolic.svg",   16, 16, defColor);
    ds->icon_home        = svg_load("0:/icons/user-home.svg",                16, 16, defColor);
    ds->icon_exec        = svg_load("0:/icons/utilities-terminal.svg",        16, 16, defColor);
    ds->icon_wikipedia   = svg_load("0:/icons/web-browser.svg",              20, 20, defColor);

    ds->icon_folder_lg = svg_load("0:/icons/folder.svg",                   48, 48, defColor);
    ds->icon_file_lg   = svg_load("0:/icons/text-x-generic.svg",           48, 48, defColor);
    ds->icon_exec_lg   = svg_load("0:/icons/utilities-terminal.svg",        48, 48, defColor);

    ds->icon_settings = svg_load("0:/icons/help-about.svg",     20, 20, defColor);
    ds->icon_reboot   = svg_load("0:/icons/system-reboot.svg", 20, 20, defColor);
    ds->icon_shutdown = svg_load("0:/icons/system-shutdown.svg", 20, 20, defColor);

    ds->icon_weather = svg_load("0:/icons/weather-widget.svg", 20, 20, defColor);

    ds->icon_doom     = svg_load("0:/icons/doom.svg", 20, 20, defColor);
    ds->icon_procmgr  = svg_load("0:/icons/system-monitor.svg", 20, 20, defColor);
    ds->icon_mandelbrot = svg_load("0:/icons/applications-science.svg", 20, 20, defColor);
    ds->icon_devexplorer = svg_load("0:/icons/hardware.svg", 20, 20, defColor);

    // Settings defaults
    ds->settings.bg_gradient = true;
    ds->settings.bg_image = false;
    ds->settings.bg_grad_top = Color::from_rgb(0xD0, 0xD8, 0xE8);
    ds->settings.bg_grad_bottom = Color::from_rgb(0xA0, 0xA8, 0xB8);
    ds->settings.bg_solid = Color::from_rgb(0xD0, 0xD8, 0xE8);
    ds->settings.bg_image_path[0] = '\0';
    ds->settings.bg_wallpaper = nullptr;
    ds->settings.bg_wallpaper_w = 0;
    ds->settings.bg_wallpaper_h = 0;
    ds->settings.panel_color = Color::from_rgb(0x10, 0x10, 0x10);
    ds->settings.accent_color = colors::ACCENT;
    ds->settings.show_shadows = true;
    ds->settings.clock_24h = true;
    ds->settings.ui_scale = 1;

    // Try to load default wallpaper
    wallpaper_load(&ds->settings, "0:/home/stephen-walker-DaC8D3USffk-unsplash.jpg",
                   ds->screen_w, ds->screen_h);
    montauk::win_setscale(1);

    ds->ctx_menu_open = false;
    ds->ctx_menu_x = 0;
    ds->ctx_menu_y = 0;

    ds->net_popup_open = false;
    montauk::get_netcfg(&ds->cached_net_cfg);
    ds->net_cfg_last_poll = montauk::get_milliseconds();
    ds->net_icon_rect = {0, 0, 0, 0};

}

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

    // For external windows, send a close event instead of freeing the buffer
    if (win->external) {
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
// App Menu (categorized)
// ============================================================================

static constexpr int MENU_W = 220;
static constexpr int MENU_ITEM_H = 36;
static constexpr int MENU_CAT_H = 28;
static constexpr int MENU_DIV_H = 10;

struct MenuRow {
    bool is_category;
    const char* label;    // "" for divider-only rows
    int app_id;           // -1 for category headers / dividers
};

static constexpr int MENU_ROW_COUNT = 20;
static const MenuRow menu_rows[MENU_ROW_COUNT] = {
    { true,  "Applications", -1 },
    { false, "Terminal",      0 },
    { false, "Files",         1 },
    { false, "Text Editor",   4 },
    { false, "Calculator",    3 },
    { true,  "Internet",     -1 },
    { false, "Wikipedia",     9 },
    { false, "Weather",      13 },
    { true,  "System",       -1 },
    { false, "System Info",   2 },
    { false, "Kernel Log",    5 },
    { false, "Processes",     6 },
    { false, "Devices",       8 },
    { true,  "Games",        -1 },
    { false, "Mandelbrot",    7 },
    { false, "DOOM",         10 },
    { true,  "",             -1 },  // divider
    { false, "Settings",     11 },
    { false, "Reboot",       12 },
    { false, "Shutdown",     14 },
};

static int menu_row_height(const MenuRow& row) {
    if (!row.is_category) return MENU_ITEM_H;
    return row.label[0] ? MENU_CAT_H : MENU_DIV_H;
}

static int menu_total_height() {
    int h = 10; // top + bottom padding
    for (int i = 0; i < MENU_ROW_COUNT; i++)
        h += menu_row_height(menu_rows[i]);
    return h;
}

static void desktop_draw_app_menu(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    int menu_x = 4;
    int menu_y = PANEL_HEIGHT + 2;
    int menu_h = menu_total_height();

    // Menu shadow
    draw_shadow(fb, menu_x, menu_y, MENU_W, menu_h, 4, colors::SHADOW);

    // Menu background with rounded corners
    fill_rounded_rect(fb, menu_x, menu_y, MENU_W, menu_h, 8, colors::MENU_BG);
    draw_rect(fb, menu_x, menu_y, MENU_W, menu_h, colors::BORDER);

    // Icon lookup by app_id
    SvgIcon* icons[15] = {
        &ds->icon_terminal,     // 0
        &ds->icon_filemanager,  // 1
        &ds->icon_sysinfo,      // 2
        &ds->icon_calculator,   // 3
        &ds->icon_texteditor,   // 4
        &ds->icon_terminal,     // 5 (Kernel Log)
        &ds->icon_procmgr,      // 6
        &ds->icon_mandelbrot,   // 7
        &ds->icon_devexplorer,  // 8
        &ds->icon_wikipedia,    // 9
        &ds->icon_doom,         // 10
        &ds->icon_settings,     // 11
        &ds->icon_reboot,       // 12
        &ds->icon_weather,      // 13
        &ds->icon_shutdown,     // 14
    };

    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    int iy = menu_y + 5;

    for (int i = 0; i < MENU_ROW_COUNT; i++) {
        const MenuRow& row = menu_rows[i];
        int row_h = menu_row_height(row);

        if (row.is_category) {
            // Separator line above (except first category)
            if (i > 0) {
                for (int sx = menu_x + 8; sx < menu_x + MENU_W - 8; sx++)
                    fb.put_pixel(sx, iy + row_h / 2, colors::BORDER);
            }

            // Category label (dimmed), skip for divider-only rows
            if (row.label[0]) {
                int tx = menu_x + 12;
                int ty = iy + (row_h - system_font_height()) / 2;
                if (i > 0) ty += 2;
                draw_text(fb, tx, ty, row.label, Color::from_rgb(0x88, 0x88, 0x88));
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
            if (row.app_id >= 0 && row.app_id < 15) {
                SvgIcon* icon = icons[row.app_id];
                if (icon && icon->pixels) {
                    fb.blit_alpha(icon_x, icon_y, icon->width, icon->height, icon->pixels);
                }
            }

            // Label
            int tx = icon_x + 28;
            int ty = item_rect.y + (row_h - system_font_height()) / 2;
            draw_text(fb, tx, ty, row.label, colors::TEXT_COLOR);
        }

        iy += row_h;
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

// ============================================================================
// Reboot Dialog
// ============================================================================

struct RebootDialogState {
    DesktopState* ds;
    // Button layout (pixel-buffer-relative coordinates)
    int btn_w, btn_h, btn_y, reboot_x, cancel_x;
    bool hover_reboot, hover_cancel;
};

static void reboot_dialog_on_draw(Window* win, Framebuffer& fb) {
    RebootDialogState* rs = (RebootDialogState*)win->app_data;
    if (!rs) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    // "Reboot the system?" centered
    const char* msg = "Reboot the system?";
    int tw = text_width(msg);
    c.text((c.w - tw) / 2, 30, msg, colors::TEXT_COLOR);

    // Compute button layout
    int btn_w = 100;
    int btn_h = 32;
    int btn_y = c.h - btn_h - 20;
    int gap = 20;
    int total_w = btn_w * 2 + gap;
    int bx = (c.w - total_w) / 2;
    rs->btn_w = btn_w;
    rs->btn_h = btn_h;
    rs->btn_y = btn_y;
    rs->reboot_x = bx;
    rs->cancel_x = bx + btn_w + gap;

    // Draw Reboot button
    Color reboot_bg = rs->hover_reboot
        ? Color::from_rgb(0xDD, 0x44, 0x44)
        : Color::from_rgb(0xCC, 0x33, 0x33);
    c.button(rs->reboot_x, btn_y, btn_w, btn_h, "Reboot", reboot_bg, colors::WHITE, 4);

    // Draw Cancel button
    Color cancel_bg = rs->hover_cancel
        ? Color::from_rgb(0x99, 0x99, 0x99)
        : Color::from_rgb(0x88, 0x88, 0x88);
    c.button(rs->cancel_x, btn_y, btn_w, btn_h, "Cancel", cancel_bg, colors::WHITE, 4);
}

static void reboot_dialog_on_mouse(Window* win, MouseEvent& ev) {
    RebootDialogState* rs = (RebootDialogState*)win->app_data;
    if (!rs) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    Rect rb = {rs->reboot_x, rs->btn_y, rs->btn_w, rs->btn_h};
    Rect cb = {rs->cancel_x, rs->btn_y, rs->btn_w, rs->btn_h};
    rs->hover_reboot = rb.contains(lx, ly);
    rs->hover_cancel = cb.contains(lx, ly);

    if (ev.left_pressed()) {
        if (rs->hover_reboot) montauk::reset();
        if (rs->hover_cancel) {
            for (int i = 0; i < rs->ds->window_count; i++) {
                if (rs->ds->windows[i].app_data == rs) {
                    desktop_close_window(rs->ds, i);
                    return;
                }
            }
        }
    }
}

static void reboot_dialog_on_key(Window* win, const Montauk::KeyEvent& key) {
    RebootDialogState* rs = (RebootDialogState*)win->app_data;
    if (!rs || !key.pressed) return;

    if (key.ascii == '\n' || key.ascii == '\r') {
        montauk::reset();
    }
    if (key.scancode == 0x01) { // Escape
        for (int i = 0; i < rs->ds->window_count; i++) {
            if (rs->ds->windows[i].app_data == rs) {
                desktop_close_window(rs->ds, i);
                return;
            }
        }
    }
}

static void reboot_dialog_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

void open_reboot_dialog(DesktopState* ds) {
    int wx = (ds->screen_w - 300) / 2;
    int wy = (ds->screen_h - 150) / 2;
    int idx = desktop_create_window(ds, "Reboot", wx, wy, 300, 150);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    RebootDialogState* rs = (RebootDialogState*)montauk::malloc(sizeof(RebootDialogState));
    montauk::memset(rs, 0, sizeof(RebootDialogState));
    rs->ds = ds;

    win->app_data = rs;
    win->on_draw = reboot_dialog_on_draw;
    win->on_mouse = reboot_dialog_on_mouse;
    win->on_key = reboot_dialog_on_key;
    win->on_close = reboot_dialog_on_close;
}

// ============================================================================
// Shutdown Dialog
// ============================================================================

struct ShutdownDialogState {
    DesktopState* ds;
    int btn_w, btn_h, btn_y, shutdown_x, cancel_x;
    bool hover_shutdown, hover_cancel;
};

static void shutdown_dialog_on_draw(Window* win, Framebuffer& fb) {
    ShutdownDialogState* ss = (ShutdownDialogState*)win->app_data;
    if (!ss) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    const char* msg = "Shut down the system?";
    int tw = text_width(msg);
    c.text((c.w - tw) / 2, 30, msg, colors::TEXT_COLOR);

    int btn_w = 100;
    int btn_h = 32;
    int btn_y = c.h - btn_h - 20;
    int gap = 20;
    int total_w = btn_w * 2 + gap;
    int bx = (c.w - total_w) / 2;
    ss->btn_w = btn_w;
    ss->btn_h = btn_h;
    ss->btn_y = btn_y;
    ss->shutdown_x = bx;
    ss->cancel_x = bx + btn_w + gap;

    Color shutdown_bg = ss->hover_shutdown
        ? Color::from_rgb(0xDD, 0x44, 0x44)
        : Color::from_rgb(0xCC, 0x33, 0x33);
    c.button(ss->shutdown_x, btn_y, btn_w, btn_h, "Shut Down", shutdown_bg, colors::WHITE, 4);

    Color cancel_bg = ss->hover_cancel
        ? Color::from_rgb(0x99, 0x99, 0x99)
        : Color::from_rgb(0x88, 0x88, 0x88);
    c.button(ss->cancel_x, btn_y, btn_w, btn_h, "Cancel", cancel_bg, colors::WHITE, 4);
}

static void shutdown_dialog_on_mouse(Window* win, MouseEvent& ev) {
    ShutdownDialogState* ss = (ShutdownDialogState*)win->app_data;
    if (!ss) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    Rect sb = {ss->shutdown_x, ss->btn_y, ss->btn_w, ss->btn_h};
    Rect cb = {ss->cancel_x, ss->btn_y, ss->btn_w, ss->btn_h};
    ss->hover_shutdown = sb.contains(lx, ly);
    ss->hover_cancel = cb.contains(lx, ly);

    if (ev.left_pressed()) {
        if (ss->hover_shutdown) montauk::shutdown();
        if (ss->hover_cancel) {
            for (int i = 0; i < ss->ds->window_count; i++) {
                if (ss->ds->windows[i].app_data == ss) {
                    desktop_close_window(ss->ds, i);
                    return;
                }
            }
        }
    }
}

static void shutdown_dialog_on_key(Window* win, const Montauk::KeyEvent& key) {
    ShutdownDialogState* ss = (ShutdownDialogState*)win->app_data;
    if (!ss || !key.pressed) return;

    if (key.ascii == '\n' || key.ascii == '\r') {
        montauk::shutdown();
    }
    if (key.scancode == 0x01) { // Escape
        for (int i = 0; i < ss->ds->window_count; i++) {
            if (ss->ds->windows[i].app_data == ss) {
                desktop_close_window(ss->ds, i);
                return;
            }
        }
    }
}

static void shutdown_dialog_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

void open_shutdown_dialog(DesktopState* ds) {
    int wx = (ds->screen_w - 300) / 2;
    int wy = (ds->screen_h - 150) / 2;
    int idx = desktop_create_window(ds, "Shut Down", wx, wy, 300, 150);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    ShutdownDialogState* ss = (ShutdownDialogState*)montauk::malloc(sizeof(ShutdownDialogState));
    montauk::memset(ss, 0, sizeof(ShutdownDialogState));
    ss->ds = ds;

    win->app_data = ss;
    win->on_draw = shutdown_dialog_on_draw;
    win->on_mouse = shutdown_dialog_on_mouse;
    win->on_key = shutdown_dialog_on_key;
    win->on_close = shutdown_dialog_on_close;
}

static gui::ResizeEdge hit_test_resize_edge(const gui::Rect& f, int mx, int my) {
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

static gui::CursorStyle cursor_for_edge(gui::ResizeEdge edge) {
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

    // Draw cursor last
    draw_cursor(fb, ds->mouse.x, ds->mouse.y, cur_style);
}

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
            // Walk rows to find which one was clicked
            int iy = menu_y + 5;
            for (int i = 0; i < MENU_ROW_COUNT; i++) {
                const MenuRow& row = menu_rows[i];
                int row_h = menu_row_height(row);
                if (my >= iy && my < iy + row_h) {
                    if (!row.is_category) {
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

// ============================================================================
// External Window Polling
// ============================================================================

void desktop_poll_external_windows(DesktopState* ds) {
    Montauk::WinInfo extWins[8];
    int extCount = montauk::win_enumerate(extWins, 8);

    // Check for new external windows and map them
    for (int e = 0; e < extCount; e++) {
        int extId = extWins[e].id;

        // Check if we already have this window
        bool found = false;
        for (int i = 0; i < ds->window_count; i++) {
            if (ds->windows[i].external && ds->windows[i].ext_win_id == extId) {
                found = true;
                // Update dirty flag
                if (extWins[e].dirty) {
                    ds->windows[i].dirty = true;
                }
                // Re-map if external app resized its buffer
                if (extWins[e].width != ds->windows[i].content_w ||
                    extWins[e].height != ds->windows[i].content_h) {
                    uint64_t va = montauk::win_map(extId);
                    if (va != 0) {
                        ds->windows[i].content = (uint32_t*)va;
                        ds->windows[i].content_w = extWins[e].width;
                        ds->windows[i].content_h = extWins[e].height;
                        ds->windows[i].dirty = true;
                    }
                }
                break;
            }
        }

        if (!found && ds->window_count < MAX_WINDOWS) {
            // Map the pixel buffer into our address space
            uint64_t va = montauk::win_map(extId);
            if (va == 0) continue;

            int idx = ds->window_count;
            Window* win = &ds->windows[idx];
            montauk::memset(win, 0, sizeof(Window));

            montauk::strncpy(win->title, extWins[e].title, MAX_TITLE_LEN);
            int w = extWins[e].width;
            int h = extWins[e].height;
            // Position the window centered-ish
            int wx = (ds->screen_w - w - 2 * BORDER_WIDTH) / 2 + idx * 30;
            int wy = PANEL_HEIGHT + 20 + idx * 30;
            win->frame = {wx, wy, w + 2 * BORDER_WIDTH, h + TITLEBAR_HEIGHT + BORDER_WIDTH};
            win->state = WIN_NORMAL;
            win->z_order = idx;
            win->focused = true;
            win->dirty = true;
            win->dragging = false;
            win->resizing = false;
            win->saved_frame = win->frame;

            // Point content to the shared pixel buffer
            win->content = (uint32_t*)va;
            win->content_w = w;
            win->content_h = h;

            win->on_draw = nullptr;
            win->on_mouse = nullptr;
            win->on_key = nullptr;
            win->on_close = nullptr;
            win->on_poll = nullptr;
            win->app_data = nullptr;
            win->external = true;
            win->ext_win_id = extId;

            // Unfocus previous window
            if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
                ds->windows[ds->focused_window].focused = false;
            }
            ds->focused_window = idx;
            ds->window_count++;
        }
    }

    // Check for removed external windows (process exited)
    for (int i = ds->window_count - 1; i >= 0; i--) {
        if (!ds->windows[i].external) continue;

        int extId = ds->windows[i].ext_win_id;
        bool stillExists = false;
        for (int e = 0; e < extCount; e++) {
            if (extWins[e].id == extId) {
                stillExists = true;
                break;
            }
        }

        if (!stillExists) {
            // Window gone — remove without freeing content (shared memory)
            ds->windows[i].content = nullptr; // prevent free
            gui::desktop_close_window(ds, i);
        }
    }
}

void gui::desktop_run(DesktopState* ds) {
    for (;;) {
        // Poll mouse state
        ds->prev_buttons = ds->mouse.buttons;
        montauk::mouse_state(&ds->mouse);

        // Poll keyboard events
        while (montauk::is_key_available()) {
            Montauk::KeyEvent key;
            montauk::getkey(&key);
            desktop_handle_keyboard(ds, key);
        }

        // Poll external windows (discover new, remove dead, update dirty)
        desktop_poll_external_windows(ds);

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
        montauk::sleep_ms(16);
    }
}

// ============================================================================
// Entry Point
// ============================================================================

static DesktopState* g_desktop;

extern "C" void _start() {
    DesktopState* ds = (DesktopState*)montauk::malloc(sizeof(DesktopState));
    montauk::memset(ds, 0, sizeof(DesktopState));

    // Placement-new the Framebuffer since it has a constructor
    new (&ds->fb) Framebuffer();

    g_desktop = ds;

    desktop_init(ds);
    desktop_run(ds);

    montauk::exit(0);
}
