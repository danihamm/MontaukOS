/*
    * persistence.cpp
    * Settings persistence helpers for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

static int64_t color_to_int(Color c) {
    return ((int64_t)c.r << 16) | ((int64_t)c.g << 8) | c.b;
}

void settings_persist(SettingsState* st) {
    DesktopSettings& s = st->desktop->settings;
    const char* user = st->desktop->current_user;

    montauk::toml::Doc doc;
    doc.init();

    // Background mode
    const char* mode = s.bg_image ? "image" : (s.bg_gradient ? "gradient" : "solid");
    montauk::config::set_string(&doc, "background.mode", mode);

    // Wallpaper path
    if (s.bg_image && s.bg_image_path[0])
        montauk::config::set_string(&doc, "wallpaper.path", s.bg_image_path);

    // Background colors
    montauk::config::set_int(&doc, "background.solid_color", color_to_int(s.bg_solid));
    montauk::config::set_int(&doc, "background.grad_top", color_to_int(s.bg_grad_top));
    montauk::config::set_int(&doc, "background.grad_bottom", color_to_int(s.bg_grad_bottom));

    // Appearance
    montauk::config::set_int(&doc, "appearance.panel_color", color_to_int(s.panel_color));
    montauk::config::set_int(&doc, "appearance.accent_color", color_to_int(s.accent_color));

    // Display
    montauk::config::set_int(&doc, "display.ui_scale", s.ui_scale);
    montauk::config::set_bool(&doc, "display.clock_24h", s.clock_24h);
    montauk::config::set_bool(&doc, "display.show_shadows", s.show_shadows);

    montauk::config::save_user(user, "desktop", &doc);
    doc.destroy();
}

void settings_persist_tz(SettingsState* st) {
    montauk::toml::Doc doc;
    doc.init();
    montauk::config::set_int(&doc, "timezone.offset_minutes", st->desktop->settings.tz_offset_minutes);
    montauk::config::save("timezone", &doc);
    doc.destroy();
}

} // namespace settings_app
