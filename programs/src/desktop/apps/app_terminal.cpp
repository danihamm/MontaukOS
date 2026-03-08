/*
    * app_terminal.cpp
    * MontaukOS Desktop
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Tab data model
// ============================================================================

static constexpr int TERM_TAB_BAR_H = 34;
static constexpr int TERM_MAX_TABS  = 8;
static constexpr int TERM_TAB_W     = 130;
static constexpr int TERM_TAB_GAP   = 4;
static constexpr int TERM_PLUS_W    = 28;
static constexpr int TERM_PLUS_PAD  = 8;

static constexpr int TERM_TAB_PAD  = 8;

struct TermTabState {
    TerminalState* tabs[TERM_MAX_TABS];
    int tab_count;
    int active_tab;
    int prev_active_tab;      // track tab switches to force redraw
    uint32_t* last_content;   // detect content buffer reallocation
    DesktopState* desktop;
};

// ============================================================================
// Tab helpers
// ============================================================================

static void term_free_tab(TerminalState* ts) {
    if (!ts) return;
    if (ts->child_pid > 0) montauk::kill(ts->child_pid);
    if (ts->cells) montauk::free(ts->cells);
    if (ts->alt_cells) montauk::free(ts->alt_cells);
    montauk::mfree(ts);
}

static TerminalState* term_create_tab(DesktopState* ds, int cols, int rows) {
    TerminalState* ts = (TerminalState*)montauk::malloc(sizeof(TerminalState));
    montauk::memset(ts, 0, sizeof(TerminalState));
    terminal_init(ts, cols, rows);
    ts->desktop = ds;
    return ts;
}

static void term_close_tab(TermTabState* tts, Window* win, int idx) {
    if (idx < 0 || idx >= tts->tab_count) return;

    term_free_tab(tts->tabs[idx]);

    // Shift remaining tabs left
    for (int i = idx; i < tts->tab_count - 1; i++)
        tts->tabs[i] = tts->tabs[i + 1];
    tts->tab_count--;
    tts->tabs[tts->tab_count] = nullptr;

    if (tts->tab_count == 0) {
        // Last tab closed — close the window
        DesktopState* ds = tts->desktop;
        if (ds) {
            for (int i = 0; i < ds->window_count; i++) {
                if (&ds->windows[i] == win) {
                    desktop_close_window(ds, i);
                    return;
                }
            }
        }
        return;
    }

    // Adjust active tab index
    if (idx == tts->active_tab) {
        // Closed the active tab: prefer left neighbor, fall back to right
        if (tts->active_tab > 0)
            tts->active_tab--;
        // else stays at 0 (right neighbor shifted here)
    } else if (tts->active_tab > idx) {
        tts->active_tab--;
    }
    if (tts->active_tab >= tts->tab_count)
        tts->active_tab = tts->tab_count - 1;

    // Mark new active tab dirty so it redraws
    tts->tabs[tts->active_tab]->dirty = true;
}

static void term_add_tab(TermTabState* tts, int cols, int rows) {
    if (tts->tab_count >= TERM_MAX_TABS) return;
    TerminalState* ts = term_create_tab(tts->desktop, cols, rows);
    tts->tabs[tts->tab_count] = ts;
    tts->active_tab = tts->tab_count;
    tts->tab_count++;
}

// ============================================================================
// Terminal callbacks
// ============================================================================

static void terminal_on_draw(Window* win, Framebuffer& fb) {
    TermTabState* tts = (TermTabState*)win->app_data;
    if (!tts || tts->tab_count == 0) return;
    if (tts->active_tab < 0 || tts->active_tab >= tts->tab_count)
        tts->active_tab = 0;

    Rect cr = win->content_rect();
    int term_h = cr.h - TERM_TAB_BAR_H;
    if (term_h < 1) term_h = 1;

    // Force full redraw when active tab changed
    if (tts->active_tab != tts->prev_active_tab) {
        tts->tabs[tts->active_tab]->dirty = true;
        tts->prev_active_tab = tts->active_tab;
    }

    // Force full redraw when content buffer was reallocated (resize/maximize)
    if (win->content != tts->last_content) {
        tts->tabs[tts->active_tab]->dirty = true;
        tts->last_content = win->content;
    }

    TerminalState* ts = tts->tabs[tts->active_tab];

    // Check if window was resized and update terminal grid
    int new_cols = cr.w / mono_cell_width();
    int new_rows = term_h / mono_cell_height();
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;
    if (new_cols != ts->cols || new_rows != ts->rows) {
        for (int i = 0; i < tts->tab_count; i++)
            terminal_resize(tts->tabs[i], new_cols, new_rows);
    }

    // Only redraw when terminal content changed
    if (!ts->dirty) return;

    // ---- Draw tab bar ----
    // Bar is darker than TERM_BG; active tab matches TERM_BG to merge with content
    Canvas c(win->content, cr.w, cr.h);
    Color bar_bg = Color::from_hex(0x1C1C1C);
    c.fill_rect(0, 0, cr.w, TERM_TAB_BAR_H, bar_bg);

    int fh = system_font_height();
    int tab_x = TERM_TAB_PAD;

    for (int i = 0; i < tts->tab_count; i++) {
        bool active = (i == tts->active_tab);
        char label[16];
        snprintf(label, sizeof(label), "Tab %d", i + 1);

        if (active) {
            // Active tab: TERM_BG pill with squared-off bottom → merges into content
            int ty = 5;
            int th = TERM_TAB_BAR_H - ty;
            c.fill_rounded_rect(tab_x, ty, TERM_TAB_W, th, 6, colors::TERM_BG);
            c.fill_rect(tab_x, TERM_TAB_BAR_H - 6, TERM_TAB_W, 6, colors::TERM_BG);

            int text_y = ty + (th - fh) / 2;
            c.text(tab_x + 12, text_y, label, Color::from_hex(0xE0E0E0));
            c.text(tab_x + TERM_TAB_W - 20, text_y, "x", Color::from_hex(0x707070));
        } else {
            // Inactive tab: subtle rounded pill, sits above the bottom edge
            int ty = 7;
            int th = 22;
            c.fill_rounded_rect(tab_x, ty, TERM_TAB_W, th, 5, Color::from_hex(0x262626));

            int text_y = ty + (th - fh) / 2;
            c.text(tab_x + 12, text_y, label, Color::from_hex(0x6E6E6E));
            c.text(tab_x + TERM_TAB_W - 20, text_y, "x", Color::from_hex(0x444444));
        }

        tab_x += TERM_TAB_W + TERM_TAB_GAP;
    }

    // "+" button pinned to right edge
    if (tts->tab_count < TERM_MAX_TABS) {
        int plus_h = 22;
        int py = 7;
        int px = cr.w - TERM_PLUS_PAD - TERM_PLUS_W;
        c.fill_rounded_rect(px, py, TERM_PLUS_W, plus_h, 5, Color::from_hex(0x262626));
        int pw_text = text_width("+");
        c.text(px + (TERM_PLUS_W - pw_text) / 2, py + (plus_h - fh) / 2, "+",
               Color::from_hex(0x6E6E6E));
    }

    // ---- Render active terminal into offset buffer ----
    uint32_t* term_pixels = win->content + TERM_TAB_BAR_H * cr.w;
    terminal_render(ts, term_pixels, cr.w, term_h);

    // Draw scrollbar overlay when scrollback exists
    if (ts->scrollback_lines > 0 && !ts->alt_screen_active) {
        int cell_h = mono_cell_height();
        int total_rows = ts->scrollback_lines + ts->rows;
        int total_h = total_rows * cell_h;
        int view_h = term_h;
        if (total_h > view_h) {
            Canvas sc(term_pixels, cr.w, term_h);
            int sb_w = 4;
            int sb_x = cr.w - sb_w - 2;
            int thumb_h = (view_h * view_h) / total_h;
            if (thumb_h < 20) thumb_h = 20;
            int scroll_from_top = ts->scrollback_lines - ts->view_offset;
            int max_scroll = ts->scrollback_lines;
            int thumb_y = (max_scroll > 0)
                ? (scroll_from_top * (view_h - thumb_h)) / max_scroll : 0;
            sc.fill_rect(sb_x, 0, sb_w, view_h, Color::from_rgb(0x33, 0x33, 0x33));
            sc.fill_rect(sb_x, thumb_y, sb_w, thumb_h, Color::from_rgb(0x88, 0x88, 0x88));
        }
    }
}

static void terminal_on_mouse(Window* win, MouseEvent& ev) {
    TermTabState* tts = (TermTabState*)win->app_data;
    if (!tts || tts->tab_count == 0) return;

    // Scroll wheel: forward to active tab regardless of mouse position
    TerminalState* ts = tts->tabs[tts->active_tab];
    if (ev.scroll != 0 && !ts->alt_screen_active) {
        ts->view_offset += (ev.scroll < 0) ? 3 : -3;
        if (ts->view_offset < 0) ts->view_offset = 0;
        if (ts->view_offset > ts->scrollback_lines)
            ts->view_offset = ts->scrollback_lines;
        ts->dirty = true;
    }

    // Tab bar click handling
    if (!ev.left_pressed()) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    if (ly >= TERM_TAB_BAR_H) return;  // click in terminal area, ignore

    int tab_x = TERM_TAB_PAD;
    for (int i = 0; i < tts->tab_count; i++) {
        if (lx >= tab_x && lx < tab_x + TERM_TAB_W) {
            // Check if click is on the close "x" (last 24px of tab)
            if (lx >= tab_x + TERM_TAB_W - 24) {
                term_close_tab(tts, win, i);
            } else if (i != tts->active_tab) {
                tts->active_tab = i;
                tts->tabs[i]->dirty = true;
            }
            return;
        }
        tab_x += TERM_TAB_W + TERM_TAB_GAP;
    }

    // "+" button pinned to right edge
    int px = cr.w - TERM_PLUS_PAD - TERM_PLUS_W;
    if (tts->tab_count < TERM_MAX_TABS && lx >= px && lx < px + TERM_PLUS_W) {
        int term_h = cr.h - TERM_TAB_BAR_H;
        int cols = cr.w / mono_cell_width();
        int rows = term_h / mono_cell_height();
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        term_add_tab(tts, cols, rows);
    }
}

static void terminal_on_key(Window* win, const Montauk::KeyEvent& key) {
    TermTabState* tts = (TermTabState*)win->app_data;
    if (!tts || tts->tab_count == 0) return;

    // Ctrl+T: new tab (scancode 0x14 = 'T')
    if (key.ctrl && key.pressed && key.scancode == 0x14) {
        if (tts->tab_count < TERM_MAX_TABS) {
            Rect cr = win->content_rect();
            int term_h = cr.h - TERM_TAB_BAR_H;
            int cols = cr.w / mono_cell_width();
            int rows = term_h / mono_cell_height();
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            term_add_tab(tts, cols, rows);
        }
        return;
    }

    // Ctrl+W: close current tab (scancode 0x11 = 'W')
    if (key.ctrl && key.pressed && key.scancode == 0x11) {
        term_close_tab(tts, win, tts->active_tab);
        return;
    }

    // Forward all other keys to active tab
    TerminalState* ts = tts->tabs[tts->active_tab];
    terminal_handle_key(ts, key);
}

static void terminal_on_close(Window* win) {
    TermTabState* tts = (TermTabState*)win->app_data;
    if (tts) {
        for (int i = 0; i < tts->tab_count; i++)
            term_free_tab(tts->tabs[i]);
        montauk::mfree(tts);
        win->app_data = nullptr;
    }
}

static void terminal_on_poll(Window* win) {
    TermTabState* tts = (TermTabState*)win->app_data;
    if (!tts) return;

    // Poll ALL tabs — background shells still produce output
    for (int i = tts->tab_count - 1; i >= 0; i--) {
        if (!terminal_poll(tts->tabs[i])) {
            // This tab's shell exited
            term_close_tab(tts, win, i);
            // term_close_tab may have closed the window (last tab).
            // desktop_close_window calls on_close which nulls app_data
            // before the window slot is recycled, so win is still valid here.
            if (!win->app_data) return;
        }
    }
}

// ============================================================================
// Terminal launcher
// ============================================================================

void open_terminal(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Terminal", 200, 80, 648, 480);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    Rect cr = win->content_rect();
    int term_h = cr.h - TERM_TAB_BAR_H;
    int cols = cr.w / mono_cell_width();
    int rows = term_h / mono_cell_height();
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    TermTabState* tts = (TermTabState*)montauk::malloc(sizeof(TermTabState));
    montauk::memset(tts, 0, sizeof(TermTabState));
    tts->desktop = ds;
    tts->tabs[0] = term_create_tab(ds, cols, rows);
    tts->tab_count = 1;
    tts->active_tab = 0;
    tts->prev_active_tab = 0;
    tts->last_content = win->content;

    win->app_data = tts;
    win->on_draw = terminal_on_draw;
    win->on_mouse = terminal_on_mouse;
    win->on_key = terminal_on_key;
    win->on_close = terminal_on_close;
    win->on_poll = terminal_on_poll;
}
