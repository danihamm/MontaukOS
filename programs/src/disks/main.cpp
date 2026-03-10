/*
 * main.cpp
 * MontaukOS Disk Tool — entry point, event loop, state
 * Copyright (c) 2026 Daniel Hammer
 */

#include "disks.h"

// ============================================================================
// Global state definitions
// ============================================================================

int g_win_w = INIT_W;
int g_win_h = INIT_H;

DiskToolState g_state;
TrueTypeFont* g_font = nullptr;

const Color part_colors[NUM_PART_COLORS] = {
    Color::from_rgb(0x42, 0x7A, 0xB5),
    Color::from_rgb(0x5C, 0xA0, 0x5C),
    Color::from_rgb(0xCC, 0x88, 0x33),
    Color::from_rgb(0x99, 0x55, 0xBB),
    Color::from_rgb(0xCC, 0x55, 0x55),
    Color::from_rgb(0x44, 0xAA, 0xAA),
    Color::from_rgb(0xBB, 0xBB, 0x44),
    Color::from_rgb(0x88, 0x66, 0x44),
};

// ============================================================================
// Helpers
// ============================================================================

void set_status(const char* msg) {
    int i = 0;
    for (; i < 79 && msg[i]; i++) g_state.status[i] = msg[i];
    g_state.status[i] = '\0';
    g_state.status_time = montauk::get_milliseconds();
}

int get_disk_parts(int* indices, int max) {
    int count = 0;
    for (int i = 0; i < g_state.part_count && count < max; i++) {
        if (g_state.parts[i].blockDev == g_state.selected_disk) {
            indices[count++] = i;
        }
    }
    return count;
}

void format_disk_size(char* buf, int bufsize, uint64_t sectors, uint16_t sectorSize) {
    uint64_t bytes = sectors * sectorSize;
    uint64_t gb = bytes / (1024ULL * 1024 * 1024);
    if (gb >= 1024) {
        uint64_t tb = gb / 1024;
        uint64_t frac = ((gb % 1024) * 10) / 1024;
        snprintf(buf, bufsize, "%lu.%lu TB", (unsigned)tb, (unsigned)frac);
    } else if (gb > 0) {
        uint64_t frac = ((bytes % (1024ULL * 1024 * 1024)) * 10) / (1024ULL * 1024 * 1024);
        snprintf(buf, bufsize, "%lu.%lu GB", (unsigned)gb, (unsigned)frac);
    } else {
        uint64_t mb = bytes / (1024ULL * 1024);
        snprintf(buf, bufsize, "%lu MB", (unsigned)mb);
    }
}

// ============================================================================
// Toolbar hit testing (returns button widths matching render layout)
// ============================================================================

static int disk_btn_width(int idx) {
    char label[8];
    snprintf(label, sizeof(label), "Disk %d", idx);
    int lw = g_font ? g_font->measure_text(label, FONT_SIZE) + 16 : 48;
    return lw;
}

static bool handle_toolbar_click(int mx, int my) {
    if (my >= TOOLBAR_H || my < TB_BTN_Y) return false;

    auto& dt = g_state;

    // Disk selector buttons
    int bx = 8;
    for (int i = 0; i < dt.disk_count; i++) {
        int lw = disk_btn_width(i);
        if (mx >= bx && mx < bx + lw && my >= TB_BTN_Y && my < TB_BTN_Y + TB_BTN_H) {
            dt.selected_disk = i;
            dt.selected_part = -1;
            dt.scroll_y = 0;
            return true;
        }
        bx += lw + 6;
    }

    // Right-side buttons (must match render layout)
    int rx = g_win_w - 8;

    int ref_w = 64; rx -= ref_w;
    if (mx >= rx && mx < rx + ref_w && my >= TB_BTN_Y && my < TB_BTN_Y + TB_BTN_H) {
        disktool_refresh();
        set_status("Refreshed");
        return true;
    }
    rx -= 6;

    int mnt_w = 60; rx -= mnt_w;
    if (mx >= rx && mx < rx + mnt_w && my >= TB_BTN_Y && my < TB_BTN_Y + TB_BTN_H) {
        do_mount_partition();
        return true;
    }
    rx -= 6;

    int fmt_w = 64; rx -= fmt_w;
    if (mx >= rx && mx < rx + fmt_w && my >= TB_BTN_Y && my < TB_BTN_Y + TB_BTN_H) {
        open_format_dialog();
        return true;
    }
    rx -= 6;

    int np_w = 74; rx -= np_w;
    if (mx >= rx && mx < rx + np_w && my >= TB_BTN_Y && my < TB_BTN_Y + TB_BTN_H) {
        do_create_partition();
        return true;
    }

    return false;
}

// ============================================================================
// Mouse handling for content area
// ============================================================================

