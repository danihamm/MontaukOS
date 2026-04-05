/*
    * printdialog.cpp
    * Print dialog implementation
    * Copyright (c) 2026 Daniel Hammer
*/

#include "printdialog.hpp"
#include "main.hpp"
#include <gui/svg.hpp>
#include <gui/canvas.hpp>
#include <print/print.hpp>

extern "C" {
#include <stdio.h>
}

namespace {

using namespace gui;

struct PrintDialogState {
    SvgIcon printer_icon;
    char source_path[256];
    char job_name[128];
    struct PrinterRow {
        char uri[256];
        char name[128];
        char status[128];
        char seen_at[32];
        bool ok;
        bool is_default;
    } printers[printdialog::MAX_DISCOVERED_PRINTERS];
    int printer_count;
    int selected_printer;
    uint32_t copies;
    int queue_count;
    int active_count;
    bool daemon_running;
    bool submit_mode;
    bool source_ready;
    char note[160];
    char error[160];
    int mouse_x;
    int mouse_y;
};

PrintDialogState g_print;

void safe_copy(char* dst, int dst_len, const char* src) {
    gui::dialogs::safe_copy(dst, dst_len, src);
}

bool path_exists_via_open(const char* path) {
    if (!path || !path[0]) return false;
    int fd = montauk::open(path);
    if (fd < 0) return false;
    montauk::close(fd);
    return true;
}

const char* path_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' && *(p + 1)) last = p + 1;
    }
    return last;
}

void fit_text_end(const char* src, char* out, int out_len, int max_px) {
    if (!src || !src[0]) {
        safe_copy(out, out_len, "");
        return;
    }
    if (text_width(src) <= max_px) {
        safe_copy(out, out_len, src);
        return;
    }

    int slen = montauk::slen(src);
    int keep = slen;
    while (keep > 1) {
        char candidate[256];
        candidate[0] = '.';
        candidate[1] = '.';
        int pos = 2;
        const char* tail = src + (slen - keep);
        while (*tail && pos < (int)sizeof(candidate) - 1)
            candidate[pos++] = *tail++;
        candidate[pos] = '\0';
        if (text_width(candidate) <= max_px) {
            safe_copy(out, out_len, candidate);
            return;
        }
        keep--;
    }

    safe_copy(out, out_len, src);
}

void draw_action_button(Canvas& c, const Rect& rect, const char* label, bool enabled, bool hovered, bool primary) {
    Color bg = primary ? colors::ACCENT : Color::from_rgb(0xE8, 0xE8, 0xE8);
    Color fg = primary ? colors::WHITE : colors::TEXT_COLOR;
    if (!enabled) {
        bg = Color::from_rgb(0xF0, 0xF0, 0xF0);
        fg = Color::from_rgb(0x9A, 0x9A, 0x9A);
    } else if (hovered) {
        bg = primary ? Color::from_rgb(0x2B, 0x6B, 0xE0) : Color::from_rgb(0xDC, 0xDC, 0xDC);
    }
    c.fill_rounded_rect(rect.x, rect.y, rect.w, rect.h, 4, bg);
    int tx = rect.x + (rect.w - text_width(label)) / 2;
    int ty = rect.y + (rect.h - system_font_height()) / 2;
    c.text(tx, ty, label, fg);
}

PrintDialogLayout print_layout() {
    PrintDialogLayout lo = {};
    lo.header_rect = {0, 0, 0, 0};
    lo.printers_rect = {16, 16, 212, g_app.win.height - 94};
    lo.detail_rect = {244, 16, g_app.win.width - 260, g_app.win.height - 94};
    int copy_y = lo.detail_rect.y + 202;
    lo.copy_minus_btn = {lo.detail_rect.x + 16, copy_y, 28, 28};
    lo.copy_value_rect = {lo.copy_minus_btn.x + 36, copy_y, 104, 28};
    lo.copy_plus_btn = {lo.copy_value_rect.x + lo.copy_value_rect.w + 8, copy_y, 28, 28};
    int btn_y = g_app.win.height - printdialog::BUTTON_H - 16;
    lo.confirm_btn = {g_app.win.width - printdialog::BUTTON_W - 16, btn_y, printdialog::BUTTON_W, printdialog::BUTTON_H};
    lo.cancel_btn = {lo.confirm_btn.x - printdialog::BUTTON_W - 8, btn_y, printdialog::BUTTON_W, printdialog::BUTTON_H};
    lo.refresh_btn = {16, btn_y, printdialog::BUTTON_W, printdialog::BUTTON_H};
    lo.printers_btn = {lo.refresh_btn.x + printdialog::BUTTON_W + 8, btn_y, 112, printdialog::BUTTON_H};
    return lo;
}

