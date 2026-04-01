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
static constexpr int PAD_X = 16;
static constexpr int PAD_Y = 20;
static constexpr int BTN_H = 30;
static constexpr int ACTION_RADIUS = 4;
static constexpr int FIELD_H = 32;

struct AppState {
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
    char probe_detail[MAX_DEBUG_LEN];

    char status_msg[128];
    uint64_t status_time;

    bool config_open;
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

    Rect dialog;
    Rect dialog_input;
    Rect dialog_save;
    Rect dialog_cancel;
};

static WsWindow g_win;
static AppState g_app = {};

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
    g_app.probe_detail[0] = '\0';
}

static void set_probe_transport_detail(const IppUri& uri, const char* resolution) {
    snprintf(g_app.probe_detail, sizeof(g_app.probe_detail),
             "host=%s ip=%s port=%u path=%s %s",
             uri.host,
             resolution && *resolution ? resolution : "unresolved",
             (unsigned)uri.port,
             uri.path[0] ? uri.path : "/",
             uri.use_tls ? "tls" : "plain");
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
    int y = PAD_Y;

    if (status_visible()) y += sfh + 12;

    lo->configure_btn = {g_win.width - x - 112 - 8 - 84, y, 112, BTN_H};
    lo->clear_btn = {lo->configure_btn.x + 120, y, 84, BTN_H};

    y += BTN_H + 8;
    y += sfh + 4;
    y += sfh + 14;
    y += 16;

    lo->probe_btn = {g_win.width - x - 92, y, 92, BTN_H};

    y += BTN_H + 8;
    y += sfh + 4;
    y += sfh + 4;
    y += sfh + 4;
    if (g_app.probe_valid && g_app.probe_ok && g_app.probe_detail[0]) y += sfh + 4;
    y += sfh + 14;
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

    lo->test_btn = {g_win.width - x - 132, y, 132, BTN_H};

    int dw = 408;
    int dh = 220;
    lo->dialog = {(g_win.width - dw) / 2, (g_win.height - dh) / 2, dw, dh};
    lo->dialog_input = {lo->dialog.x + 16, lo->dialog.y + 58, dw - 32, FIELD_H};
    lo->dialog_save = {lo->dialog.x + 16, lo->dialog.y + dh - BTN_H - 16, 80, BTN_H};
    lo->dialog_cancel = {lo->dialog.x + 104, lo->dialog.y + dh - BTN_H - 16, 80, BTN_H};
}

static void refresh_state(bool force = false) {
    uint64_t now = montauk::get_milliseconds();
    if (!force && g_app.printers_loaded && now - g_app.last_refresh_ms < 1000) return;

    char previous_uri[MAX_PATH_LEN];
    safe_copy(previous_uri, sizeof(previous_uri), g_app.printer_uri);

    ensure_spool_dirs();

    if (!read_default_printer_uri(g_app.printer_uri, sizeof(g_app.printer_uri)))
        g_app.printer_uri[0] = '\0';

    if (!montauk::streq(previous_uri, g_app.printer_uri))
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
    clear_probe();

    if (!g_app.printer_uri[0]) {
        safe_copy(g_app.probe_message, sizeof(g_app.probe_message), "No printer configured");
        g_app.probe_valid = true;
        g_app.probe_ok = false;
        set_status("No printer configured");
        return;
    }

    char err[128] = {};
    IppUri normalized = {};
    if (!normalize_ipp_uri(g_app.printer_uri, &normalized, err, sizeof(err))) {
        safe_copy(g_app.probe_message, sizeof(g_app.probe_message), err);
        g_app.probe_valid = true;
        g_app.probe_ok = false;
        set_status("Printer probe failed");
        return;
    }

    uint32_t ip = 0;
    if (!resolve_host(normalized.host, &ip)) {
        set_probe_transport_detail(normalized, "unresolved");
        if (host_looks_like_mdns(normalized.host))
            safe_copy(g_app.probe_message, sizeof(g_app.probe_message),
                      ".local/mDNS hostnames are not supported yet");
        else
            snprintf(g_app.probe_message, sizeof(g_app.probe_message),
                     "Could not resolve %s", normalized.host);
        g_app.probe_valid = true;
        g_app.probe_ok = false;
        set_status("Printer probe failed");
        return;
    }

    char ip_text[20];
    format_ipv4(ip_text, sizeof(ip_text), ip);
    set_probe_transport_detail(normalized, ip_text);

    if (!ipp_get_printer_capabilities(normalized.normalized, &g_app.probe_caps, err, sizeof(err))) {
        safe_copy(g_app.probe_message, sizeof(g_app.probe_message), err[0] ? err : "IPP probe failed");
        summarize_ipp_capabilities(&g_app.probe_caps, g_app.probe_detail, sizeof(g_app.probe_detail));
        g_app.probe_valid = true;
        g_app.probe_ok = false;
        set_status("Printer probe failed");
        return;
    }

    safe_copy(g_app.probe_message, sizeof(g_app.probe_message), "Printer probe succeeded");
    summarize_ipp_capabilities(&g_app.probe_caps, g_app.probe_detail, sizeof(g_app.probe_detail));
    g_app.probe_valid = true;
    g_app.probe_ok = true;
    set_status("Printer probe succeeded");
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

    g_app.config_open = false;
    refresh_state(true);
    set_status("Printer saved");
}