static bool handle_content_click(int mx, int my) {
    auto& dt = g_state;
    if (dt.disk_count == 0 || dt.selected_disk < 0) return false;

    int fh = g_font ? g_font->get_cache(FONT_SIZE)->ascent - g_font->get_cache(FONT_SIZE)->descent : 16;
    int y = TOOLBAR_H + 8 + fh + 8;

    // Partition map bar click
    int map_x = MAP_PAD;
    int map_w = g_win_w - MAP_PAD * 2;
    if (my >= y && my < y + MAP_H) {
        Montauk::DiskInfo& disk = dt.disks[dt.selected_disk];
        int part_indices[MAX_PARTS];
        int nparts = get_disk_parts(part_indices, MAX_PARTS);
        uint64_t total = disk.sectorCount;
        if (total > 0) {
            for (int pi = 0; pi < nparts; pi++) {
                Montauk::PartInfo& p = dt.parts[part_indices[pi]];
                int px = map_x + (int)((p.startLba * (uint64_t)map_w) / total);
                int pw = (int)((p.sectorCount * (uint64_t)map_w) / total);
                if (pw < 2) pw = 2;
                if (mx >= px && mx < px + pw) {
                    dt.selected_part = pi;
                    return true;
                }
            }
        }
        dt.selected_part = -1;
        return true;
    }

    y += MAP_H + 8 + HEADER_H;

    // Partition list click
    int list_bottom = g_win_h - STATUS_H;
    if (my >= y && my < list_bottom) {
        int row = (my - y + dt.scroll_y) / ITEM_H;
        int part_indices[MAX_PARTS];
        int nparts = get_disk_parts(part_indices, MAX_PARTS);
        if (row >= 0 && row < nparts)
            dt.selected_part = row;
        else
            dt.selected_part = -1;
        return true;
    }

    return false;
}

// ============================================================================
// New partition dialog mouse handling
// ============================================================================

static bool handle_newpart_dialog_click(int mx, int my, bool clicked) {
    auto& dlg = g_state.np_dlg;
    if (!dlg.open) return false;

    int dw = NP_DLG_W, dh = NP_DLG_H;

    int btn_w = 90, btn_h = 30;
    int btn_y = dh - btn_h - 16;
    int gap = 16;
    int total_w = btn_w * 2 + gap;
    int bx = (dw - total_w) / 2;

    dlg.hover_confirm = (mx >= bx && mx < bx + btn_w && my >= btn_y && my < btn_y + btn_h);
    dlg.hover_cancel = (mx >= bx + btn_w + gap && mx < bx + btn_w * 2 + gap && my >= btn_y && my < btn_y + btn_h);

    if (!clicked) return true;

    if (dlg.hover_confirm) {
        newpart_dialog_confirm();
        return true;
    }

    if (dlg.hover_cancel) {
        close_newpart_dialog();
        return true;
    }

    return true;
}

// ============================================================================
// Format dialog mouse handling
// ============================================================================

static bool handle_format_dialog_click(int mx, int my, bool clicked) {
    auto& dlg = g_state.fmt_dlg;
    if (!dlg.open) return false;

    int dw = FMT_DLG_W, dh = FMT_DLG_H;
    int fh = g_font ? g_font->get_cache(FONT_SIZE)->ascent - g_font->get_cache(FONT_SIZE)->descent : 16;

    // Button hit testing
    int btn_w = 90, btn_h = 30;
    int btn_y = dh - btn_h - 16;
    int gap = 16;
    int total_w = btn_w * 2 + gap;
    int bx = (dw - total_w) / 2;

    dlg.hover_format = (mx >= bx && mx < bx + btn_w && my >= btn_y && my < btn_y + btn_h);
    dlg.hover_cancel = (mx >= bx + btn_w + gap && mx < bx + btn_w * 2 + gap && my >= btn_y && my < btn_y + btn_h);

    if (!clicked) return true;

    // FS type selector
    int sel_y = 12 + fh * 2 + 18;
    int opt_y = sel_y + fh + 8;
    for (int i = 0; i < NUM_FS_TYPES; i++) {
        int iy = opt_y + i * 32;
        if (mx >= 24 && mx < dw - 24 && my >= iy && my < iy + 28) {
            dlg.selected_fs = i;
            return true;
        }
    }

    if (dlg.hover_format) {
        format_dialog_do_format();
        return true;
    }

    if (dlg.hover_cancel) {
        close_format_dialog();
        return true;
    }

    return true;
}

// ============================================================================
// Key handling
// ============================================================================

