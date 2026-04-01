/*
 * main.cpp
 * MontaukOS Device Explorer — entry point, event loop, state
 * Copyright (c) 2026 Daniel Hammer
 */

#include "devexplorer.h"
#include <gui/standalone.hpp>

// ============================================================================
// Global state definitions
// ============================================================================

int g_win_w = INIT_W;
int g_win_h = INIT_H;

DevExplorerState g_state;
TrueTypeFont* g_font = nullptr;

static void refresh_devices(DevExplorerState* de) {
    if (de == nullptr) return;

    int kernel_count = montauk::devlist(de->devs, MAX_DEVS);
    if (kernel_count < 0) kernel_count = 0;
    if (kernel_count > MAX_DEVS) kernel_count = MAX_DEVS;

    int printer_count = append_printer_devices(
        de->devs + kernel_count, MAX_TOTAL_DEVS - kernel_count);
    if (printer_count < 0) printer_count = 0;

    de->dev_count = kernel_count + printer_count;
    de->last_poll_ms = montauk::get_milliseconds();
}

// ============================================================================
// Display row building
// ============================================================================

int build_display_rows(DevExplorerState* de, DisplayRow* rows) {
    int count = 0;
    for (int cat = 0; cat < NUM_CATEGORIES; cat++) {
        int cat_count = 0;
        for (int d = 0; d < de->dev_count; d++)
            if (de->devs[d].category == cat) cat_count++;
        if (cat_count == 0) continue;

        rows[count].type = ROW_CATEGORY;
        rows[count].category = cat;
        rows[count].dev_index = -1;
        count++;

        if (!de->collapsed[cat]) {
            for (int d = 0; d < de->dev_count; d++) {
                if (de->devs[d].category == cat) {
                    rows[count].type = ROW_DEVICE;
                    rows[count].category = cat;
                    rows[count].dev_index = d;
                    count++;
                }
            }
        }
    }
    return count;
}

// ============================================================================
// Toolbar hit testing
// ============================================================================

static bool handle_toolbar_click(int mx, int my) {
    if (my >= TOOLBAR_H) return false;

    int btn_w = 80, btn_h = 26;
    int btn_x = 8;
    int btn_y = (TOOLBAR_H - btn_h) / 2;
    if (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h) {
        g_state.last_poll_ms = 0; // force refresh
        return true;
    }
    return false;
}

// ============================================================================
// List click handling
// ============================================================================

static bool handle_list_click(int mx, int my) {
    auto& de = g_state;
    int list_y = TOOLBAR_H;
    if (my < list_y) return false;

    DisplayRow rows[MAX_DISPLAY_ROWS];
    int row_count = build_display_rows(&de, rows);

    uint64_t now = montauk::get_milliseconds();
    int cur_y = list_y;
    for (int i = de.scroll_y; i < row_count; i++) {
        int row_h = (rows[i].type == ROW_CATEGORY) ? CAT_H : ITEM_H;
        if (my >= cur_y && my < cur_y + row_h) {
            if (rows[i].type == ROW_CATEGORY) {
                int cat = rows[i].category;
                de.collapsed[cat] = !de.collapsed[cat];
                de.selected_row = -1;
                de.last_click_row = -1;
            } else {
                int di = rows[i].dev_index;
                bool is_double = (de.last_click_row == i)
                              && (now - de.last_click_ms < 400);

                if (is_double && de.devs[di].category == CAT_STORAGE) {
                    int port = (int)de.devs[di]._pad[0];
                    open_disk_detail(port, de.devs[di].name);
                    de.last_click_row = -1;
                } else {
                    de.selected_row = i;
                    de.last_click_row = i;
                    de.last_click_ms = now;
                }
            }
            return true;
        }
        cur_y += row_h;
        if (cur_y >= g_win_h) break;
    }
    de.selected_row = -1;
    de.last_click_row = -1;
    return true;
}

// ============================================================================
// Key handling
// ============================================================================

