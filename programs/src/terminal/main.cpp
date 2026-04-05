/*
 * main.cpp
 * MontaukOS Terminal - standalone Window Server app
 * Preserves the old desktop-integrated terminal layout and tab behavior
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/canvas.hpp>
#include <gui/standalone.hpp>
#include <gui/terminal.hpp>
#include <gui/truetype.hpp>

extern "C" {
#include <stdio.h>
}

using namespace gui;

static constexpr int INIT_W = 646;
static constexpr int INIT_H = 449;

static constexpr int TERM_TAB_BAR_H = 34;
static constexpr int TERM_MAX_TABS  = 8;
static constexpr int TERM_TAB_W     = 130;
static constexpr int TERM_TAB_GAP   = 4;
static constexpr int TERM_PLUS_W    = 28;
static constexpr int TERM_PLUS_PAD  = 8;
static constexpr int TERM_TAB_PAD   = 8;

struct TermTabs {
    TerminalState* tabs[TERM_MAX_TABS];
    int tab_count;
    int active_tab;
    int prev_active_tab;
    uint32_t* last_pixels;
};

static WsWindow g_win;
static TermTabs g_tabs = {};
static bool g_should_exit = false;
static bool g_force_redraw = true;
static int g_last_win_w = 0;
static int g_last_win_h = 0;

static bool left_pressed(const Montauk::WinEvent& ev) {
    return (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
}

static void term_request_redraw() {
    g_force_redraw = true;
}

static void term_free_tab(TerminalState* ts) {
    if (!ts) return;
    if (ts->child_pid > 0) montauk::kill(ts->child_pid);
    if (ts->cells) montauk::free(ts->cells);
    if (ts->alt_cells) montauk::free(ts->alt_cells);
    montauk::mfree(ts);
}

static TerminalState* term_create_tab(int cols, int rows) {
    TerminalState* ts = (TerminalState*)montauk::malloc(sizeof(TerminalState));
    if (!ts) return nullptr;
    montauk::memset(ts, 0, sizeof(TerminalState));
    terminal_init(ts, cols, rows);
    if (!ts->cells || !ts->alt_cells) {
        montauk::mfree(ts);
        return nullptr;
    }
    return ts;
}

static void term_close_tab(int idx) {
    if (idx < 0 || idx >= g_tabs.tab_count) return;

    term_free_tab(g_tabs.tabs[idx]);

    for (int i = idx; i < g_tabs.tab_count - 1; i++)
        g_tabs.tabs[i] = g_tabs.tabs[i + 1];
    g_tabs.tab_count--;
    g_tabs.tabs[g_tabs.tab_count] = nullptr;

    if (g_tabs.tab_count == 0) {
        g_should_exit = true;
        return;
    }

    if (idx == g_tabs.active_tab) {
        if (g_tabs.active_tab > 0)
            g_tabs.active_tab--;
    } else if (g_tabs.active_tab > idx) {
        g_tabs.active_tab--;
    }

    if (g_tabs.active_tab >= g_tabs.tab_count)
        g_tabs.active_tab = g_tabs.tab_count - 1;

    g_tabs.tabs[g_tabs.active_tab]->dirty = true;
    term_request_redraw();
}

static bool term_add_tab(int cols, int rows) {
    if (g_tabs.tab_count >= TERM_MAX_TABS) return false;

    TerminalState* ts = term_create_tab(cols, rows);
    if (!ts) return false;

    g_tabs.tabs[g_tabs.tab_count] = ts;
    g_tabs.active_tab = g_tabs.tab_count;
    g_tabs.tab_count++;
    term_request_redraw();
    return true;
}

static bool term_poll_tabs() {
    bool changed = false;

    for (int i = g_tabs.tab_count - 1; i >= 0; i--) {
        if (!terminal_poll(g_tabs.tabs[i])) {
            term_close_tab(i);
            changed = true;
            if (g_should_exit) return true;
        }
    }

    if (g_tabs.tab_count > 0 && g_tabs.tabs[g_tabs.active_tab]->dirty)
        changed = true;

    return changed || g_force_redraw;
}

static bool term_render() {
    if (!g_win.pixels || g_tabs.tab_count <= 0) return false;
    if (g_tabs.active_tab < 0 || g_tabs.active_tab >= g_tabs.tab_count)
        g_tabs.active_tab = 0;

    int term_h = g_win.height - TERM_TAB_BAR_H;
    if (term_h < 1) term_h = 1;

    int new_cols = g_win.width / mono_cell_width();
    int new_rows = term_h / mono_cell_height();
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;

    TerminalState* ts = g_tabs.tabs[g_tabs.active_tab];

    // Only resize the terminal buffer when window pixel dimensions change,
    // not when font size (zoom) changes
    if (g_win.width != g_last_win_w || term_h != g_last_win_h) {
        g_last_win_w = g_win.width;
        g_last_win_h = term_h;
        for (int i = 0; i < g_tabs.tab_count; i++)
            terminal_resize(g_tabs.tabs[i], new_cols, new_rows);
    }

    if (g_tabs.active_tab != g_tabs.prev_active_tab) {
        g_tabs.tabs[g_tabs.active_tab]->dirty = true;
        g_tabs.prev_active_tab = g_tabs.active_tab;
    }

    if (g_win.pixels != g_tabs.last_pixels) {
        g_tabs.tabs[g_tabs.active_tab]->dirty = true;
        g_tabs.last_pixels = g_win.pixels;
    }

    ts = g_tabs.tabs[g_tabs.active_tab];
    if (!g_force_redraw && !ts->dirty) return false;

    Canvas c = g_win.canvas();
    Color bar_bg = Color::from_hex(0x1C1C1C);
    c.fill_rect(0, 0, g_win.width, TERM_TAB_BAR_H, bar_bg);

    int fh = system_font_height();
    int tab_x = TERM_TAB_PAD;

    for (int i = 0; i < g_tabs.tab_count; i++) {
        bool active = (i == g_tabs.active_tab);
        char label[16];
        snprintf(label, sizeof(label), "Tab %d", i + 1);

        if (active) {
            int ty = 5;
            int th = TERM_TAB_BAR_H - ty;
            c.fill_rounded_rect(tab_x, ty, TERM_TAB_W, th, 6, colors::TERM_BG);
            c.fill_rect(tab_x, TERM_TAB_BAR_H - 6, TERM_TAB_W, 6, colors::TERM_BG);

            int text_y = ty + (th - fh) / 2;
            c.text(tab_x + 12, text_y, label, Color::from_hex(0xE0E0E0));
            c.text(tab_x + TERM_TAB_W - 20, text_y, "x", Color::from_hex(0x707070));
        } else {
            int ty = 7;
            int th = 22;
            c.fill_rounded_rect(tab_x, ty, TERM_TAB_W, th, 5, Color::from_hex(0x262626));

            int text_y = ty + (th - fh) / 2;
            c.text(tab_x + 12, text_y, label, Color::from_hex(0x6E6E6E));
            c.text(tab_x + TERM_TAB_W - 20, text_y, "x", Color::from_hex(0x444444));
        }

        tab_x += TERM_TAB_W + TERM_TAB_GAP;
    }

    if (g_tabs.tab_count < TERM_MAX_TABS) {
        int plus_h = 22;
        int py = 7;
        int px = g_win.width - TERM_PLUS_PAD - TERM_PLUS_W;
        c.fill_rounded_rect(px, py, TERM_PLUS_W, plus_h, 5, Color::from_hex(0x262626));
        int pw_text = text_width(fonts::system_font, "+", fonts::UI_SIZE);
        c.text(px + (TERM_PLUS_W - pw_text) / 2, py + (plus_h - fh) / 2, "+",
               Color::from_hex(0x6E6E6E));
    }

    uint32_t* term_pixels = g_win.pixels + TERM_TAB_BAR_H * g_win.width;
    terminal_render(ts, term_pixels, g_win.width, term_h);

    if (ts->scrollback_lines > 0 && !ts->alt_screen_active) {
        int cell_h = mono_cell_height();
        int total_rows = ts->scrollback_lines + ts->rows;
        int total_h = total_rows * cell_h;
        int view_h = term_h;
        if (total_h > view_h) {
            Canvas sc(term_pixels, g_win.width, term_h);
            int sb_w = 4;
            int sb_x = g_win.width - sb_w - 2;
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

    g_force_redraw = false;
    return true;
}

static void term_handle_mouse(const Montauk::WinEvent& ev) {
    if (g_tabs.tab_count <= 0) return;

    TerminalState* ts = g_tabs.tabs[g_tabs.active_tab];
    if (ev.mouse.scroll != 0 && !ts->alt_screen_active) {
        ts->view_offset += (ev.mouse.scroll < 0) ? 3 : -3;
        if (ts->view_offset < 0) ts->view_offset = 0;
        if (ts->view_offset > ts->scrollback_lines)
            ts->view_offset = ts->scrollback_lines;
        ts->dirty = true;
    }

    if (!left_pressed(ev)) return;
    if (ev.mouse.y >= TERM_TAB_BAR_H) return;

    int tab_x = TERM_TAB_PAD;
    for (int i = 0; i < g_tabs.tab_count; i++) {
        if (ev.mouse.x >= tab_x && ev.mouse.x < tab_x + TERM_TAB_W) {
            if (ev.mouse.x >= tab_x + TERM_TAB_W - 24) {
                term_close_tab(i);
            } else if (i != g_tabs.active_tab) {
                g_tabs.active_tab = i;
                g_tabs.tabs[i]->dirty = true;
                term_request_redraw();
            }
            return;
        }
        tab_x += TERM_TAB_W + TERM_TAB_GAP;
    }

    int px = g_win.width - TERM_PLUS_PAD - TERM_PLUS_W;
    if (g_tabs.tab_count < TERM_MAX_TABS &&
        ev.mouse.x >= px && ev.mouse.x < px + TERM_PLUS_W) {
        int cols = g_win.width / mono_cell_width();
        int rows = (g_win.height - TERM_TAB_BAR_H) / mono_cell_height();
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        term_add_tab(cols, rows);
    }
}

static void term_handle_key(const Montauk::KeyEvent& key) {
    if (g_tabs.tab_count <= 0) return;

    if (key.ctrl && key.pressed && key.scancode == 0x14) {
        if (g_tabs.tab_count < TERM_MAX_TABS) {
            int cols = g_win.width / mono_cell_width();
            int rows = (g_win.height - TERM_TAB_BAR_H) / mono_cell_height();
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            term_add_tab(cols, rows);
        }
        return;
    }

    if (key.ctrl && key.pressed && key.scancode == 0x11) {
        term_close_tab(g_tabs.active_tab);
        return;
    }

    // Ctrl+Plus/Equal: zoom in
    if (key.ctrl && key.pressed && (key.ascii == '+' || key.ascii == '=')) {
        if (fonts::TERM_SIZE < 64) {
            fonts::TERM_SIZE += 2;
            for (int i = 0; i < g_tabs.tab_count; i++)
                g_tabs.tabs[i]->dirty = true;
            term_request_redraw();
        }
        return;
    }

    // Ctrl+Minus: zoom out
    if (key.ctrl && key.pressed && key.ascii == '-') {
        if (fonts::TERM_SIZE > 8) {
            fonts::TERM_SIZE -= 2;
            for (int i = 0; i < g_tabs.tab_count; i++)
                g_tabs.tabs[i]->dirty = true;
            term_request_redraw();
        }
        return;
    }

    terminal_handle_key(g_tabs.tabs[g_tabs.active_tab], key);
}

static void term_cleanup() {
    for (int i = 0; i < g_tabs.tab_count; i++)
        term_free_tab(g_tabs.tabs[i]);
    g_tabs.tab_count = 0;
}

extern "C" void _start() {
    if (!fonts::init())
        montauk::exit(1);

    char args[256] = {};
    int arglen = montauk::getargs(args, sizeof(args));
    if (arglen > 0 && args[0])
        montauk::chdir(args);

    if (!g_win.create("Terminal", INIT_W, INIT_H))
        montauk::exit(1);

    int term_h = g_win.height - TERM_TAB_BAR_H;
    g_last_win_w = g_win.width;
    g_last_win_h = term_h;

    int cols = g_win.width / mono_cell_width();
    int rows = (g_win.height - TERM_TAB_BAR_H) / mono_cell_height();
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    g_tabs.tabs[0] = term_create_tab(cols, rows);
    if (!g_tabs.tabs[0]) {
        g_win.destroy();
        montauk::exit(1);
    }
    g_tabs.tab_count = 1;
    g_tabs.active_tab = 0;
    g_tabs.prev_active_tab = -1;
    g_tabs.last_pixels = g_win.pixels;

    if (term_render())
        g_win.present();

    while (!g_should_exit) {
        bool redraw = term_poll_tabs();
        if (g_should_exit) break;

        Montauk::WinEvent ev;
        bool quit = false;
        int r = 0;

        while ((r = g_win.poll(&ev)) > 0) {
            redraw = true;

            if (ev.type == 3) {
                quit = true;
                break;
            }

            if (ev.type == 0) {
                term_handle_key(ev.key);
            } else if (ev.type == 1) {
                term_handle_mouse(ev);
            } else if (ev.type == 2 || ev.type == 4) {
                term_request_redraw();
                if (g_tabs.tab_count > 0)
                    g_tabs.tabs[g_tabs.active_tab]->dirty = true;
            }

            if (g_should_exit) {
                quit = true;
                break;
            }
        }

        if (r < 0 || quit) break;

        if (redraw && term_render())
            g_win.present();

        if (r == 0)
            montauk::sleep_ms(16);
    }

    term_cleanup();
    g_win.destroy();
    montauk::exit(0);
}
