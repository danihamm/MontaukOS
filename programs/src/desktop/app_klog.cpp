/*
    * app_klog.cpp
    * MontaukOS Desktop - Kernel Log viewer (tails the kernel ring buffer)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Kernel Log state
// ============================================================================

static constexpr int KLOG_READ_SIZE = 65536; // matches kernel ring buffer size
static constexpr int KLOG_POLL_MS   = 250;

struct KlogState {
    TerminalState term;
    char* klog_buf;
    int last_len;
    char last_tail_byte;
    uint64_t last_poll_ms;
};

// ============================================================================
// Re-feed the last screenful of log into the terminal
// ============================================================================

static void klog_refeed(KlogState* klog, int n) {
    // Find start of last `rows` lines from the end of the buffer
    int lines_needed = klog->term.rows;
    int start = 0;
    for (int i = n - 1; i >= 0 && lines_needed > 0; i--) {
        if (klog->klog_buf[i] == '\n') {
            lines_needed--;
            if (lines_needed == 0) { start = i + 1; break; }
        }
    }

    // Clear terminal
    int total = klog->term.cols * klog->term.rows;
    for (int i = 0; i < total; i++)
        klog->term.cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    klog->term.cursor_x = 0;
    klog->term.cursor_y = 0;
    klog->term.current_fg = colors::TERM_FG;
    klog->term.current_bg = colors::TERM_BG;

    terminal_feed(&klog->term, klog->klog_buf + start, n - start);
}

// ============================================================================
// Callbacks
// ============================================================================

static void klog_on_draw(Window* win, Framebuffer& fb) {
    KlogState* klog = (KlogState*)win->app_data;
    if (!klog) return;

    Rect cr = win->content_rect();
    terminal_render(&klog->term, win->content, cr.w, cr.h);
}

static void klog_on_mouse(Window* win, MouseEvent& ev) {
    // Read-only viewer — no mouse interaction needed
}

static void klog_on_key(Window* win, const Montauk::KeyEvent& key) {
    // Read-only viewer — no keyboard input
}

static void klog_on_poll(Window* win) {
    KlogState* klog = (KlogState*)win->app_data;
    if (!klog) return;

    uint64_t now = montauk::get_milliseconds();
    if (now - klog->last_poll_ms < KLOG_POLL_MS) return;
    klog->last_poll_ms = now;

    int n = (int)montauk::read_klog(klog->klog_buf, KLOG_READ_SIZE);
    if (n <= 0 && klog->last_len <= 0) return;

    if (n > klog->last_len) {
        // Buffer grew — feed only the new portion
        terminal_feed(&klog->term, klog->klog_buf + klog->last_len, n - klog->last_len);
    } else if (n == klog->last_len && n > 0) {
        // Same size — check if the tail changed (ring buffer wrapped)
        if (klog->klog_buf[n - 1] != klog->last_tail_byte) {
            klog_refeed(klog, n);
        } else {
            return; // nothing changed
        }
    } else if (n < klog->last_len) {
        // Unexpected shrink — full re-render
        klog_refeed(klog, n);
    }

    klog->last_len = n;
    if (n > 0) klog->last_tail_byte = klog->klog_buf[n - 1];
}

static void klog_on_close(Window* win) {
    KlogState* klog = (KlogState*)win->app_data;
    if (klog) {
        if (klog->term.cells) montauk::mfree(klog->term.cells);
        if (klog->term.alt_cells) montauk::mfree(klog->term.alt_cells);
        if (klog->klog_buf) montauk::mfree(klog->klog_buf);
        montauk::mfree(klog);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Kernel Log launcher
// ============================================================================

void open_klog(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Kernel Log", 160, 60, 720, 480);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    Rect cr = win->content_rect();
    int cols = cr.w / mono_cell_width();
    int rows = cr.h / mono_cell_height();

    KlogState* klog = (KlogState*)montauk::malloc(sizeof(KlogState));
    montauk::memset(klog, 0, sizeof(KlogState));

    // Initialize the terminal cell grid (reuse terminal infrastructure)
    terminal_init_cells(&klog->term, cols, rows);

    // Allocate klog read buffer
    klog->klog_buf = (char*)montauk::malloc(KLOG_READ_SIZE);
    klog->last_len = 0;
    klog->last_tail_byte = 0;
    klog->last_poll_ms = 0;

    // Do an initial read to show existing log content
    int n = (int)montauk::read_klog(klog->klog_buf, KLOG_READ_SIZE);
    if (n > 0) {
        klog_refeed(klog, n);
        klog->last_len = n;
        klog->last_tail_byte = klog->klog_buf[n - 1];
    }

    win->app_data = klog;
    win->on_draw = klog_on_draw;
    win->on_mouse = klog_on_mouse;
    win->on_key = klog_on_key;
    win->on_close = klog_on_close;
    win->on_poll = klog_on_poll;
}