static void draw_modal(Canvas& c, const Layout& lo) {
    c.fill_rounded_rect(lo.dialog.x, lo.dialog.y, lo.dialog.w, lo.dialog.h, 6, colors::WINDOW_BG);
    c.rect(lo.dialog.x, lo.dialog.y, lo.dialog.w, lo.dialog.h, colors::BORDER);

    int sfh = system_font_height();
    int y = lo.dialog.y + 16;
    c.text(lo.dialog.x + 16, y, "Default Printer", colors::TEXT_COLOR);
    y += sfh + 6;
    c.text(lo.dialog.x + 16, y, "Enter an IPP or IPPS destination URI.", Color::from_rgb(0x88, 0x88, 0x88));
    y += sfh + 10;

    draw_input_field(c, lo.dialog_input.x, lo.dialog_input.y - (sfh + 2),
                     lo.dialog_input.w, lo.dialog_input.h, "Printer URI",
                     g_app.edit_uri, true);

    y = lo.dialog_input.y + lo.dialog_input.h + 12;
    c.text(lo.dialog.x + 16, y, "Example: ipp://printer.local/ipp/print", Color::from_rgb(0x88, 0x88, 0x88));
    y += sfh + 10;

    if (g_app.edit_error[0])
        c.text(lo.dialog.x + 16, y, g_app.edit_error, Color::from_rgb(0xD0, 0x3E, 0x3E));

    draw_action_btn(c, lo.dialog_save, "Save", true, true);
    draw_action_btn(c, lo.dialog_cancel, "Cancel", true, false);
}

