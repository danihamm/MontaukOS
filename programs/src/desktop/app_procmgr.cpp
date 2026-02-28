/*
    * app_procmgr.cpp
    * MontaukOS Desktop - Process Manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Process Manager state
// ============================================================================

static constexpr int PM_TOOLBAR_H = 36;
static constexpr int PM_HEADER_H  = 24;
static constexpr int PM_ITEM_H    = 28;
static constexpr int PM_MAX_PROCS = 16;
static constexpr int PM_POLL_MS   = 1000;

struct ProcMgrState {
    DesktopState* desktop;
    Montauk::ProcInfo procs[PM_MAX_PROCS];
    int proc_count;
    int selected;          // selected row index (-1 = none)
    uint64_t last_poll_ms;
};

// ============================================================================
// Callbacks
// ============================================================================

static void procmgr_on_poll(Window* win) {
    ProcMgrState* pm = (ProcMgrState*)win->app_data;
    if (!pm) return;

    uint64_t now = montauk::get_milliseconds();
    if (now - pm->last_poll_ms < PM_POLL_MS) return;
    pm->last_poll_ms = now;

    // Remember previously selected PID
    int prev_pid = -1;
    if (pm->selected >= 0 && pm->selected < pm->proc_count) {
        prev_pid = pm->procs[pm->selected].pid;
    }

    pm->proc_count = montauk::proclist(pm->procs, PM_MAX_PROCS);

    // Restore selection by matching PID
    pm->selected = -1;
    if (prev_pid >= 0) {
        for (int i = 0; i < pm->proc_count; i++) {
            if (pm->procs[i].pid == prev_pid) {
                pm->selected = i;
                break;
            }
        }
    }
}

static void procmgr_on_draw(Window* win, Framebuffer& fb) {
    ProcMgrState* pm = (ProcMgrState*)win->app_data;
    if (!pm) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    int fh = system_font_height();

    // --- Toolbar ---
    c.fill_rect(0, 0, c.w, PM_TOOLBAR_H, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.hline(0, PM_TOOLBAR_H - 1, c.w, colors::BORDER);

    // "End Process" button
    int btn_w = 100;
    int btn_h = 26;
    int btn_x = 8;
    int btn_y = (PM_TOOLBAR_H - btn_h) / 2;
    Color btn_bg = (pm->selected >= 0 && pm->procs[pm->selected].pid != 0)
        ? Color::from_rgb(0xCC, 0x33, 0x33)
        : Color::from_rgb(0xAA, 0xAA, 0xAA);
    c.button(btn_x, btn_y, btn_w, btn_h, "End Process", btn_bg, colors::WHITE, 4);

    // Process count on right
    char count_str[24];
    snprintf(count_str, sizeof(count_str), "%d processes", pm->proc_count);
    int cw = text_width(count_str);
    c.text(c.w - cw - 12, (PM_TOOLBAR_H - fh) / 2, count_str, colors::TEXT_COLOR);

    // --- Header row ---
    int header_y = PM_TOOLBAR_H;
    c.fill_rect(0, header_y, c.w, PM_HEADER_H, Color::from_rgb(0xF0, 0xF0, 0xF0));

    int col_pid = 12;
    int col_name = 64;
    int col_mem = c.w - 120;
    int col_state = c.w - 52;

    int ty = header_y + (PM_HEADER_H - fh) / 2;
    c.text(col_pid, ty, "PID", Color::from_rgb(0x66, 0x66, 0x66));
    c.text(col_name, ty, "Name", Color::from_rgb(0x66, 0x66, 0x66));
    c.text(col_mem, ty, "Mem", Color::from_rgb(0x66, 0x66, 0x66));
    c.text(col_state, ty, "St", Color::from_rgb(0x66, 0x66, 0x66));

    c.hline(0, header_y + PM_HEADER_H - 1, c.w, colors::BORDER);

    // --- Process rows ---
    int list_y = PM_TOOLBAR_H + PM_HEADER_H;
    for (int i = 0; i < pm->proc_count; i++) {
        int row_y = list_y + i * PM_ITEM_H;
        if (row_y + PM_ITEM_H > c.h) break;

        // Selected row highlight
        if (i == pm->selected) {
            c.fill_rect(0, row_y, c.w, PM_ITEM_H, colors::MENU_HOVER);
        }

        int ry = row_y + (PM_ITEM_H - fh) / 2;

        // PID (right-aligned in PID column)
        char pid_str[8];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)pm->procs[i].pid);
        int pid_w = text_width(pid_str);
        c.text(col_pid + 32 - pid_w, ry, pid_str, colors::TEXT_COLOR);

        // Name (truncated to fit)
        c.text(col_name, ry, pm->procs[i].name, colors::TEXT_COLOR);

        // Memory
        char mem_str[16];
        format_size(mem_str, (int)pm->procs[i].heapUsed);
        c.text(col_mem, ry, mem_str, colors::TEXT_COLOR);

        // State
        const char* st_str;
        Color st_color;
        switch (pm->procs[i].state) {
        case 2: // Running
            st_str = "Run";
            st_color = Color::from_rgb(0x22, 0x88, 0x22);
            break;
        case 1: // Ready
            st_str = "Rdy";
            st_color = Color::from_rgb(0x33, 0x66, 0xCC);
            break;
        case 3: // Terminated
            st_str = "Term";
            st_color = Color::from_rgb(0xCC, 0x33, 0x33);
            break;
        default:
            st_str = "?";
            st_color = colors::TEXT_COLOR;
            break;
        }
        c.text(col_state, ry, st_str, st_color);
    }
}

static void procmgr_on_mouse(Window* win, MouseEvent& ev) {
    ProcMgrState* pm = (ProcMgrState*)win->app_data;
    if (!pm) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    if (ev.left_pressed()) {
        // Check "End Process" button click
        int btn_w = 100;
        int btn_h = 26;
        int btn_x = 8;
        int btn_y = (PM_TOOLBAR_H - btn_h) / 2;
        Rect btn_rect = {btn_x, btn_y, btn_w, btn_h};
        if (btn_rect.contains(lx, ly)) {
            if (pm->selected >= 0 && pm->selected < pm->proc_count
                && pm->procs[pm->selected].pid != 0) {
                montauk::kill(pm->procs[pm->selected].pid);
                pm->last_poll_ms = 0; // force refresh
            }
            return;
        }

        // Check row click
        int list_y = PM_TOOLBAR_H + PM_HEADER_H;
        if (ly >= list_y) {
            int row = (ly - list_y) / PM_ITEM_H;
            if (row >= 0 && row < pm->proc_count) {
                pm->selected = row;
            } else {
                pm->selected = -1;
            }
        }
    }
}

static void procmgr_on_key(Window* win, const Montauk::KeyEvent& key) {
    ProcMgrState* pm = (ProcMgrState*)win->app_data;
    if (!pm || !key.pressed) return;

    if (key.scancode == 0x48) { // Up arrow
        if (pm->selected > 0) pm->selected--;
        else if (pm->proc_count > 0) pm->selected = 0;
    } else if (key.scancode == 0x50) { // Down arrow
        if (pm->selected < pm->proc_count - 1) pm->selected++;
    } else if (key.scancode == 0x53) { // Delete key
        if (pm->selected >= 0 && pm->selected < pm->proc_count
            && pm->procs[pm->selected].pid != 0) {
            montauk::kill(pm->procs[pm->selected].pid);
            pm->last_poll_ms = 0; // force refresh
        }
    }
}

static void procmgr_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Process Manager launcher
// ============================================================================

void open_procmgr(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Processes", 180, 80, 520, 400);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];

    ProcMgrState* pm = (ProcMgrState*)montauk::malloc(sizeof(ProcMgrState));
    montauk::memset(pm, 0, sizeof(ProcMgrState));
    pm->desktop = ds;
    pm->selected = -1;
    pm->last_poll_ms = 0;

    // Initial poll
    pm->proc_count = montauk::proclist(pm->procs, PM_MAX_PROCS);

    win->app_data = pm;
    win->on_draw = procmgr_on_draw;
    win->on_mouse = procmgr_on_mouse;
    win->on_key = procmgr_on_key;
    win->on_close = procmgr_on_close;
    win->on_poll = procmgr_on_poll;
}