static bool handle_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return false;

    auto& dt = g_state;

    // New partition dialog keys
    if (dt.np_dlg.open) {
        if (key.scancode == 0x01) { // Escape
            close_newpart_dialog();
        } else if (key.ascii == '\n' || key.ascii == '\r') {
            newpart_dialog_confirm();
        }
        return true;
    }

    // Format dialog keys
    if (dt.fmt_dlg.open) {
        if (key.scancode == 0x01) { // Escape
            close_format_dialog();
        } else if (key.scancode == 0x48 && dt.fmt_dlg.selected_fs > 0) {
            dt.fmt_dlg.selected_fs--;
        } else if (key.scancode == 0x50 && dt.fmt_dlg.selected_fs < NUM_FS_TYPES - 1) {
            dt.fmt_dlg.selected_fs++;
        } else if (key.ascii == '\n' || key.ascii == '\r') {
            format_dialog_do_format();
        }
        return true;
    }

    int part_indices[MAX_PARTS];
    int nparts = get_disk_parts(part_indices, MAX_PARTS);

    if (key.scancode == 0x01) { // Escape
        return false; // signal quit
    } else if (key.scancode == 0x48) { // Up
        if (dt.selected_part > 0) dt.selected_part--;
        else if (nparts > 0) dt.selected_part = 0;
    } else if (key.scancode == 0x50) { // Down
        if (dt.selected_part < nparts - 1) dt.selected_part++;
    } else if (key.scancode == 0x4B) { // Left
        if (dt.selected_disk > 0) { dt.selected_disk--; dt.selected_part = -1; dt.scroll_y = 0; }
    } else if (key.scancode == 0x4D) { // Right
        if (dt.selected_disk < dt.disk_count - 1) { dt.selected_disk++; dt.selected_part = -1; dt.scroll_y = 0; }
    } else if (key.ascii == 'n' || key.ascii == 'N') {
        do_create_partition();
    } else if (key.ascii == 'm' || key.ascii == 'M') {
        do_mount_partition();
    } else if (key.ascii == 'f' || key.ascii == 'F') {
        open_format_dialog();
    } else {
        return true; // consumed but no action
    }
    return true;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    montauk::memset(&g_state, 0, sizeof(g_state));
    g_state.selected_disk = 0;
    g_state.selected_part = -1;

    // Load font
    {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (f) {
            montauk::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init("0:/fonts/Roboto-Medium.ttf")) { montauk::mfree(f); f = nullptr; }
        }
        g_font = f;
    }

    disktool_refresh();

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Disks", INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    render(pixels);
    montauk::win_present(win_id);

    while (true) {
        Montauk::WinEvent ev;
        bool redraw_main = false;
        bool redraw_dlg = false;

        bool redraw_np = false;

        // Poll new partition dialog window
        if (g_state.np_dlg.open) {
            int dr = montauk::win_poll(g_state.np_dlg.win_id, &ev);
            if (dr > 0) {
                if (ev.type == 3) { // close
                    close_newpart_dialog();
                    redraw_main = true;
                } else if (ev.type == 0 && ev.key.pressed) {
                    handle_key(ev.key);
                    redraw_np = g_state.np_dlg.open;
                    redraw_main = true;
                } else if (ev.type == 1) {
                    bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
                    handle_newpart_dialog_click(ev.mouse.x, ev.mouse.y, clicked);
                    redraw_np = g_state.np_dlg.open;
                    redraw_main = true;
                }
            }
        }

        // Poll format dialog window
        if (g_state.fmt_dlg.open) {
            int dr = montauk::win_poll(g_state.fmt_dlg.win_id, &ev);
            if (dr > 0) {
                if (ev.type == 3) { // close
                    close_format_dialog();
                    redraw_main = true;
                } else if (ev.type == 0 && ev.key.pressed) {
                    handle_key(ev.key);
                    redraw_dlg = g_state.fmt_dlg.open;
                    redraw_main = true;
                } else if (ev.type == 1) {
                    bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
                    handle_format_dialog_click(ev.mouse.x, ev.mouse.y, clicked);
                    redraw_dlg = g_state.fmt_dlg.open;
                    redraw_main = true;
                }
            }
        }

        // Poll main window
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;
        if (r == 0) {
            // Even with no main event, dialog may have triggered redraws
            if (redraw_main) { render(pixels); montauk::win_present(win_id); }
            if (redraw_dlg) { render_format_window(); montauk::win_present(g_state.fmt_dlg.win_id); }
            if (redraw_np) { render_newpart_window(); montauk::win_present(g_state.np_dlg.win_id); }
            if (!redraw_main && !redraw_dlg && !redraw_np) montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) break; // close

        // Resize
        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            redraw_main = true;
        }

        // Keyboard
        if (ev.type == 0 && ev.key.pressed) {
            if (!handle_key(ev.key) && ev.key.scancode == 0x01)
                break; // Escape quits when not in dialog
            redraw_main = true;
        }

        // Mouse
        if (ev.type == 1) {
            int mx = ev.mouse.x;
            int my = ev.mouse.y;
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);

            // Scroll
            if (ev.mouse.scroll != 0) {
                g_state.scroll_y -= ev.mouse.scroll * 20;
                if (g_state.scroll_y < 0) g_state.scroll_y = 0;
                redraw_main = true;
            }

            if (clicked) {
                if (handle_toolbar_click(mx, my) || handle_content_click(mx, my))
                    redraw_main = true;
            }
        }

        if (redraw_main) { render(pixels); montauk::win_present(win_id); }
        if (redraw_dlg) { render_format_window(); montauk::win_present(g_state.fmt_dlg.win_id); }
        if (redraw_np) { render_newpart_window(); montauk::win_present(g_state.np_dlg.win_id); }
    }

    close_newpart_dialog();
    close_format_dialog();
    montauk::win_destroy(win_id);
    montauk::exit(0);
}