static void render() {
    refresh_state();

    Canvas c = g_win.canvas();
    c.fill(colors::WINDOW_BG);

    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    Color success = Color::from_rgb(0x22, 0x88, 0x22);
    Color danger = Color::from_rgb(0xCC, 0x33, 0x33);
    int sfh = system_font_height();
    int x = PAD_X;
    int y = PAD_Y;
    int w = g_win.width - 2 * x;
    Layout lo = {};
    compute_layout(&lo);

    if (status_visible()) {
        c.text(x, y, g_app.status_msg, g_app.accent);
        y += sfh + 12;
    }

    // Default printer
    c.text(x, y + 6, "Default Printer", colors::TEXT_COLOR);
    draw_action_btn(c, lo.configure_btn, "Configure", true, true);
    draw_action_btn(c, lo.clear_btn, "Clear", g_app.printer_uri[0] != '\0', false);
    y += BTN_H + 8;

    char line[MAX_DEBUG_LEN];
    if (g_app.printer_uri[0]) text_fit(g_app.printer_uri, line, sizeof(line), w);
    else safe_copy(line, sizeof(line), "No printer configured");
    c.text(x, y, line, g_app.printer_uri[0] ? colors::TEXT_COLOR : dim);
    y += sfh + 4;
    c.text(x, y, "System-wide IPP / IPPS destination", dim);
    y += sfh + 14;
    c.hline(x, y, w, colors::BORDER);
    y += 16;

    // Connection
    c.text(x, y + 6, "Connection", colors::TEXT_COLOR);
    draw_action_btn(c, lo.probe_btn, "Probe", g_app.printer_uri[0] != '\0', false);
    y += BTN_H + 8;

    if (g_app.printer_uri[0]) {
        IppUri normalized = {};
        char err[128] = {};
        if (normalize_ipp_uri(g_app.printer_uri, &normalized, err, sizeof(err))) {
            snprintf(line, sizeof(line), "Host: %s", normalized.host);
            c.text(x, y, line, colors::TEXT_COLOR);
            y += sfh + 4;

            uint32_t ip = 0;
            if (resolve_host(normalized.host, &ip)) {
                char ip_text[20];
                format_ipv4(ip_text, sizeof(ip_text), ip);
                snprintf(line, sizeof(line), "Resolved: %s", ip_text);
            } else if (host_looks_like_mdns(normalized.host)) {
                safe_copy(line, sizeof(line), "Resolved: unavailable (.local/mDNS not supported yet)");
            } else {
                safe_copy(line, sizeof(line), "Resolved: unavailable");
            }
            text_fit(line, line, sizeof(line), w);
            c.text(x, y, line, dim);
        } else {
            safe_copy(line, sizeof(line), "Host: invalid printer URI");
            c.text(x, y, line, danger);
            y += sfh + 4;
            text_fit(err, line, sizeof(line), w);
            c.text(x, y, line, dim);
        }
    } else {
        c.text(x, y, "Host: not configured", dim);
        y += sfh + 4;
        c.text(x, y, "Resolved: unavailable", dim);
    }
    y += sfh + 4;

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
        } else if (g_app.probe_detail[0]) {
            safe_copy(line, sizeof(line), g_app.probe_detail);
        } else {
            safe_copy(line, sizeof(line), "Run Probe to query printer capabilities.");
        }
    } else {
        safe_copy(line, sizeof(line), "Probe: not run");
        c.text(x, y, line, dim);
        y += sfh + 4;
        safe_copy(line, sizeof(line), "Run Probe to query printer capabilities.");
    }
    text_fit(line, line, sizeof(line), w);
    c.text(x, y, line, dim);
    if (g_app.probe_valid && g_app.probe_ok && g_app.probe_detail[0]) {
        y += sfh + 4;
        text_fit(g_app.probe_detail, line, sizeof(line), w);
        c.text(x, y, line, dim);
    }
    y += sfh + 14;
    c.hline(x, y, w, colors::BORDER);
    y += 16;

    // Spooler
    c.text(x, y + 6, "Spooler", colors::TEXT_COLOR);
    draw_action_btn(c, lo.start_btn, "Start Daemon", !g_app.daemon_running, false);
    draw_action_btn(c, lo.refresh_btn, "Refresh", true, false);

    const char* daemon_label = g_app.daemon_running ? "Running" : "Stopped";
    int chip_w = text_width(daemon_label) + 18;
    int chip_x = lo.start_btn.x - chip_w - 12;
    if (chip_x < x + 70) chip_x = x + 70;
    c.fill_rounded_rect(chip_x, y + 2, chip_w, sfh + 8, (sfh + 8) / 2,
                        g_app.daemon_running ? success : danger);
    c.text(chip_x + 9, y + 6, daemon_label, colors::WHITE);
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

    y += 10;
    c.hline(x, y, w, colors::BORDER);
    y += 16;

    // Test page
    c.text(x, y + 6, "Test Page", colors::TEXT_COLOR);
    draw_action_btn(c, lo.test_btn, "Print Test Page", g_app.printer_uri[0] != '\0', true);
    y += BTN_H + 8;
    c.text(x, y, "Queue a simple test page through the print spooler.", dim);
    y += sfh + 4;
    c.text(x, y,
           g_app.printer_uri[0] ? "Uses the configured default printer." : "Configure a printer first.",
           g_app.printer_uri[0] ? colors::TEXT_COLOR : dim);

    if (g_app.config_open)
        draw_modal(c, lo);

    g_win.present();
}

