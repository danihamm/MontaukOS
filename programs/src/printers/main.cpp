/*
 * main.cpp
 * MontaukOS Printers - standalone Window Server app
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/config.h>
#include <montauk/heap.h>
#include <montauk/string.h>
#include <montauk/syscall.h>
#include <gui/gui.hpp>
#include <gui/canvas.hpp>
#include <gui/svg.hpp>
#include <gui/standalone.hpp>
#include <gui/truetype.hpp>
#include <print/print.hpp>

extern "C" {
#include <dirent.h>
#include <stdio.h>
#include <string.h>
}

using namespace gui;
using namespace print;

static constexpr int INIT_W = 560;
static constexpr int INIT_H = 500;
static constexpr int CONFIG_W = 408;
static constexpr int CONFIG_H = 220;
static constexpr int TAB_BAR_H = 36;
static constexpr int PAD_X = 16;
static constexpr int PAD_Y = 20;
static constexpr int BTN_H = 30;
static constexpr int ACTION_RADIUS = 4;
static constexpr int FIELD_H = 32;
static constexpr int PRINTER_ICON_SIZE = 24;

enum PrinterTab {
    TAB_OVERVIEW = 0,
    TAB_DETAILS  = 1,
    TAB_COUNT    = 2,
};

static const char* g_tab_labels[TAB_COUNT] = {
    "Overview",
    "Details",
};

struct AppState {
    int active_tab;
    char printer_uri[MAX_PATH_LEN];
    bool printers_loaded;
    bool daemon_running;
    int daemon_pid;
    int queue_count;
    int active_count;
    int done_count;
    int failed_count;
    uint64_t last_refresh_ms;

    JobMeta last_active;
    JobMeta last_failed;
    bool has_active;
    bool has_failed;

    IppCapabilities probe_caps;
    bool probe_valid;
    bool probe_ok;
    char probe_message[128];
    char probe_time[32];
    char probe_detail[MAX_DEBUG_LEN];

    char status_msg[128];
    uint64_t status_time;

    bool config_open;
    int config_button_down;
    char edit_uri[MAX_PATH_LEN];
    int edit_len;
    char edit_error[128];

    Color accent;
};

struct Layout {
    Rect configure_btn;
    Rect clear_btn;
    Rect probe_btn;
    Rect start_btn;
    Rect refresh_btn;
    Rect test_btn;
    Rect printer_icon;
};

struct ConfigLayout {
    Rect input;
    Rect save_btn;
    Rect cancel_btn;
};

struct ConfigPopup {
    int win_id;
    uint32_t* pixels;
    int width;
    int height;
    bool open;
};

static WsWindow g_win;
static ConfigPopup g_config = {-1, nullptr, CONFIG_W, CONFIG_H, false};
static AppState g_app = {};
static SvgIcon g_printer_icon = {};

static void render_config_popup();

static void str_append(char* dst, const char* src, int max) {
    int len = montauk::slen(dst);
    int i = 0;
    while (src[i] && len < max - 1) dst[len++] = src[i++];
    dst[len] = '\0';
}

static bool status_visible() {
    return g_app.status_msg[0] &&
           (montauk::get_milliseconds() - g_app.status_time < 4000);
}

static void set_status(const char* msg) {
    montauk::strncpy(g_app.status_msg, msg ? msg : "", (int)sizeof(g_app.status_msg) - 1);
    g_app.status_time = montauk::get_milliseconds();
}

static void clear_probe() {
    montauk::memset(&g_app.probe_caps, 0, sizeof(g_app.probe_caps));
    g_app.probe_valid = false;
    g_app.probe_ok = false;
    g_app.probe_message[0] = '\0';
    g_app.probe_time[0] = '\0';
    g_app.probe_detail[0] = '\0';
}

static void apply_probe_state(const ProbeState& state) {
    g_app.probe_caps = state.caps;
    g_app.probe_valid = state.printer_uri[0] != '\0' || state.message[0] != '\0';
    g_app.probe_ok = state.ok;
    safe_copy(g_app.probe_message, sizeof(g_app.probe_message), state.message);
    safe_copy(g_app.probe_time, sizeof(g_app.probe_time), state.probed_at);
    safe_copy(g_app.probe_detail, sizeof(g_app.probe_detail), state.detail);
}

static void set_accent(Color accent) {
    g_app.accent = accent;
}

static void load_accent() {
    set_accent(colors::ACCENT);

    char user[64];
    montauk::memset(user, 0, sizeof(user));
    if (montauk::getuser(user, sizeof(user)) > 0 && user[0]) {
        auto doc = montauk::config::load_user(user, "desktop");
        int64_t accent = doc.get_int("appearance.accent_color", -1);
        if (accent >= 0) {
            set_accent(Color::from_rgb(
                (uint8_t)((accent >> 16) & 0xFF),
                (uint8_t)((accent >> 8) & 0xFF),
                (uint8_t)(accent & 0xFF)));
            doc.destroy();
            return;
        }
        doc.destroy();
    }

    auto doc = montauk::config::load("desktop");
    int64_t accent = doc.get_int("appearance.accent_color", -1);
    if (accent >= 0) {
        set_accent(Color::from_rgb(
            (uint8_t)((accent >> 16) & 0xFF),
            (uint8_t)((accent >> 8) & 0xFF),
            (uint8_t)(accent & 0xFF)));
    }
    doc.destroy();
}

static void load_icons() {
    if (g_printer_icon.pixels) svg_free(g_printer_icon);
    g_printer_icon = svg_load("0:/icons/printer-symbolic.svg",
                              PRINTER_ICON_SIZE, PRINTER_ICON_SIZE,
                              Color::from_rgb(0x6A, 0x72, 0x7D));
}

static int count_dir_entries(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;

    int count = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    return count;
}

static bool load_latest_job(const char* dir, JobMeta* out) {
    if (out) zero_job(out);
    DIR* d = opendir(dir);
    if (!d) return false;

    bool found = false;
    char best_key[32] = {};
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        char path[MAX_PATH_LEN];
        path_join(path, sizeof(path), dir, ent->d_name);

        JobMeta job = {};
        if (!load_job_from_path(path, &job)) continue;

        const char* key = job.updated_at[0] ? job.updated_at : job.created_at;
        if (!found || strcmp(key, best_key) > 0) {
            found = true;
            safe_copy(best_key, sizeof(best_key), key);
            if (out) *out = job;
        }
    }

    closedir(d);
    return found;
}

static void text_fit(const char* src, char* out, int out_len, int max_w) {
    if (!src || !out || out_len <= 0) return;
    montauk::strncpy(out, src, out_len - 1);
    if (text_width(out) <= max_w) return;

    const char* ell = "...";
    int keep = montauk::slen(src);
    while (keep > 0) {
        char tmp[MAX_DEBUG_LEN];
        int n = keep < (int)sizeof(tmp) - 4 ? keep : (int)sizeof(tmp) - 4;
        for (int i = 0; i < n; i++) tmp[i] = src[i];
        tmp[n] = '\0';
        str_append(tmp, ell, (int)sizeof(tmp));
        if (text_width(tmp) <= max_w) {
            montauk::strncpy(out, tmp, out_len - 1);
            return;
        }
        keep--;
    }
    montauk::strncpy(out, ell, out_len - 1);
}

static void draw_action_btn(Canvas& c, const Rect& r, const char* label,
                            bool enabled, bool primary) {
    Color bg;
    Color fg;
    if (!enabled) {
        bg = Color::from_rgb(0xE6, 0xE6, 0xE6);
        fg = Color::from_rgb(0x9A, 0x9A, 0x9A);
    } else if (primary) {
        bg = g_app.accent;
        fg = colors::WHITE;
    } else {
        bg = colors::WINDOW_BG;
        fg = colors::TEXT_COLOR;
    }

    c.fill_rounded_rect(r.x, r.y, r.w, r.h, ACTION_RADIUS, bg);
    if (enabled && !primary) c.rect(r.x, r.y, r.w, r.h, colors::BORDER);
    if (!enabled) c.rect(r.x, r.y, r.w, r.h, Color::from_rgb(0xD4, 0xD4, 0xD4));

    int tw = text_width(label);
    int fh = system_font_height();
    c.text(r.x + (r.w - tw) / 2, r.y + (r.h - fh) / 2, label, fg);
}

static void draw_chip(Canvas& c, int x, int y, const char* label, Color bg, Color fg = colors::WHITE) {
    int sfh = system_font_height();
    int chip_h = sfh + 8;
    int chip_w = text_width(label) + 18;
    c.fill_rounded_rect(x, y, chip_w, chip_h, chip_h / 2, bg);
    c.text(x + 9, y + (chip_h - sfh) / 2, label, fg);
}

static int chip_height() {
    return system_font_height() + 8;
}

static int control_row_text_y(int row_y) {
    return row_y + 6;
}

static int control_row_chip_y(int row_y) {
    return row_y + 2;
}

static void draw_overview_stat_row(Canvas& c, int x, int y, int w,
                                   const char* label, int value) {
    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "%d", value);

    c.text(x, y, label, colors::TEXT_COLOR);
    int value_w = text_width(value_buf);
    c.text(x + w - value_w, y, value_buf, colors::TEXT_COLOR);
}

static void draw_tabs(Canvas& c) {
    int sfh = system_font_height();
    c.fill_rect(0, 0, g_win.width, TAB_BAR_H, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.hline(0, TAB_BAR_H - 1, g_win.width, colors::BORDER);

    int tab_w = g_win.width / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = i * tab_w;
        bool active = (i == g_app.active_tab);
        if (active) {
            c.fill_rect(tx, 0, tab_w, TAB_BAR_H, colors::WINDOW_BG);
            c.fill_rect(tx + 4, TAB_BAR_H - 3, tab_w - 8, 3, g_app.accent);
        }

        int tw = text_width(g_tab_labels[i]);
        Color tc = active ? g_app.accent : Color::from_rgb(0x66, 0x66, 0x66);
        c.text(tx + (tab_w - tw) / 2, (TAB_BAR_H - sfh) / 2, g_tab_labels[i], tc);
    }
}

static void draw_input_field(Canvas& c, int x, int y, int w, int h,
                             const char* label, const char* value, bool focused) {
    int sfh = system_font_height();
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);

    c.text(x, y, label, dim);
    y += sfh + 2;
    c.fill_rounded_rect(x, y, w, h, 3, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.rect(x, y, w, h, focused ? g_app.accent : colors::BORDER);
    c.text(x + 8, y + (h - sfh) / 2, value, colors::TEXT_COLOR);

    if (focused) {
        int tw = text_width(value);
        c.fill_rect(x + 8 + tw, y + 6, 2, h - 12, g_app.accent);
    }
}

static void compute_layout(Layout* lo) {
    int sfh = system_font_height();
    int x = PAD_X;
    int y = TAB_BAR_H + PAD_Y;

    if (status_visible()) y += sfh + 12;

    lo->configure_btn = {0, 0, 0, 0};
    lo->clear_btn = {0, 0, 0, 0};
    lo->probe_btn = {0, 0, 0, 0};
    lo->start_btn = {0, 0, 0, 0};
    lo->refresh_btn = {0, 0, 0, 0};
    lo->test_btn = {0, 0, 0, 0};
    lo->printer_icon = {0, 0, 0, 0};

    if (g_app.active_tab == TAB_OVERVIEW) {
        int right_edge = g_win.width - x;
        lo->clear_btn = {right_edge - 84, y, 84, BTN_H};
        lo->configure_btn = {lo->clear_btn.x - 8 - 112, y, 112, BTN_H};
        if (g_app.probe_ok && g_app.probe_caps.printer_name[0] && g_printer_icon.pixels) {
            lo->printer_icon = {
                x,
                y + (BTN_H - PRINTER_ICON_SIZE) / 2,
                PRINTER_ICON_SIZE,
                PRINTER_ICON_SIZE
            };
        }

        y += BTN_H + 8;
        y += sfh + 4;
        y += sfh + 4;
        if (!g_app.probe_ok || !g_app.probe_caps.printer_name[0])
            y += sfh + 4;
        y += sfh + 14;
        y += 16;

        int queue_row_h = sfh + 10;
        y += sfh + 8;
        y += queue_row_h * 4;
        y += sfh + 8;
        if (g_app.has_failed) y += sfh + 4;
        if (!g_app.daemon_running) y += sfh + 4;
        y += 14;
        y += 16;

        lo->test_btn = {g_win.width - x - 132, y, 132, BTN_H};
    } else {
        lo->probe_btn = {g_win.width - x - 92, y, 92, BTN_H};

        y += BTN_H + 8;
        if (g_app.printer_uri[0]) {
            char err[128] = {};
            IppUri normalized = {};
            if (normalize_ipp_uri(g_app.printer_uri, &normalized, err, sizeof(err))) {
                y += sfh + 4;
                y += sfh + 4;
                y += sfh + 4;
                y += sfh + 4;
            } else {
                y += sfh + 4;
                y += sfh + 4;
            }
        } else {
            y += sfh + 4;
            y += sfh + 4;
        }
        y += sfh + 4;
        if (g_app.probe_time[0]) y += sfh + 4;
        if (g_app.probe_valid) {
            y += sfh + 4;
            if (g_app.probe_ok) y += sfh + 4;
            if (g_app.probe_caps.supported_formats[0]) y += sfh + 4;
            if (g_app.probe_detail[0]) y += sfh + 4;
        } else {
            y += sfh + 4;
        }
        y += 14;
        y += 16;

        lo->start_btn = {g_win.width - x - 118 - 8 - 86, y, 118, BTN_H};
        lo->refresh_btn = {lo->start_btn.x + 126, y, 86, BTN_H};

        y += BTN_H + 8;
        y += sfh + 6;
        y += sfh + 4;
        y += sfh + 4;
        if (g_app.has_active) y += sfh + 4;
        if (g_app.has_active && g_app.last_active.debug_info[0]) y += sfh + 4;
        if (g_app.has_failed) {
            y += sfh + 4;
            if (g_app.last_failed.debug_info[0]) y += sfh + 4;
        }
        y += 14;
        y += 16;
    }

}

static void compute_config_layout(ConfigLayout* lo) {
    int pad = 16;
    int sfh = system_font_height();
    int field_w = g_config.width - pad * 2;
    if (field_w < 120) field_w = 120;

    int input_box_y = 16 + sfh + 6 + sfh + 10 + sfh + 2;
    lo->input = {pad, input_box_y, field_w, FIELD_H};
    lo->save_btn = {pad, g_config.height - BTN_H - 16, 80, BTN_H};
    lo->cancel_btn = {pad + 88, g_config.height - BTN_H - 16, 80, BTN_H};
}

static void refresh_state(bool force = false) {
    uint64_t now = montauk::get_milliseconds();
    if (!force && g_app.printers_loaded && now - g_app.last_refresh_ms < 1000) return;

    ensure_spool_dirs();

    if (!read_default_printer_uri(g_app.printer_uri, sizeof(g_app.printer_uri)))
        g_app.printer_uri[0] = '\0';

    ProbeState probe = {};
    if (g_app.printer_uri[0] && load_printer_probe_state(&probe) &&
        montauk::streq(probe.printer_uri, g_app.printer_uri))
        apply_probe_state(probe);
    else
        clear_probe();

    g_app.daemon_pid = -1;
    g_app.daemon_running = daemon_is_running(&g_app.daemon_pid);
    g_app.queue_count = count_dir_entries(SPOOL_QUEUE_DIR);
    g_app.active_count = count_dir_entries(SPOOL_ACTIVE_DIR);
    g_app.done_count = count_dir_entries(SPOOL_DONE_DIR);
    g_app.failed_count = count_dir_entries(SPOOL_FAILED_DIR);
    g_app.has_active = load_latest_job(SPOOL_ACTIVE_DIR, &g_app.last_active);
    g_app.has_failed = load_latest_job(SPOOL_FAILED_DIR, &g_app.last_failed);
    g_app.printers_loaded = true;
    g_app.last_refresh_ms = now;
}

static void probe_printer() {
    ProbeState probe = {};
    char err[128] = {};
    bool ok = probe_printer_uri(g_app.printer_uri, &probe, err, sizeof(err));
    apply_probe_state(probe);
    if (probe.printer_uri[0]) save_printer_probe_state(&probe);
    set_status(ok ? "Printer probe succeeded" : (probe.message[0] ? probe.message : "Printer probe failed"));
}

static void close_config_popup() {
    if (g_config.open && g_config.win_id >= 0)
        montauk::win_destroy(g_config.win_id);
    g_config.win_id = -1;
    g_config.pixels = nullptr;
    g_config.width = CONFIG_W;
    g_config.height = CONFIG_H;
    g_config.open = false;
    g_app.config_open = false;
}

static void open_config_popup() {
    safe_copy(g_app.edit_uri, sizeof(g_app.edit_uri), g_app.printer_uri);
    g_app.edit_len = montauk::slen(g_app.edit_uri);
    g_app.edit_error[0] = '\0';
    g_app.config_button_down = 0;

    if (!g_config.open) {
        Montauk::WinCreateResult wres;
        if (montauk::win_create("Configure Printer", CONFIG_W, CONFIG_H, &wres) < 0 || wres.id < 0) {
            set_status("Failed to open printer dialog");
            return;
        }
        g_config.win_id = wres.id;
        g_config.pixels = (uint32_t*)(uintptr_t)wres.pixelVa;
        g_config.width = CONFIG_W;
        g_config.height = CONFIG_H;
        g_config.open = true;
    }

    g_app.config_open = true;
    render_config_popup();
}

static void save_configured_printer() {
    if (g_app.edit_len == 0) {
        safe_copy(g_app.edit_error, sizeof(g_app.edit_error), "Printer URI is required");
        return;
    }

    char err[128] = {};
    IppUri uri = {};
    if (!normalize_ipp_uri(g_app.edit_uri, &uri, err, sizeof(err))) {
        safe_copy(g_app.edit_error, sizeof(g_app.edit_error), err);
        return;
    }

    if (!write_default_printer_uri(uri.normalized)) {
        safe_copy(g_app.edit_error, sizeof(g_app.edit_error), "Failed to save printer");
        return;
    }

    close_config_popup();
    refresh_state(true);
    set_status("Printer saved");
}

static void render_config_popup() {
    if (!g_app.config_open || !g_config.open || g_config.win_id < 0 || g_config.pixels == nullptr) return;

    Canvas c(g_config.pixels, g_config.width, g_config.height);
    c.fill(colors::WINDOW_BG);

    ConfigLayout lo = {};
    compute_config_layout(&lo);
    int sfh = system_font_height();
    int x = 16;
    int y = 16;
    c.text(x, y, "Default Printer", colors::TEXT_COLOR);
    y += sfh + 6;
    c.text(x, y, "Enter an IPP or IPPS destination URI.", Color::from_rgb(0x88, 0x88, 0x88));
    y += sfh + 10;

    draw_input_field(c, lo.input.x, lo.input.y - (sfh + 2),
                     lo.input.w, lo.input.h, "Printer URI",
                     g_app.edit_uri, true);

    y = lo.input.y + lo.input.h + 12;
    c.text(x, y, "Example: ipp://printer.local/ipp/print", Color::from_rgb(0x88, 0x88, 0x88));
    y += sfh + 10;

    if (g_app.edit_error[0])
        c.text(x, y, g_app.edit_error, Color::from_rgb(0xD0, 0x3E, 0x3E));

    draw_action_btn(c, lo.save_btn, "Save", true, true);
    draw_action_btn(c, lo.cancel_btn, "Cancel", true, false);
    montauk::win_present(g_config.win_id);
}

static bool handle_config_mouse(int mx, int my, uint8_t buttons, uint8_t prev_buttons) {
    ConfigLayout lo = {};
    compute_config_layout(&lo);

    bool left_pressed = (buttons & 1) && !(prev_buttons & 1);
    bool left_released = !(buttons & 1) && (prev_buttons & 1);

    if (left_pressed) {
        if (lo.save_btn.contains(mx, my))
            g_app.config_button_down = 1;
        else if (lo.cancel_btn.contains(mx, my))
            g_app.config_button_down = 2;
        else
            g_app.config_button_down = 0;
        return false;
    }

    if (!left_released) return false;

    int pressed = g_app.config_button_down;
    g_app.config_button_down = 0;

    if (pressed == 1 && lo.save_btn.contains(mx, my)) {
        save_configured_printer();
        return !g_app.config_open;
    }
    if (pressed == 2 && lo.cancel_btn.contains(mx, my)) {
        close_config_popup();
        return true;
    }
    return false;
}

static bool handle_config_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return false;

    if (key.scancode == 0x01) {
        close_config_popup();
        return true;
    }
    if (key.ascii == '\n' || key.ascii == '\r' || key.scancode == 0x1C) {
        save_configured_printer();
        return !g_app.config_open;
    }
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        if (g_app.edit_len > 0) {
            g_app.edit_len--;
            g_app.edit_uri[g_app.edit_len] = '\0';
            g_app.edit_error[0] = '\0';
        }
        return false;
    }
    if (key.ascii >= 0x20 && key.ascii < 0x7F && g_app.edit_len < MAX_PATH_LEN - 1) {
        g_app.edit_uri[g_app.edit_len++] = key.ascii;
        g_app.edit_uri[g_app.edit_len] = '\0';
        g_app.edit_error[0] = '\0';
    }
    return false;
}

static void render_overview(Canvas& c, const Layout& lo) {
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    Color success = Color::from_rgb(0x22, 0x88, 0x22);
    Color danger = Color::from_rgb(0xCC, 0x33, 0x33);
    Color quiet_bg = Color::from_rgb(0xE6, 0xE6, 0xE6);
    int sfh = system_font_height();
    int x = PAD_X;
    int y = TAB_BAR_H + PAD_Y;
    int w = g_win.width - 2 * x;
    int title_x = x;
    char title[MAX_DEBUG_LEN];

    if (status_visible()) {
        c.text(x, y, g_app.status_msg, g_app.accent);
        y += sfh + 12;
    }

    const char* chip_label = "Not Set";
    Color chip_bg = quiet_bg;
    Color chip_fg = colors::TEXT_COLOR;
    if (g_app.printer_uri[0] && g_app.probe_ok) {
        chip_label = "Ready";
        chip_bg = success;
        chip_fg = colors::WHITE;
    } else if (g_app.printer_uri[0] && g_app.probe_valid) {
        chip_label = "Problem";
        chip_bg = danger;
        chip_fg = colors::WHITE;
    } else if (g_app.printer_uri[0]) {
        chip_label = "Checking";
        chip_bg = Color::from_rgb(0xE3, 0xE8, 0xEF);
        chip_fg = colors::TEXT_COLOR;
    }

    int chip_w = text_width(chip_label) + 18;
    int chip_x = lo.configure_btn.x - chip_w - 12;
    if (chip_x < x + 74) chip_x = x + 74;

    if (lo.printer_icon.w > 0) {
        title_x = lo.printer_icon.x + lo.printer_icon.w + 10;
        if (chip_x < title_x + 80) chip_x = title_x + 80;
    }

    if (g_app.probe_ok && g_app.probe_caps.printer_name[0])
        safe_copy(title, sizeof(title), g_app.probe_caps.printer_name);
    else
        safe_copy(title, sizeof(title), "Printer");

    int title_max_w = chip_x - title_x - 12;
    if (title_max_w < 80) title_max_w = 80;
    text_fit(title, title, sizeof(title), title_max_w);

    if (lo.printer_icon.w > 0 && g_printer_icon.pixels)
        c.icon(lo.printer_icon.x, lo.printer_icon.y, g_printer_icon);
    c.text(title_x, control_row_text_y(y), title, colors::TEXT_COLOR);
    draw_chip(c, chip_x, control_row_chip_y(y), chip_label, chip_bg, chip_fg);
    draw_action_btn(c, lo.configure_btn, "Configure", true, true);
    draw_action_btn(c, lo.clear_btn, "Clear", g_app.printer_uri[0] != '\0', false);
    y += BTN_H + 8;

    char line[MAX_DEBUG_LEN];
    if (!g_app.probe_ok || !g_app.probe_caps.printer_name[0]) {
        if (g_app.printer_uri[0])
            safe_copy(line, sizeof(line), "Configured printer");
        else
            safe_copy(line, sizeof(line), "No printer configured");
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, colors::TEXT_COLOR);
        y += sfh + 4;
    }

    if (g_app.printer_uri[0]) {
        text_fit(g_app.printer_uri, line, sizeof(line), w);
        c.text(x, y, line, dim);
    } else {
        c.text(x, y, "Choose an IPP or IPPS printer to start printing.", dim);
    }
    y += sfh + 4;

    if (!g_app.printer_uri[0]) {
        c.text(x, y, "The saved printer is shared system-wide through the spooler.", dim);
    } else if (g_app.probe_ok) {
        if (g_app.probe_caps.status_message[0])
            safe_copy(line, sizeof(line), g_app.probe_caps.status_message);
        else
            safe_copy(line, sizeof(line), "Printer is reachable and ready for queued jobs.");
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, dim);
    } else if (g_app.probe_valid) {
        text_fit(g_app.probe_message, line, sizeof(line), w);
        c.text(x, y, line, danger);
    } else {
        c.text(x, y, "Waiting for the spooler to probe the configured printer.", dim);
    }
    y += sfh + 14;
    c.hline(x, y, w, colors::BORDER);
    y += 16;

    if (!g_app.printer_uri[0]) {
        c.text(x, y, "Queue and test-page controls appear after a printer is configured.", dim);
        return;
    }

    c.text(x, y, "Queue", colors::TEXT_COLOR);
    y += sfh + 8;

    int row_h = sfh + 10;
    draw_overview_stat_row(c, x, y, w, "Waiting", g_app.queue_count);
    y += row_h;
    c.hline(x, y - 5, w, Color::from_rgb(0xEB, 0xEB, 0xEB));

    draw_overview_stat_row(c, x, y, w, "Printing", g_app.active_count);
    y += row_h;
    c.hline(x, y - 5, w, Color::from_rgb(0xEB, 0xEB, 0xEB));

    draw_overview_stat_row(c, x, y, w, "Completed", g_app.done_count);
    y += row_h;
    c.hline(x, y - 5, w, Color::from_rgb(0xEB, 0xEB, 0xEB));

    draw_overview_stat_row(c, x, y, w, "Failed", g_app.failed_count);
    y += row_h;
    y += 2;

    if (g_app.has_active) {
        snprintf(line, sizeof(line), "Now printing: %s", g_app.last_active.job_name);
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, dim);
        y += sfh + 4;
    } else if (g_app.queue_count > 0) {
        c.text(x, y, "Jobs are waiting to be picked up by the spooler.", dim);
        y += sfh + 4;
    } else {
        c.text(x, y, "No jobs in queue.", dim);
        y += sfh + 4;
    }

    if (g_app.has_failed) {
        snprintf(line, sizeof(line), "Last failure: %s", g_app.last_failed.status_message);
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, danger);
        y += sfh + 4;
    }

    if (!g_app.daemon_running) {
        c.text(x, y, "Printing is paused until the spooler starts.", danger);
        y += sfh + 4;
    }

    y += 10;
    c.hline(x, y, w, colors::BORDER);
    y += 16;

    c.text(x, y + 6, "Test Page", colors::TEXT_COLOR);
    draw_action_btn(c, lo.test_btn, "Print Test Page", true, true);
    y += BTN_H + 8;
    c.text(x, y, "Queue a simple test page through the print spooler.", dim);
    y += sfh + 4;
    c.text(x, y, "Uses the configured default printer.", colors::TEXT_COLOR);
}

static void render_details(Canvas& c, const Layout& lo) {
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    Color success = Color::from_rgb(0x22, 0x88, 0x22);
    Color danger = Color::from_rgb(0xCC, 0x33, 0x33);
    int sfh = system_font_height();
    int x = PAD_X;
    int y = TAB_BAR_H + PAD_Y;
    int w = g_win.width - 2 * x;
    char line[MAX_DEBUG_LEN];

    if (status_visible()) {
        c.text(x, y, g_app.status_msg, g_app.accent);
        y += sfh + 12;
    }

    c.text(x, y + 6, "Connection", colors::TEXT_COLOR);
    draw_action_btn(c, lo.probe_btn, "Probe", g_app.printer_uri[0] != '\0', false);
    y += BTN_H + 8;

    if (g_app.printer_uri[0]) {
        IppUri normalized = {};
        char err[128] = {};
        if (normalize_ipp_uri(g_app.printer_uri, &normalized, err, sizeof(err))) {
            text_fit(g_app.printer_uri, line, sizeof(line), w);
            c.text(x, y, line, colors::TEXT_COLOR);
            y += sfh + 4;

            snprintf(line, sizeof(line), "Host: %s", normalized.host);
            c.text(x, y, line, dim);
            y += sfh + 4;

            uint32_t ip = 0;
            if (resolve_host(normalized.host, &ip)) {
                char ip_text[20];
                format_ipv4(ip_text, sizeof(ip_text), ip);
                snprintf(line, sizeof(line), "Resolved: %s    Transport: %s", ip_text,
                         normalized.use_tls ? "TLS" : "Plain TCP");
            } else if (host_looks_like_mdns(normalized.host)) {
                safe_copy(line, sizeof(line), "Resolved: unavailable (.local/mDNS not supported yet)");
            } else {
                safe_copy(line, sizeof(line), "Resolved: unavailable");
            }
            text_fit(line, line, sizeof(line), w);
            c.text(x, y, line, dim);
            y += sfh + 4;

            snprintf(line, sizeof(line), "Path: %s    Port: %u", normalized.path, (unsigned)normalized.port);
            text_fit(line, line, sizeof(line), w);
            c.text(x, y, line, dim);
        } else {
            c.text(x, y, "Printer URI is invalid", danger);
            y += sfh + 4;
            text_fit(err, line, sizeof(line), w);
            c.text(x, y, line, dim);
        }
    } else {
        c.text(x, y, "No printer configured", dim);
        y += sfh + 4;
        c.text(x, y, "Use Overview to configure the system printer.", dim);
    }
    y += sfh + 4;

    if (g_app.probe_time[0]) {
        snprintf(line, sizeof(line), "Last probe: %s", g_app.probe_time);
        c.text(x, y, line, dim);
        y += sfh + 4;
    }

    if (g_app.probe_valid) {
        if (g_app.probe_ok && g_app.probe_caps.printer_name[0])
            snprintf(line, sizeof(line), "Printer: %s", g_app.probe_caps.printer_name);
        else
            snprintf(line, sizeof(line), "Probe: %s",
                     g_app.probe_message[0] ? g_app.probe_message : (g_app.probe_ok ? "ok" : "failed"));
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, g_app.probe_ok ? colors::TEXT_COLOR : danger);
        y += sfh + 4;

        if (g_app.probe_ok) {
            snprintf(line, sizeof(line), "Caps: PDF %s, JPEG %s, text %s",
                     g_app.probe_caps.supports_pdf ? "yes" : "no",
                     g_app.probe_caps.supports_jpeg ? "yes" : "no",
                     g_app.probe_caps.supports_text ? "yes" : "no");
            c.text(x, y, line, dim);
            y += sfh + 4;
        }

        if (g_app.probe_caps.supported_formats[0]) {
            snprintf(line, sizeof(line), "Formats: %s", g_app.probe_caps.supported_formats);
            text_fit(line, line, sizeof(line), w);
            c.text(x, y, line, dim);
            y += sfh + 4;
        }

        if (g_app.probe_detail[0]) {
            text_fit(g_app.probe_detail, line, sizeof(line), w);
            c.text(x, y, line, dim);
            y += sfh + 4;
        }
    } else {
        c.text(x, y, "Waiting for daemon auto-probe or run Probe.", dim);
        y += sfh + 4;
    }

    y += 10;
    c.hline(x, y, w, colors::BORDER);
    y += 16;

    c.text(x, control_row_text_y(y), "Spooler", colors::TEXT_COLOR);
    draw_action_btn(c, lo.start_btn, "Start Daemon", !g_app.daemon_running, false);
    draw_action_btn(c, lo.refresh_btn, "Refresh", true, false);

    const char* daemon_label = g_app.daemon_running ? "Running" : "Stopped";
    int chip_w = text_width(daemon_label) + 18;
    int chip_x = lo.start_btn.x - chip_w - 12;
    if (chip_x < x + 70) chip_x = x + 70;
    draw_chip(c, chip_x, control_row_chip_y(y), daemon_label, g_app.daemon_running ? success : danger);
    y += BTN_H + 8;

    if (g_app.daemon_running)
        snprintf(line, sizeof(line), "Daemon PID %d", g_app.daemon_pid);
    else
        safe_copy(line, sizeof(line), "Spooler is not active");
    c.text(x, y, line, dim);
    y += sfh + 6;

    snprintf(line, sizeof(line), "Queued: %d    Printing: %d", g_app.queue_count, g_app.active_count);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += sfh + 4;
    snprintf(line, sizeof(line), "Completed: %d    Failed: %d", g_app.done_count, g_app.failed_count);
    c.text(x, y, line, colors::TEXT_COLOR);
    y += sfh + 4;

    if (g_app.has_active) {
        snprintf(line, sizeof(line), "Active: %s", g_app.last_active.job_name);
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, dim);
        y += sfh + 4;

        if (g_app.last_active.debug_info[0]) {
            snprintf(line, sizeof(line), "Debug: %s", g_app.last_active.debug_info);
            text_fit(line, line, sizeof(line), w);
            c.text(x, y, line, dim);
            y += sfh + 4;
        }
    }

    if (g_app.has_failed) {
        snprintf(line, sizeof(line), "Last failure: %s", g_app.last_failed.status_message);
        text_fit(line, line, sizeof(line), w);
        c.text(x, y, line, danger);
        y += sfh + 4;

        if (g_app.last_failed.debug_info[0]) {
            snprintf(line, sizeof(line), "Debug: %s", g_app.last_failed.debug_info);
            text_fit(line, line, sizeof(line), w);
            c.text(x, y, line, dim);
            y += sfh + 4;
        }
    }
}

static void render() {
    refresh_state();

    Canvas c = g_win.canvas();
    c.fill(colors::WINDOW_BG);
    Layout lo = {};
    compute_layout(&lo);
    draw_tabs(c);

    switch (g_app.active_tab) {
    case TAB_OVERVIEW:
        render_overview(c, lo);
        break;
    case TAB_DETAILS:
        render_details(c, lo);
        break;
    }

    g_win.present();
}

static void handle_click(int mx, int my) {
    Layout lo = {};
    compute_layout(&lo);

    if (g_app.config_open)
        return;

    if (my >= 0 && my < TAB_BAR_H) {
        int tab_w = g_win.width / TAB_COUNT;
        int tab = mx / tab_w;
        if (tab >= 0 && tab < TAB_COUNT) g_app.active_tab = tab;
        return;
    }

    if (g_app.active_tab == TAB_OVERVIEW && lo.configure_btn.contains(mx, my)) {
        open_config_popup();
        return;
    }

    if (g_app.active_tab == TAB_OVERVIEW && g_app.printer_uri[0] && lo.clear_btn.contains(mx, my)) {
        if (montauk::fdelete(DEFAULT_PRINTER_PATH) < 0) set_status("Failed to clear printer");
        else {
            refresh_state(true);
            set_status("Printer cleared");
        }
        return;
    }

    if (g_app.active_tab == TAB_DETAILS && g_app.printer_uri[0] && lo.probe_btn.contains(mx, my)) {
        probe_printer();
        return;
    }

    if (g_app.active_tab == TAB_DETAILS && !g_app.daemon_running && lo.start_btn.contains(mx, my)) {
        char err[128] = {};
        if (ensure_daemon_running(err, sizeof(err))) {
            refresh_state(true);
            set_status("Print spooler started");
        } else {
            set_status(err[0] ? err : "Failed to start print spooler");
        }
        return;
    }

    if (g_app.active_tab == TAB_DETAILS && lo.refresh_btn.contains(mx, my)) {
        refresh_state(true);
        set_status("Printer status refreshed");
        return;
    }

    if (g_app.active_tab == TAB_OVERVIEW && g_app.printer_uri[0] && lo.test_btn.contains(mx, my)) {
        char job_id[48] = {};
        char err[128] = {};
        if (submit_test_page_job(nullptr, job_id, sizeof(job_id), err, sizeof(err))) {
            refresh_state(true);
            set_status("Test page queued");
        } else {
            set_status(err[0] ? err : "Failed to queue test page");
        }
        return;
    }
}

static void cycle_tab(int delta) {
    int next = g_app.active_tab + delta;
    while (next < 0) next += TAB_COUNT;
    while (next >= TAB_COUNT) next -= TAB_COUNT;
    g_app.active_tab = next;
}

static void handle_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return;

    if (g_app.config_open) {
        return;
    }

    if (key.scancode == 0x01) {
        g_win.closed = true;
        return;
    }

    if (key.scancode == 0x0F) {
        cycle_tab(key.shift ? -1 : 1);
        return;
    }

    if (key.scancode == 0x4B) {
        cycle_tab(-1);
        return;
    }

    if (key.scancode == 0x4D) {
        cycle_tab(1);
        return;
    }

    if (key.ascii == 'r' || key.ascii == 'R') {
        refresh_state(true);
        set_status("Printer status refreshed");
    } else if (key.ascii == 'p' || key.ascii == 'P') {
        if (g_app.printer_uri[0]) probe_printer();
    } else if (key.ascii == 't' || key.ascii == 'T') {
        if (g_app.printer_uri[0]) {
            char job_id[48] = {};
            char err[128] = {};
            if (submit_test_page_job(nullptr, job_id, sizeof(job_id), err, sizeof(err))) {
                refresh_state(true);
                set_status("Test page queued");
            } else {
                set_status(err[0] ? err : "Failed to queue test page");
            }
        }
    }
}

extern "C" void _start() {
    if (!fonts::init())
        montauk::exit(1);

    g_app.active_tab = TAB_OVERVIEW;
    load_accent();
    load_icons();
    refresh_state(true);

    if (!g_win.create("Printers", INIT_W, INIT_H))
        montauk::exit(1);

    render();

    while (g_win.id >= 0 && !g_win.closed) {
        Montauk::WinEvent ev;
        bool redraw_config = false;
        bool redraw_main = false;

        if (g_app.config_open && g_config.open) {
            int cr = montauk::win_poll(g_config.win_id, &ev);
            if (cr < 0) {
                close_config_popup();
                redraw_main = true;
            } else if (cr > 0) {
                if (ev.type == 3) {
                    close_config_popup();
                    redraw_main = true;
                } else if (ev.type == 2 || ev.type == 4) {
                    if (ev.type == 2) {
                        g_config.width = ev.resize.w;
                        g_config.height = ev.resize.h;
                        g_config.pixels = (uint32_t*)(uintptr_t)montauk::win_resize(
                            g_config.win_id, ev.resize.w, ev.resize.h);
                    }
                    redraw_config = true;
                } else if (ev.type == 0) {
                    if (handle_config_key(ev.key))
                        redraw_main = true;
                    if (g_app.config_open)
                        redraw_config = true;
                } else if (ev.type == 1) {
                    if (handle_config_mouse(ev.mouse.x, ev.mouse.y,
                                            ev.mouse.buttons, ev.mouse.prev_buttons))
                        redraw_main = true;
                    if (g_app.config_open)
                        redraw_config = true;
                }
            }
        }

        int r = g_win.poll(&ev);
        if (r < 0) break;
        if (r == 0) {
            uint64_t last_refresh = g_app.last_refresh_ms;
            refresh_state(false);
            if (g_app.last_refresh_ms != last_refresh)
                redraw_main = true;

            if (redraw_main)
                render();
            if (redraw_config && g_app.config_open)
                render_config_popup();
            if (!redraw_main && !(redraw_config && g_app.config_open))
                montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) break;
        if (ev.type == 2 || ev.type == 4) redraw_main = true;

        if (ev.type == 0) {
            handle_key(ev.key);
            redraw_main = true;
        } else if (ev.type == 1) {
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
            if (clicked) {
                handle_click(ev.mouse.x, ev.mouse.y);
                redraw_main = true;
            }
        }

        if (redraw_main)
            render();
        if (redraw_config && g_app.config_open)
            render_config_popup();
    }

    close_config_popup();
    if (g_printer_icon.pixels) svg_free(g_printer_icon);
    g_win.destroy();
    montauk::exit(0);
}
