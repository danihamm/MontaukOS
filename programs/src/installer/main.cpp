/*
 * main.cpp
 * MontaukOS Installer — entry point, event loop, state
 * Copyright (c) 2026 Daniel Hammer
 */

#include "installer.h"

// ============================================================================
// Global state definitions
// ============================================================================

int g_win_w = INIT_W;
int g_win_h = INIT_H;

int FONT_SIZE = 18;
int FONT_SM   = 14;

InstallerState g_state;
TrueTypeFont* g_font = nullptr;
uint32_t* g_pixels = nullptr;
uint32_t* g_backbuf = nullptr;
int g_win_id = -1;

void apply_scale(int scale) {
    switch (scale) {
    case 0: FONT_SIZE = 14; FONT_SM = 11; break;
    case 2: FONT_SIZE = 22; FONT_SM = 17; break;
    default: FONT_SIZE = 18; FONT_SM = 14; break;
    }
}

// ============================================================================
// Helpers
// ============================================================================

void set_status(const char* msg) {
    int i = 0;
    for (; i < 79 && msg[i]; i++) g_state.status[i] = msg[i];
    g_state.status[i] = '\0';
    g_state.status_time = montauk::get_milliseconds();
}

void add_log(const char* msg) {
    if (g_state.log_count >= LOG_LINES) {
        // Scroll: shift all lines up by one
        for (int j = 0; j < LOG_LINES - 1; j++)
            montauk::memcpy(g_state.log[j], g_state.log[j + 1], LOG_LINE_LEN);
        g_state.log_count = LOG_LINES - 1;
    }
    int i = 0;
    for (; i < LOG_LINE_LEN - 1 && msg[i]; i++)
        g_state.log[g_state.log_count][i] = msg[i];
    g_state.log[g_state.log_count][i] = '\0';
    g_state.log_count++;
}

