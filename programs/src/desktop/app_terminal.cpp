/*
    * app_terminal.cpp
    * MontaukOS Desktop - Terminal application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Terminal callbacks
// ============================================================================

static void terminal_on_draw(Window* win, Framebuffer& fb) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (!ts) return;

    Rect cr = win->content_rect();

    // Check if window was resized and update terminal grid
    int new_cols = cr.w / mono_cell_width();
    int new_rows = cr.h / mono_cell_height();
    if (new_cols != ts->cols || new_rows != ts->rows) {
        terminal_resize(ts, new_cols, new_rows);
    }

    terminal_render(ts, win->content, cr.w, cr.h);
}

static void terminal_on_mouse(Window* win, MouseEvent& ev) {
    // Terminal doesn't need mouse handling for now
}

static void terminal_on_key(Window* win, const Montauk::KeyEvent& key) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (!ts) return;
    terminal_handle_key(ts, key);
}

static void terminal_on_close(Window* win) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (ts) {
        if (ts->cells) montauk::mfree(ts->cells);
        montauk::mfree(ts);
        win->app_data = nullptr;
    }
}

static void terminal_on_poll(Window* win) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (ts) terminal_poll(ts);
}

// ============================================================================
// Terminal launcher
// ============================================================================

void open_terminal(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Terminal", 200, 80, 648, 480);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    Rect cr = win->content_rect();
    int cols = cr.w / mono_cell_width();
    int rows = cr.h / mono_cell_height();

    TerminalState* ts = (TerminalState*)montauk::malloc(sizeof(TerminalState));
    montauk::memset(ts, 0, sizeof(TerminalState));
    terminal_init(ts, cols, rows);

    win->app_data = ts;
    win->on_draw = terminal_on_draw;
    win->on_mouse = terminal_on_mouse;
    win->on_key = terminal_on_key;
    win->on_close = terminal_on_close;
    win->on_poll = terminal_on_poll;
}
