/*
    * app_sysinfo.cpp
    * MontaukOS Desktop - System Info application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// System Info state and callbacks
// ============================================================================

struct SysInfoState {
    Montauk::SysInfo sys_info;
    Montauk::NetCfg net_cfg;
    uint64_t uptime_ms;
};

static void sysinfo_on_draw(Window* win, Framebuffer& fb) {
    SysInfoState* si = (SysInfoState*)win->app_data;
    if (!si) return;

    si->uptime_ms = montauk::get_milliseconds();

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    int y = 16;
    int x = 16;
    char line[128];

    // Title
    c.text(x, y, "System Information", colors::ACCENT);
    y += system_font_height() + 12;

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 8;

    // OS Name
    snprintf(line, sizeof(line), "OS:       %s", si->sys_info.osName);
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // OS Version
    snprintf(line, sizeof(line), "Version:  %s", si->sys_info.osVersion);
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // API Version
    snprintf(line, sizeof(line), "API:      %d", (int)si->sys_info.apiVersion);
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // Max Processes
    snprintf(line, sizeof(line), "Max PIDs: %d", (int)si->sys_info.maxProcesses);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += system_font_height() + 12;

    // Uptime
    int up_sec = (int)(si->uptime_ms / 1000);
    int up_min = up_sec / 60;
    int up_hr = up_min / 60;
    snprintf(line, sizeof(line), "Uptime:   %d:%02d:%02d", up_hr, up_min % 60, up_sec % 60);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += system_font_height() + 12;

    // Network section
    c.text(x, y, "Network", colors::ACCENT);
    y += system_font_height() + 8;

    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 8;

    // IP Address
    uint32_t ip = si->net_cfg.ipAddress;
    snprintf(line, sizeof(line), "IP:       %d.%d.%d.%d",
        (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
        (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // Subnet
    uint32_t mask = si->net_cfg.subnetMask;
    snprintf(line, sizeof(line), "Subnet:   %d.%d.%d.%d",
        (int)(mask & 0xFF), (int)((mask >> 8) & 0xFF),
        (int)((mask >> 16) & 0xFF), (int)((mask >> 24) & 0xFF));
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // Gateway
    uint32_t gw = si->net_cfg.gateway;
    snprintf(line, sizeof(line), "Gateway:  %d.%d.%d.%d",
        (int)(gw & 0xFF), (int)((gw >> 8) & 0xFF),
        (int)((gw >> 16) & 0xFF), (int)((gw >> 24) & 0xFF));
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // DNS
    uint32_t dns = si->net_cfg.dnsServer;
    snprintf(line, sizeof(line), "DNS:      %d.%d.%d.%d",
        (int)(dns & 0xFF), (int)((dns >> 8) & 0xFF),
        (int)((dns >> 16) & 0xFF), (int)((dns >> 24) & 0xFF));
    c.kv_line(x, &y, line, colors::TEXT_COLOR);

    // MAC Address
    snprintf(line, sizeof(line), "MAC:      %02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned)si->net_cfg.macAddress[0], (unsigned)si->net_cfg.macAddress[1],
        (unsigned)si->net_cfg.macAddress[2], (unsigned)si->net_cfg.macAddress[3],
        (unsigned)si->net_cfg.macAddress[4], (unsigned)si->net_cfg.macAddress[5]);
    c.text(x, y, line, colors::TEXT_COLOR);
}

static void sysinfo_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
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
    SysInfoState* si = (SysInfoState*)montauk::malloc(sizeof(SysInfoState));
    montauk::memset(si, 0, sizeof(SysInfoState));
    montauk::get_info(&si->sys_info);
    montauk::get_netcfg(&si->net_cfg);
    si->uptime_ms = montauk::get_milliseconds();

    win->app_data = si;
    win->on_draw = sysinfo_on_draw;
    win->on_mouse = nullptr;
    win->on_key = nullptr;
    win->on_close = sysinfo_on_close;
}
