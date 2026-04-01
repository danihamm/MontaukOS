/*
    * about.cpp
    * About tab for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

void settings_draw_about(Canvas& c, SettingsState* st) {
    st->uptime_ms = montauk::get_milliseconds();

    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    int x = 16;
    int y = 20;
    char line[128];
    int sfh = system_font_height();
    int line_h = sfh + 6;

    // OS name in 2x size
    c.text_2x(x, y, st->sys_info.osName, st->desktop->settings.accent_color);
    int large_h = (fonts::system_font && fonts::system_font->valid)
        ? fonts::system_font->get_line_height(fonts::LARGE_SIZE) : (FONT_HEIGHT * 2);
    y += large_h + 8;

    snprintf(line, sizeof(line), "Version %s", st->sys_info.osVersion);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += line_h + 8;

    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    snprintf(line, sizeof(line), "API version: %d", (int)st->sys_info.apiVersion);
    c.kv_line(x, &y, line, colors::TEXT_COLOR, line_h);

    int up_sec = (int)(st->uptime_ms / 1000);
    int up_min = up_sec / 60;
    int up_hr = up_min / 60;
    snprintf(line, sizeof(line), "Uptime: %d:%02d:%02d", up_hr, up_min % 60, up_sec % 60);
    c.kv_line(x, &y, line, colors::TEXT_COLOR, line_h);
    y += 8;

    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    c.text(x, y, "Copyright (c) 2026 Daniel Hammer", dim);
}

} // namespace settings_app