static void handle_click(int mx, int my) {
    Layout lo = {};
    compute_layout(&lo);

    if (g_app.config_open) {
        if (lo.dialog_save.contains(mx, my)) {
            save_configured_printer();
        } else if (lo.dialog_cancel.contains(mx, my)) {
            g_app.config_open = false;
        }
        return;
    }

    if (lo.configure_btn.contains(mx, my)) {
        safe_copy(g_app.edit_uri, sizeof(g_app.edit_uri), g_app.printer_uri);
        g_app.edit_len = montauk::slen(g_app.edit_uri);
        g_app.edit_error[0] = '\0';
        g_app.config_open = true;
        return;
    }

    if (g_app.printer_uri[0] && lo.clear_btn.contains(mx, my)) {
        if (montauk::fdelete(DEFAULT_PRINTER_PATH) < 0) set_status("Failed to clear printer");
        else {
            refresh_state(true);
            set_status("Printer cleared");
        }
        return;
    }

    if (g_app.printer_uri[0] && lo.probe_btn.contains(mx, my)) {
        probe_printer();
        return;
    }

    if (!g_app.daemon_running && lo.start_btn.contains(mx, my)) {
        char err[128] = {};
        if (ensure_daemon_running(err, sizeof(err))) {
            refresh_state(true);
            set_status("Print spooler started");
        } else {
            set_status(err[0] ? err : "Failed to start print spooler");
        }
        return;
    }

    if (lo.refresh_btn.contains(mx, my)) {
        refresh_state(true);
        set_status("Printer status refreshed");
        return;
    }

    if (g_app.printer_uri[0] && lo.test_btn.contains(mx, my)) {
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

static void handle_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return;

    if (g_app.config_open) {
        if (key.scancode == 0x01) {
            g_app.config_open = false;
            return;
        }
        if (key.ascii == '\n' || key.ascii == '\r' || key.scancode == 0x1C) {
            save_configured_printer();
            return;
        }
        if (key.ascii == '\b' || key.scancode == 0x0E) {
            if (g_app.edit_len > 0) {
                g_app.edit_len--;
                g_app.edit_uri[g_app.edit_len] = '\0';
            }
            return;
        }
        if (key.ascii >= 0x20 && key.ascii < 0x7F && g_app.edit_len < MAX_PATH_LEN - 1) {
            g_app.edit_uri[g_app.edit_len++] = key.ascii;
            g_app.edit_uri[g_app.edit_len] = '\0';
        }
        return;
    }

    if (key.scancode == 0x01) {
        g_win.closed = true;
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

    load_accent();
    refresh_state(true);

    if (!g_win.create("Printers", INIT_W, INIT_H))
        montauk::exit(1);

    render();

    while (g_win.id >= 0 && !g_win.closed) {
        Montauk::WinEvent ev;
        int r = g_win.poll(&ev);
        if (r < 0) break;
        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        bool redraw = false;

        if (ev.type == 3) break;
        if (ev.type == 2 || ev.type == 4) redraw = true;

        if (ev.type == 0) {
            handle_key(ev.key);
            redraw = true;
        } else if (ev.type == 1) {
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
            if (clicked) {
                handle_click(ev.mouse.x, ev.mouse.y);
                redraw = true;
            }
        }

        if (redraw)
            render();
    }

    g_win.destroy();
    montauk::exit(0);
}