void print_set_error(PrintDialogState* st, const char* msg) {
    safe_copy(st->error, sizeof(st->error), msg);
}

void print_adjust_copies(PrintDialogState* st, int delta) {
    if (!st) return;
    int next = (int)st->copies + delta;
    if (next < 1) next = 1;
    if (next > 99) next = 99;
    st->copies = (uint32_t)next;
}

void print_update_note(PrintDialogState* st) {
    if (st->printer_count <= 0) {
        safe_copy(st->note, sizeof(st->note), "No printers discovered from the userspace print spooler yet.");
        return;
    }
    if (!st->daemon_running) {
        safe_copy(st->note, sizeof(st->note), "The print spooler is offline. Saved printers are still available.");
        return;
    }
    if (st->submit_mode) {
        safe_copy(st->note, sizeof(st->note), "The selected document will be handed to the userspace print spooler.");
    } else {
        safe_copy(st->note, sizeof(st->note), "This app stores printer settings only for now. Document output wiring will be added later.");
    }
}

int print_find_printer(const PrintDialogState* st, const char* uri) {
    if (!st || !uri || !uri[0]) return -1;
    for (int i = 0; i < st->printer_count; i++) {
        if (montauk::streq(st->printers[i].uri, uri)) return i;
    }
    return -1;
}

void print_add_printer(PrintDialogState* st,
                       const char* uri,
                       const char* name,
                       const char* status,
                       const char* seen_at,
                       bool ok,
                       bool is_default) {
    if (!st || !uri || !uri[0]) return;

    int idx = print_find_printer(st, uri);
    if (idx < 0) {
        if (st->printer_count >= printdialog::MAX_DISCOVERED_PRINTERS) return;
        idx = st->printer_count++;
        montauk::memset(&st->printers[idx], 0, sizeof(st->printers[idx]));
        safe_copy(st->printers[idx].uri, sizeof(st->printers[idx].uri), uri);
    }

    if (name && name[0]) safe_copy(st->printers[idx].name, sizeof(st->printers[idx].name), name);
    if (status && status[0]) safe_copy(st->printers[idx].status, sizeof(st->printers[idx].status), status);
    if (seen_at && seen_at[0]) safe_copy(st->printers[idx].seen_at, sizeof(st->printers[idx].seen_at), seen_at);
    st->printers[idx].ok = ok;
    st->printers[idx].is_default = is_default;
}

Rect print_row_rect(const PrintDialogLayout& lo, int idx) {
    return {lo.printers_rect.x + 8, lo.printers_rect.y + 28 + idx * 58, lo.printers_rect.w - 16, 52};
}

bool print_can_confirm(const PrintDialogState* st) {
    if (!st || st->selected_printer < 0 || st->selected_printer >= st->printer_count) return false;
    if (!st->submit_mode) return true;
    return st->source_ready;
}