static bool handle_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return false;

    auto& de = g_state;
    DisplayRow rows[MAX_DISPLAY_ROWS];
    int row_count = build_display_rows(&de, rows);
    if (row_count == 0) return true;

    if (key.scancode == 0x48) { // Up
        if (de.selected_row <= 0) de.selected_row = 0;
        else de.selected_row--;
        if (de.selected_row < de.scroll_y)
            de.scroll_y = de.selected_row;
    } else if (key.scancode == 0x50) { // Down
        if (de.selected_row < row_count - 1)
            de.selected_row++;
        int list_h = g_win_h - TOOLBAR_H;
        int cur_h = 0, last_visible = de.scroll_y;
        for (int i = de.scroll_y; i < row_count; i++) {
            int rh = (rows[i].type == ROW_CATEGORY) ? CAT_H : ITEM_H;
            if (cur_h + rh > list_h) break;
            cur_h += rh;
            last_visible = i;
        }
        if (de.selected_row > last_visible)
            de.scroll_y += (de.selected_row - last_visible);
    } else if (key.scancode == 0x4B) { // Left — collapse
        if (de.selected_row >= 0 && de.selected_row < row_count) {
            int cat = rows[de.selected_row].category;
            if (!de.collapsed[cat]) {
                de.collapsed[cat] = true;
                if (rows[de.selected_row].type == ROW_DEVICE) {
                    for (int i = de.selected_row - 1; i >= 0; i--) {
                        if (rows[i].type == ROW_CATEGORY && rows[i].category == cat) {
                            de.selected_row = i;
                            break;
                        }
                    }
                    int new_count = build_display_rows(&de, rows);
                    if (de.selected_row >= new_count)
                        de.selected_row = new_count - 1;
                }
            }
        }
    } else if (key.scancode == 0x4D) { // Right — expand
        if (de.selected_row >= 0 && de.selected_row < row_count) {
            int cat = rows[de.selected_row].category;
            de.collapsed[cat] = false;
        }
    } else if (key.scancode == 0x1C) { // Enter — toggle category
        if (de.selected_row >= 0 && de.selected_row < row_count) {
            if (rows[de.selected_row].type == ROW_CATEGORY) {
                int cat = rows[de.selected_row].category;
                de.collapsed[cat] = !de.collapsed[cat];
            }
        }
    } else if (key.scancode == 0x01) { // Escape
        return false;
    }
    return true;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    montauk::memset(&g_state, 0, sizeof(g_state));
    g_state.selected_row = -1;
    g_state.last_click_row = -1;

    for (int i = 0; i < NUM_CATEGORIES; i++)
        g_state.collapsed[i] = false;

    // Load font
    {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (f) {
            montauk::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init("0:/fonts/Roboto-Medium.ttf")) { montauk::mfree(f); f = nullptr; }
        }
        g_font = f;
    }

    // Initial device poll
    refresh_devices(&g_state);

    WsWindow win;
    if (!win.create("Devices", INIT_W, INIT_H))
        montauk::exit(1);

    uint32_t* pixels = win.pixels;

    render(pixels);
    win.present();

    while (true) {
        Montauk::WinEvent ev;
        bool redraw_main = false;
        bool redraw_detail = false;

        // Poll for device refresh
        uint64_t now = montauk::get_milliseconds();
        if (now - g_state.last_poll_ms >= POLL_MS) {
            refresh_devices(&g_state);
            redraw_main = true;
        }

        // Poll disk detail window
        if (g_state.detail.open) {
            int dr = montauk::win_poll(g_state.detail.win_id, &ev);
            if (dr > 0) {
                if (ev.type == 3) { // close
                    close_disk_detail();
                    redraw_main = true;
                } else if (ev.type == 1) {
                    bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
                    if (handle_detail_mouse(ev.mouse.x, ev.mouse.y, clicked))
                        redraw_detail = true;
                } else if (ev.type == 2) { // resize
                    g_state.detail.win_w = ev.resize.w;
                    g_state.detail.win_h = ev.resize.h;
                    g_state.detail.pixels = (uint32_t*)(uintptr_t)montauk::win_resize(
                        g_state.detail.win_id, ev.resize.w, ev.resize.h);
                    redraw_detail = true;
                }
            }
        }

        // Poll main window
        int r = win.poll(&ev);

        if (r < 0) break;
        if (r == 0) {
            if (redraw_main) { render(pixels); win.present(); }
            if (redraw_detail) { render_disk_detail(); montauk::win_present(g_state.detail.win_id); }
            if (!redraw_main && !redraw_detail) montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) break; // close

        // Resize
        if (ev.type == 2) {
            g_win_w = win.width;
            g_win_h = win.height;
            pixels = win.pixels;
            redraw_main = true;
        }

        // Keyboard
        if (ev.type == 0 && ev.key.pressed) {
            if (!handle_key(ev.key) && ev.key.scancode == 0x01)
                break;
            redraw_main = true;
        }

        // Mouse
        if (ev.type == 1) {
            int mx = ev.mouse.x;
            int my = ev.mouse.y;
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);

            if (ev.mouse.scroll != 0) {
                g_state.scroll_y += ev.mouse.scroll;
                if (g_state.scroll_y < 0) g_state.scroll_y = 0;
                redraw_main = true;
            }

            if (clicked) {
                if (handle_toolbar_click(mx, my) || handle_list_click(mx, my))
                    redraw_main = true;
            }
        }

        if (redraw_main) { render(pixels); win.present(); }
        if (redraw_detail) { render_disk_detail(); montauk::win_present(g_state.detail.win_id); }
    }

    close_disk_detail();
    win.destroy();
    montauk::exit(0);
}
