/*
    * appearance.cpp
    * Appearance tab for the embedded desktop settings app
    * Copyright (c) 2026 Daniel Hammer
*/

#include "settings_internal.hpp"

namespace settings_app {

void settings_draw_appearance(Canvas& c, SettingsState* st) {
    DesktopSettings& s = st->desktop->settings;
    Color accent = s.accent_color;
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    int x = 16;
    int y = 12;
    int sfh = system_font_height();
    int line_h = sfh + 10;

    // Section: Background
    c.text(x, y, "Background", colors::TEXT_COLOR);
    y += line_h;

    // Radio buttons: Gradient / Solid Color / Image
    bool mode_grad  = s.bg_gradient && !s.bg_image;
    bool mode_solid = !s.bg_gradient && !s.bg_image;
    bool mode_image = s.bg_image;

    draw_radio(c, x, y, mode_grad, accent);
    c.text(x + 20, y + 2, "Gradient", colors::TEXT_COLOR);

    draw_radio(c, x + 120, y, mode_solid, accent);
    c.text(x + 140, y + 2, "Solid", colors::TEXT_COLOR);

    draw_radio(c, x + 220, y, mode_image, accent);
    c.text(x + 240, y + 2, "Image", colors::TEXT_COLOR);
    y += line_h + 4;

    if (mode_image) {
        // Scan for images lazily
        if (!st->wp_scanned) {
            wallpaper_scan_home(st->desktop->home_dir, &st->wp_files);
            st->wp_scanned = true;
        }

        c.text(x, y, "Wallpapers", dim);
        y += sfh + 6;

        if (st->wp_files.count == 0) {
            c.text(x + 8, y, "No .jpg files found", dim);
            y += WP_ITEM_H;
        } else {
            for (int i = 0; i < st->wp_files.count && i < 8; i++) {
                // Build full path for comparison
                char fullpath[256];
                montauk::strcpy(fullpath, st->desktop->home_dir);
                str_append(fullpath, "/", 256);
                str_append(fullpath, st->wp_files.names[i], 256);

                bool selected = s.bg_image &&
                    montauk::streq(s.bg_image_path, fullpath);

                if (selected) {
                    c.fill_rounded_rect(x, y, c.w - 2 * x, WP_ITEM_H, 3, accent);
                }

                // Truncate long filenames
                char label[40];
                int nlen = montauk::slen(st->wp_files.names[i]);
                if (nlen > 35) {
                    montauk::strncpy(label, st->wp_files.names[i], 32);
                    label[32] = '.';
                    label[33] = '.';
                    label[34] = '.';
                    label[35] = '\0';
                } else {
                    montauk::strncpy(label, st->wp_files.names[i], 39);
                }

                Color tc = selected ? colors::WHITE : colors::TEXT_COLOR;
                c.text(x + 8, y + (WP_ITEM_H - sfh) / 2, label, tc);
                y += WP_ITEM_H;
            }
        }
        y += 6;
    } else if (mode_grad) {
        // Top color swatches
        c.text(x, y + 4, "Top", dim);
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_grad_top, accent);
        y += SWATCH_SIZE + 14;

        // Bottom color swatches
        c.text(x, y + 4, "Bottom", dim);
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_grad_bottom, accent);
        y += SWATCH_SIZE + 14;
    } else {
        // Solid color swatches
        c.text(x, y + 4, "Color", dim);
        draw_swatch_row(c, x + 70, y, bg_palette, s.bg_solid, accent);
        y += SWATCH_SIZE + 14;
    }

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    // Panel color
    c.text(x, y + 4, "Panel Color", colors::TEXT_COLOR);
    draw_swatch_row(c, x + 110, y, panel_palette, s.panel_color, accent);
    y += SWATCH_SIZE + 14;

    // Separator
    c.hline(x, y, c.w - 2 * x, colors::BORDER);
    y += 12;

    // Accent color
    c.text(x, y + 4, "Accent Color", colors::TEXT_COLOR);
    draw_swatch_row(c, x + 110, y, accent_palette, s.accent_color, accent);
}

bool settings_handle_appearance_click(Window* win, SettingsState* st, int mx, int cy) {
    DesktopSettings& s = st->desktop->settings;
    int x = 16;
    int sfh = system_font_height();
    int line_h = sfh + 10;
    int y = 12;

    bool mode_grad  = s.bg_gradient && !s.bg_image;
    bool mode_image = s.bg_image;

    // "Background" label
    y += line_h;

    // Radio: Gradient
    if (mx >= x && mx < x + 100 && cy >= y && cy < y + 16) {
        s.bg_gradient = true;
        s.bg_image = false;
        settings_persist(st);
        return true;
    }
    // Radio: Solid
    if (mx >= x + 120 && mx < x + 210 && cy >= y && cy < y + 16) {
        s.bg_gradient = false;
        s.bg_image = false;
        settings_persist(st);
        return true;
    }
    // Radio: Image
    if (mx >= x + 220 && mx < x + 320 && cy >= y && cy < y + 16) {
        s.bg_image = true;
        s.bg_gradient = false;
        if (!st->wp_scanned) {
            wallpaper_scan_home(st->desktop->home_dir, &st->wp_files);
            st->wp_scanned = true;
        }
        settings_persist(st);
        return true;
    }
    y += line_h + 4;

    int idx;
    if (mode_image) {
        // Wallpaper file list
        y += sfh + 6;

        // File list clicks
        for (int i = 0; i < st->wp_files.count && i < 8; i++) {
            if (cy >= y && cy < y + WP_ITEM_H &&
                mx >= x && mx < win->content_w - x) {
                // Build full path and load wallpaper
                char fullpath[256];
                montauk::strcpy(fullpath, st->desktop->home_dir);
                str_append(fullpath, "/", 256);
                str_append(fullpath, st->wp_files.names[i], 256);
                wallpaper_load(&s, fullpath,
                               st->desktop->screen_w, st->desktop->screen_h);
                settings_persist(st);
                return true;
            }
            y += WP_ITEM_H;
        }
        if (st->wp_files.count == 0) y += WP_ITEM_H;
        y += 6;
    } else if (mode_grad) {
        // Top swatches
        if (swatch_hit(mx, cy, x + 70, y, &idx)) {
            s.bg_grad_top = bg_palette[idx];
            settings_persist(st);
            return true;
        }
        y += SWATCH_SIZE + 14;

        // Bottom swatches
        if (swatch_hit(mx, cy, x + 70, y, &idx)) {
            s.bg_grad_bottom = bg_palette[idx];
            settings_persist(st);
            return true;
        }
        y += SWATCH_SIZE + 14;
    } else {
        // Solid color swatches
        if (swatch_hit(mx, cy, x + 70, y, &idx)) {
            s.bg_solid = bg_palette[idx];
            settings_persist(st);
            return true;
        }
        y += SWATCH_SIZE + 14;
    }

    // Separator
    y += 12;

    // Panel color swatches
    if (swatch_hit(mx, cy, x + 110, y, &idx)) {
        s.panel_color = panel_palette[idx];
        settings_persist(st);
        return true;
    }
    y += SWATCH_SIZE + 14;

    // Separator
    y += 12;

    // Accent color swatches
    if (swatch_hit(mx, cy, x + 110, y, &idx)) {
        s.accent_color = accent_palette[idx];
        settings_persist(st);
        return true;
    }

    return false;
}

} // namespace settings_app
