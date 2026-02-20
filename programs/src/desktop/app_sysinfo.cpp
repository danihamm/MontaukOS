/*
    * app_sysinfo.cpp
    * ZenithOS Desktop - System Info application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// System Info state and callbacks
// ============================================================================

struct SysInfoState {
    Zenith::SysInfo sys_info;
    Zenith::NetCfg net_cfg;
    uint64_t uptime_ms;
};

static void sysinfo_on_draw(Window* win, Framebuffer& fb) {
    SysInfoState* si = (SysInfoState*)win->app_data;
    if (!si) return;

    // Refresh uptime
    si->uptime_ms = zenith::get_milliseconds();

    Rect cr = win->content_rect();
    int cw = cr.w;
    int ch = cr.h;
    uint32_t* pixels = win->content;

    // Fill background
    uint32_t bg_px = colors::WINDOW_BG.to_pixel();
    for (int i = 0; i < cw * ch; i++) pixels[i] = bg_px;

    uint32_t text_px = colors::TEXT_COLOR.to_pixel();
    uint32_t accent_px = colors::ACCENT.to_pixel();
    int y = 16;
    int x = 16;
    char line[128];

    // Title
    draw_text_to_pixels(pixels, cw, ch, x, y, "System Information", accent_px);
    y += FONT_HEIGHT + 12;

    // Separator
    uint32_t sep_px = colors::BORDER.to_pixel();
    for (int sx = x; sx < cw - x && y < ch; sx++)
        pixels[y * cw + sx] = sep_px;
    y += 8;

    // OS Name
    snprintf(line, sizeof(line), "OS:       %s", si->sys_info.osName);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // OS Version
    snprintf(line, sizeof(line), "Version:  %s", si->sys_info.osVersion);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // API Version
    snprintf(line, sizeof(line), "API:      %d", (int)si->sys_info.apiVersion);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // Max Processes
    snprintf(line, sizeof(line), "Max PIDs: %d", (int)si->sys_info.maxProcesses);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 12;

    // Uptime
    int up_sec = (int)(si->uptime_ms / 1000);
    int up_min = up_sec / 60;
    int up_hr = up_min / 60;
    snprintf(line, sizeof(line), "Uptime:   %d:%02d:%02d", up_hr, up_min % 60, up_sec % 60);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 12;

    // Network section
    draw_text_to_pixels(pixels, cw, ch, x, y, "Network", accent_px);
    y += FONT_HEIGHT + 8;

    for (int sx = x; sx < cw - x && y < ch; sx++)
        pixels[y * cw + sx] = sep_px;
    y += 8;

    // IP Address
    uint32_t ip = si->net_cfg.ipAddress;
    snprintf(line, sizeof(line), "IP:       %d.%d.%d.%d",
        (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
        (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // Subnet
    uint32_t mask = si->net_cfg.subnetMask;
    snprintf(line, sizeof(line), "Subnet:   %d.%d.%d.%d",
        (int)(mask & 0xFF), (int)((mask >> 8) & 0xFF),
        (int)((mask >> 16) & 0xFF), (int)((mask >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // Gateway
    uint32_t gw = si->net_cfg.gateway;
    snprintf(line, sizeof(line), "Gateway:  %d.%d.%d.%d",
        (int)(gw & 0xFF), (int)((gw >> 8) & 0xFF),
        (int)((gw >> 16) & 0xFF), (int)((gw >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // DNS
    uint32_t dns = si->net_cfg.dnsServer;
    snprintf(line, sizeof(line), "DNS:      %d.%d.%d.%d",
        (int)(dns & 0xFF), (int)((dns >> 8) & 0xFF),
        (int)((dns >> 16) & 0xFF), (int)((dns >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // MAC Address
    snprintf(line, sizeof(line), "MAC:      %02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned)si->net_cfg.macAddress[0], (unsigned)si->net_cfg.macAddress[1],
        (unsigned)si->net_cfg.macAddress[2], (unsigned)si->net_cfg.macAddress[3],
        (unsigned)si->net_cfg.macAddress[4], (unsigned)si->net_cfg.macAddress[5]);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
}

static void sysinfo_on_close(Window* win) {
    if (win->app_data) {
        zenith::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// System Info launcher
// ============================================================================

void open_sysinfo(DesktopState* ds) {
    int idx = desktop_create_window(ds, "System Info", 300, 100, 400, 380);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    SysInfoState* si = (SysInfoState*)zenith::malloc(sizeof(SysInfoState));
    zenith::memset(si, 0, sizeof(SysInfoState));
    zenith::get_info(&si->sys_info);
    zenith::get_netcfg(&si->net_cfg);
    si->uptime_ms = zenith::get_milliseconds();

    win->app_data = si;
    win->on_draw = sysinfo_on_draw;
    win->on_mouse = nullptr;
    win->on_key = nullptr;
    win->on_close = sysinfo_on_close;
}