int count_dir_entries(const char* path) {
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

void print_refresh(PrintDialogState* st) {
    if (!st) return;

    char preferred_uri[256] = {};
    if (st->selected_printer >= 0 && st->selected_printer < st->printer_count)
        safe_copy(preferred_uri, sizeof(preferred_uri), st->printers[st->selected_printer].uri);
    else if (g_app.request.printer_uri[0])
        safe_copy(preferred_uri, sizeof(preferred_uri), g_app.request.printer_uri);

    st->printer_count = 0;
    st->selected_printer = -1;
    st->error[0] = '\0';
    st->queue_count = count_dir_entries(print::SPOOL_QUEUE_DIR);
    st->active_count = count_dir_entries(print::SPOOL_ACTIVE_DIR);

    int pid = -1;
    st->daemon_running = print::daemon_is_running(&pid);

    print::KnownPrinter known[printdialog::MAX_DISCOVERED_PRINTERS];
    int known_count = print::list_known_printers(known, printdialog::MAX_DISCOVERED_PRINTERS);
    for (int i = 0; i < known_count; i++) {
        const char* status = known[i].status_message[0]
            ? known[i].status_message
            : (known[i].ok ? "Ready for queued jobs" : "Saved printer");
        print_add_printer(st, known[i].uri, known[i].display_name, status, known[i].last_seen, known[i].ok, known[i].is_default);
    }

    if (st->printer_count <= 0) {
        char default_uri[256] = {};
        bool have_default = print::read_default_printer_uri(default_uri, sizeof(default_uri));
        print::ProbeState probe = {};
        bool have_probe = print::load_printer_probe_state(&probe);

        if (have_probe && probe.printer_uri[0]) {
            const char* status = probe.message[0] ? probe.message
                : (probe.caps.status_message[0] ? probe.caps.status_message
                   : (probe.ok ? "Ready for queued jobs" : "Saved printer"));
            print_add_printer(st, probe.printer_uri, probe.caps.printer_name, status, probe.probed_at,
                              probe.ok, have_default && montauk::streq(default_uri, probe.printer_uri));
        } else if (have_default) {
            print_add_printer(st, default_uri, "Configured printer", "Saved by the print spooler", "", false, true);
        }
    }

    if (preferred_uri[0]) st->selected_printer = print_find_printer(st, preferred_uri);
    if (st->selected_printer < 0) {
        for (int i = 0; i < st->printer_count; i++) {
            if (st->printers[i].is_default) {
                st->selected_printer = i;
                break;
            }
        }
    }
    if (st->selected_printer < 0 && st->printer_count > 0) st->selected_printer = 0;

    st->source_ready = !st->submit_mode || (st->source_path[0] && path_exists_via_open(st->source_path));
    print_update_note(st);
}

void init_print_dialog(const gui::dialogs::Request& req) {
    PrintDialogState* st = &g_print;
    montauk::memset(st, 0, sizeof(*st));
    Color def = colors::ICON_COLOR;
    st->printer_icon = svg_load("0:/icons/printer-symbolic.svg", 24, 24, def);
    safe_copy(st->source_path, sizeof(st->source_path), req.source_path);
    safe_copy(st->job_name, sizeof(st->job_name), req.job_name);
    st->submit_mode = req.mode == gui::dialogs::PRINT_DIALOG_SUBMIT;
    st->copies = req.copies > 0 ? req.copies : 1;
    print_refresh(st);
}

void draw_print_dialog() {
    PrintDialogState* st = &g_print;
    Canvas c = g_app.win.canvas();
    c.fill(colors::WINDOW_BG);
    PrintDialogLayout lo = print_layout();
    const auto* selected = (st->selected_printer >= 0 && st->selected_printer < st->printer_count)
        ? &st->printers[st->selected_printer]
        : nullptr;

    c.fill_rounded_rect(lo.printers_rect.x, lo.printers_rect.y, lo.printers_rect.w, lo.printers_rect.h, 6, colors::WHITE);
    c.rect(lo.printers_rect.x, lo.printers_rect.y, lo.printers_rect.w, lo.printers_rect.h, colors::BORDER);
    c.text(lo.printers_rect.x + 12, lo.printers_rect.y + 8, "Printers", Color::from_rgb(0x77, 0x77, 0x77));

    if (st->printer_count <= 0) {
        c.text(lo.printers_rect.x + 12, lo.printers_rect.y + 38, "No printers discovered", colors::TEXT_COLOR);
        c.text(lo.printers_rect.x + 12, lo.printers_rect.y + 58, "Open Printers to configure one.", Color::from_rgb(0x77, 0x77, 0x77));
    } else {
        for (int i = 0; i < st->printer_count; i++) {
            Rect row = print_row_rect(lo, i);
            bool sel = i == st->selected_printer;
            bool hovered = row.contains(st->mouse_x, st->mouse_y);
            Color bg = sel ? colors::MENU_HOVER
                                : (hovered ? Color::from_rgb(0xF6, 0xF6, 0xF6) : colors::WHITE);
            c.fill_rounded_rect(row.x, row.y, row.w, row.h, 6, bg);
            c.rect(row.x, row.y, row.w, row.h, sel ? colors::ACCENT : Color::from_rgb(0xE4, 0xE4, 0xE4));
            if (st->printer_icon.pixels) c.icon(row.x + 10, row.y + 14, st->printer_icon);

            char display[128];
            if (st->printers[i].is_default && st->printers[i].name[0])
                snprintf(display, sizeof(display), "%s (Default)", st->printers[i].name);
            else if (st->printers[i].is_default)
                snprintf(display, sizeof(display), "Default printer");
            else
                safe_copy(display, sizeof(display), st->printers[i].name[0] ? st->printers[i].name : st->printers[i].uri);

            char title[112];
            fit_text_end(display, title, sizeof(title), row.w - 68);
            c.text(row.x + 42, row.y + 9, title, colors::TEXT_COLOR);

            char subtitle[256];
            safe_copy(subtitle, sizeof(subtitle), st->printers[i].uri[0] ? st->printers[i].uri : "Saved printer");
            fit_text_end(subtitle, title, sizeof(title), row.w - 68);
            c.text(row.x + 42, row.y + 28, title, Color::from_rgb(0x77, 0x77, 0x77));
        }
    }

    c.fill_rounded_rect(lo.detail_rect.x, lo.detail_rect.y, lo.detail_rect.w, lo.detail_rect.h, 6, colors::WHITE);
    c.rect(lo.detail_rect.x, lo.detail_rect.y, lo.detail_rect.w, lo.detail_rect.h, colors::BORDER);

    int dx = lo.detail_rect.x + 16;
    int dy = lo.detail_rect.y + 18;
    c.text(dx, dy, "Document", Color::from_rgb(0x77, 0x77, 0x77));
    char line[256];
    const char* job_label = st->job_name[0] ? st->job_name
        : (st->source_path[0] ? path_basename(st->source_path) : "Untitled document");
    fit_text_end(job_label, line, sizeof(line), lo.detail_rect.w - 32);
    c.text(dx, dy + 20, line, colors::TEXT_COLOR);

    dy += 68;
    c.text(dx, dy, "Printer", Color::from_rgb(0x77, 0x77, 0x77));
    if (selected) {
        const char* title = selected->name[0] ? selected->name : selected->uri;
        fit_text_end(title, line, sizeof(line), lo.detail_rect.w - 32);
        c.text(dx, dy + 18, line, colors::TEXT_COLOR);

        const char* subtitle = selected->uri[0] ? selected->uri : selected->status;
        fit_text_end(subtitle, line, sizeof(line), lo.detail_rect.w - 32);
        c.text(dx, dy + 36, line, Color::from_rgb(0x77, 0x77, 0x77));
    } else {
        c.text(dx, dy + 18, "No printer selected", Color::from_rgb(0x77, 0x77, 0x77));
    }

    dy += 86;
    c.text(dx, dy, "Copies", Color::from_rgb(0x77, 0x77, 0x77));
    draw_action_button(c, {lo.copy_minus_btn.x, lo.copy_minus_btn.y, lo.copy_minus_btn.w, lo.copy_minus_btn.h}, "-", st->copies > 1,
                       lo.copy_minus_btn.contains(st->mouse_x, st->mouse_y), false);
    c.fill_rounded_rect(lo.copy_value_rect.x, lo.copy_value_rect.y, lo.copy_value_rect.w, lo.copy_value_rect.h, 4, Color::from_rgb(0xF6, 0xF6, 0xF6));
    c.rect(lo.copy_value_rect.x, lo.copy_value_rect.y, lo.copy_value_rect.w, lo.copy_value_rect.h, colors::BORDER);
    snprintf(line, sizeof(line), "%u %s", (unsigned)st->copies, st->copies == 1 ? "copy" : "copies");
    c.text(lo.copy_value_rect.x + 10, lo.copy_value_rect.y + 7, line, colors::TEXT_COLOR);
    draw_action_button(c, {lo.copy_plus_btn.x, lo.copy_plus_btn.y, lo.copy_plus_btn.w, lo.copy_plus_btn.h}, "+", st->copies < 99,
                       lo.copy_plus_btn.contains(st->mouse_x, st->mouse_y), false);

    if (st->error[0]) {
        char line[160];
        fit_text_end(st->error, line, sizeof(line), lo.cancel_btn.x - 24);
        c.text(16, lo.cancel_btn.y - 22, line, Color::from_rgb(0xC0, 0x33, 0x33));
    }

    draw_action_button(c, {lo.refresh_btn.x, lo.refresh_btn.y, lo.refresh_btn.w, lo.refresh_btn.h}, "Refresh", true, lo.refresh_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, {lo.printers_btn.x, lo.printers_btn.y, lo.printers_btn.w, lo.printers_btn.h}, "Printers", true, lo.printers_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, {lo.cancel_btn.x, lo.cancel_btn.y, lo.cancel_btn.w, lo.cancel_btn.h}, "Cancel", true, lo.cancel_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, {lo.confirm_btn.x, lo.confirm_btn.y, lo.confirm_btn.w, lo.confirm_btn.h}, "Print", print_can_confirm(st),
                       lo.confirm_btn.contains(st->mouse_x, st->mouse_y), true);
}

void print_finish_setup(PrintDialogState* st) {
    if (!st || st->selected_printer < 0 || st->selected_printer >= st->printer_count) {
        print_set_error(st, "Choose a printer to continue");
        return;
    }

    const auto& printer = st->printers[st->selected_printer];
    dialog_finish("ok", "", "", "Printer selected",
                  printer.uri, printer.name, st->copies);
}

void print_submit(PrintDialogState* st) {
    if (!st || st->selected_printer < 0 || st->selected_printer >= st->printer_count) {
        print_set_error(st, "Choose a printer to print");
        return;
    }
    if (!st->source_ready) {
        print_set_error(st, "Source document is unavailable");
        return;
    }

    const auto& printer = st->printers[st->selected_printer];
    char job_id[64] = {};
    char err[160] = {};
    const char* job_name = st->job_name[0] ? st->job_name : path_basename(st->source_path);
    if (!print::submit_document_job(st->source_path, printer.uri, job_name,
                                    job_id, sizeof(job_id), err, sizeof(err),
                                    (int)(st->copies > 0 ? st->copies : 1))) {
        print_set_error(st, err[0] ? err : "Failed to queue print job");
        return;
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "Queued as %s", job_id);
    dialog_finish("ok", "", job_id, msg, printer.uri, printer.name, st->copies);
}

void handle_print_mouse(const Montauk::WinEvent& ev) {
    PrintDialogState* st = &g_print;
    st->mouse_x = ev.mouse.x;
    st->mouse_y = ev.mouse.y;

    MouseEvent me = {ev.mouse.x, ev.mouse.y, ev.mouse.buttons, ev.mouse.prev_buttons, ev.mouse.scroll};
    if (!me.left_pressed()) return;

    PrintDialogLayout lo = print_layout();
    if (lo.cancel_btn.contains(me.x, me.y)) { dialog_cancel(); return; }
    if (lo.refresh_btn.contains(me.x, me.y)) { print_refresh(st); return; }
    if (lo.printers_btn.contains(me.x, me.y)) { montauk::spawn("0:/apps/printers/printers.elf"); return; }
    if (lo.copy_minus_btn.contains(me.x, me.y) && st->copies > 1) {
        print_adjust_copies(st, -1);
        st->error[0] = '\0';
        return;
    }
    if (lo.copy_plus_btn.contains(me.x, me.y) && st->copies < 99) {
        print_adjust_copies(st, +1);
        st->error[0] = '\0';
        return;
    }
    for (int i = 0; i < st->printer_count; i++) {
        Rect row = print_row_rect(lo, i);
        if (row.contains(me.x, me.y)) {
            st->selected_printer = i;
            st->error[0] = '\0';
            return;
        }
    }
    if (lo.confirm_btn.contains(me.x, me.y) && print_can_confirm(st)) {
        if (st->submit_mode) print_submit(st);
        else print_finish_setup(st);
    }
}

void handle_print_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return;
    if (key.scancode == 0x01) {
        dialog_cancel();
        return;
    }
    if ((key.ascii == '\n' || key.ascii == '\r') && print_can_confirm(&g_print)) {
        if (g_print.submit_mode) print_submit(&g_print);
        else print_finish_setup(&g_print);
    }
    if (key.ctrl && (key.ascii == 'r' || key.ascii == 'R')) {
        print_refresh(&g_print);
        return;
    }
    if ((key.ascii == '+' || key.ascii == '=') && g_print.copies < 99) {
        print_adjust_copies(&g_print, +1);
        g_print.error[0] = '\0';
        return;
    }
    if ((key.ascii == '-' || key.ascii == '_') && g_print.copies > 1) {
        print_adjust_copies(&g_print, -1);
        g_print.error[0] = '\0';
        return;
    }
    if ((key.scancode == 0x48 || key.scancode == 0x4B) && g_print.printer_count > 0) {
        if (g_print.selected_printer > 0) g_print.selected_printer--;
        else g_print.selected_printer = 0;
        g_print.error[0] = '\0';
        return;
    }
    if ((key.scancode == 0x50 || key.scancode == 0x4D) && g_print.printer_count > 0) {
        if (g_print.selected_printer < g_print.printer_count - 1) g_print.selected_printer++;
        else g_print.selected_printer = g_print.printer_count - 1;
        g_print.error[0] = '\0';
    }
}

} // namespace

namespace printdialog {

void init(const gui::dialogs::Request& req) {
    init_print_dialog(req);
}

void draw() {
    draw_print_dialog();
}

void handle_mouse(const Montauk::WinEvent& ev) {
    handle_print_mouse(ev);
}

void handle_key(const Montauk::KeyEvent& key) {
    handle_print_key(key);
}

void cleanup() {
    if (g_print.printer_icon.pixels) {
        svg_free(g_print.printer_icon);
        g_print.printer_icon.pixels = nullptr;
    }
}

} // namespace printdialog