void flush_ui() {
    if (g_pixels && g_backbuf && g_win_id >= 0) {
        render(g_backbuf);
        montauk::memcpy(g_pixels, g_backbuf, g_win_w * g_win_h * 4);
        montauk::win_present(g_win_id);
    }
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
// Mouse handling
// ============================================================================

static bool handle_click(int mx, int my) {
    auto& st = g_state;
    int fh = g_font ? g_font->get_cache(FONT_SIZE)->ascent - g_font->get_cache(FONT_SIZE)->descent : 16;
    int fh_sm = g_font ? g_font->get_cache(FONT_SM)->ascent - g_font->get_cache(FONT_SM)->descent : 12;

    // Common bottom button area
    int btn_w = 120, btn_h = 34;
    int btn_y = g_win_h - STATUS_H - btn_h - 12;

    if (st.step == STEP_MODE_SELECT) {
        // Two mode cards
        int y = CONTENT_TOP + 16 + fh + 4 + fh_sm + 16;
        int card_h = 52, card_x = 16, card_w = g_win_w - 32;

        // Install card
        if (mx >= card_x && mx < card_x + card_w && my >= y && my < y + card_h) {
            st.mode = MODE_INSTALL;
            installer_refresh_disks();
            st.step = STEP_SELECT_DISK;
            return true;
        }
        y += card_h + 8;

        // Update card
        if (mx >= card_x && mx < card_x + card_w && my >= y && my < y + card_h) {
            st.mode = MODE_UPDATE;
            installer_refresh_parts();
            st.step = STEP_UPDATE_SELECT_PART;
            return true;
        }
    } else if (st.step == STEP_SELECT_DISK) {
        // Disk list items
        int y = CONTENT_TOP + 40 + fh + 12;
        for (int i = 0; i < st.disk_count; i++) {
            int item_h = 48;
            if (mx >= 16 && mx < g_win_w - 16 && my >= y && my < y + item_h) {
                st.selected_disk = i;
                return true;
            }
            y += item_h + 4;
        }

        // "Next" button
        int next_x = g_win_w - btn_w - 16;
        if (st.selected_disk >= 0 &&
            mx >= next_x && mx < next_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_PARTITION_SCHEME;
            return true;
        }

        // "Refresh" button
        int ref_w = 80;
        int ref_x = next_x - ref_w - 8;
        if (mx >= ref_x && mx < ref_x + ref_w &&
            my >= btn_y && my < btn_y + btn_h) {
            installer_refresh_disks();
            return true;
        }
    } else if (st.step == STEP_PARTITION_SCHEME) {
        // Scheme radio buttons
        int y = CONTENT_TOP + 40 + fh + 12;
        for (int i = 0; i < SCHEME_COUNT; i++) {
            int item_h = 52;
            if (mx >= 16 && mx < g_win_w - 16 && my >= y && my < y + item_h) {
                st.partition_scheme = i;
                return true;
            }
            y += item_h + 4;
        }

        // "Next" button
        int next_x = g_win_w - btn_w - 16;
        if (mx >= next_x && mx < next_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_CONFIRM;
            return true;
        }

        // "Back" button
        int back_x = next_x - btn_w - 8;
        if (mx >= back_x && mx < back_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_SELECT_DISK;
            return true;
        }
    } else if (st.step == STEP_CONFIRM) {
        int center_x = g_win_w / 2;
        int gap = 16;

        // "Install" button
        int conf_x = center_x - btn_w - gap / 2;
        if (mx >= conf_x && mx < conf_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_INSTALLING;
            st.log_count = 0;
            return true;
        }

        // "Back" button
        int canc_x = center_x + gap / 2;
        if (mx >= canc_x && mx < canc_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_PARTITION_SCHEME;
            return true;
        }
    } else if (st.step == STEP_DONE || st.step == STEP_ERROR) {
        int close_x = (g_win_w - 100) / 2;
        if (mx >= close_x && mx < close_x + 100 &&
            my >= btn_y && my < btn_y + btn_h) {
            return false; // signal quit
        }
    } else if (st.step == STEP_UPDATE_SELECT_PART) {
        // Partition list items
        int y = CONTENT_TOP + 16 + fh + 4 + fh_sm + 12;
        for (int i = 0; i < st.part_count; i++) {
            int item_h = 48;
            if (mx >= 16 && mx < g_win_w - 16 && my >= y && my < y + item_h) {
                st.selected_part = i;
                return true;
            }
            y += item_h + 4;
        }

        // "Next" button
        int next_x = g_win_w - btn_w - 16;
        if (st.selected_part >= 0 &&
            mx >= next_x && mx < next_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_UPDATE_CONFIRM;
            return true;
        }

        // "Back" button
        int back_x = next_x - btn_w - 8;
        if (mx >= back_x && mx < back_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_MODE_SELECT;
            return true;
        }
    } else if (st.step == STEP_UPDATE_CONFIRM) {
        int center_x = g_win_w / 2;
        int gap = 16;

        // "Update" button
        int upd_x = center_x - btn_w - gap / 2;
        if (mx >= upd_x && mx < upd_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_UPDATING;
            st.log_count = 0;
            return true;
        }

        // "Back" button
        int back_x = center_x + gap / 2;
        if (mx >= back_x && mx < back_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h) {
            st.step = STEP_UPDATE_SELECT_PART;
            return true;
        }
    }

    return true;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    montauk::memset(&g_state, 0, sizeof(g_state));
    g_state.selected_disk = -1;
    g_state.selected_part = -1;
    g_state.partition_scheme = SCHEME_EFI_EXT2;
    g_state.step = STEP_MODE_SELECT;

    // Load font
    {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (f) {
            montauk::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init("0:/fonts/Roboto-Medium.ttf")) { montauk::mfree(f); f = nullptr; }
        }
        g_font = f;
    }

    apply_scale(montauk::win_getscale());

    installer_refresh_disks();

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Install MontaukOS", INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;
    g_pixels = pixels;
    g_win_id = win_id;
    g_backbuf = (uint32_t*)montauk::malloc(g_win_w * g_win_h * 4);

    render(g_backbuf);
    montauk::memcpy(pixels, g_backbuf, g_win_w * g_win_h * 4);
    montauk::win_present(win_id);

    bool install_triggered = false;

    while (true) {
        Montauk::WinEvent ev;
        bool redraw = false;

        int r = montauk::win_poll(win_id, &ev);
        if (r < 0) break;

        if (r == 0) {
            if (g_state.step == STEP_INSTALLING && !install_triggered) {
                install_triggered = true;
                flush_ui();
                do_install();
                flush_ui();
            }
            if (g_state.step == STEP_UPDATING && !install_triggered) {
                install_triggered = true;
                flush_ui();
                do_update();
                flush_ui();
            }
            montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) break;

        if (ev.type == 4) {
            apply_scale(ev.scale.scale);
            redraw = true;
        }

        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            g_pixels = pixels;
            montauk::mfree(g_backbuf);
            g_backbuf = (uint32_t*)montauk::malloc(g_win_w * g_win_h * 4);
            redraw = true;
        }

        if (ev.type == 0 && ev.key.pressed) {
            if (ev.key.scancode == 0x01) break;
            redraw = true;
        }

        if (ev.type == 1) {
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
            if (clicked) {
                if (!handle_click(ev.mouse.x, ev.mouse.y))
                    break;
                redraw = true;
            }
        }

        if (redraw) flush_ui();
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
