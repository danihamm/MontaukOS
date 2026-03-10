/*
 * main.cpp
 * MontaukOS Bluetooth Manager
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>

extern "C" {
#include <string.h>
#include <stdio.h>
}

using namespace gui;

// ============================================================================
// Constants
// ============================================================================

static constexpr int WIN_W        = 360;
static constexpr int WIN_H        = 420;
static constexpr int FONT_SIZE    = 18;
static constexpr int FONT_SIZE_SM = 14;

static constexpr int ROW_H        = 56;
static constexpr int TAB_H        = 36;
static constexpr int PAD          = 16;
static constexpr int BTN_W        = 90;
static constexpr int BTN_H        = 28;
static constexpr int BTN_RAD      = 6;
static constexpr int STATUS_H     = 28;

static constexpr int MAX_SCAN     = 16;
static constexpr int MAX_CONNECTED = 8;

static constexpr Color BG_COLOR      = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TEXT_COLOR    = Color::from_rgb(0x33, 0x33, 0x33);
static constexpr Color DIM_TEXT      = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color ACCENT        = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color ROW_HOVER     = Color::from_rgb(0xF0, 0xF4, 0xFF);
static constexpr Color BORDER        = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color WHITE         = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color GREEN         = Color::from_rgb(0x2E, 0xA0, 0x43);
static constexpr Color RED           = Color::from_rgb(0xCC, 0x33, 0x33);
static constexpr Color TAB_BG        = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color TAB_ACTIVE    = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color TAB_INACTIVE  = Color::from_rgb(0x66, 0x66, 0x66);

// ============================================================================
// State
// ============================================================================

enum Tab { TAB_DEVICES = 0, TAB_SCAN = 1 };

static TrueTypeFont* g_font = nullptr;
static int g_win_w = WIN_W;
static int g_win_h = WIN_H;

static Tab g_tab = TAB_DEVICES;
static int g_hover_row = -1;

// Adapter info
static Montauk::BtAdapterInfo g_adapter;
static bool g_adapter_ok = false;

// Connected/paired devices
static Montauk::BtDevInfo g_devices[MAX_CONNECTED];
static char g_device_names[MAX_CONNECTED][64];
static int g_device_count = 0;

// Scan results
static Montauk::BtScanResult g_scan[MAX_SCAN];
static int g_scan_count = 0;
static bool g_scanning = false;

// Scroll
static int g_scroll = 0;

// Status message
static char g_status[80];
static uint64_t g_status_time = 0;

// ============================================================================
// Pixel helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x,   y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

static void px_fill_rounded(uint32_t* px, int bw, int bh,
                             int x, int y, int w, int h, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            bool skip = false;
            int cx2, cy2;
            if (col < r && row < r) { cx2 = r - col - 1; cy2 = r - row - 1; if (cx2*cx2 + cy2*cy2 >= r*r) skip = true; }
            else if (col >= w - r && row < r) { cx2 = col - (w - r); cy2 = r - row - 1; if (cx2*cx2 + cy2*cy2 >= r*r) skip = true; }
            else if (col < r && row >= h - r) { cx2 = r - col - 1; cy2 = row - (h - r); if (cx2*cx2 + cy2*cy2 >= r*r) skip = true; }
            else if (col >= w - r && row >= h - r) { cx2 = col - (w - r); cy2 = row - (h - r); if (cx2*cx2 + cy2*cy2 >= r*r) skip = true; }
            if (!skip) px[dy * bw + dx] = v;
        }
    }
}

static void px_hline(uint32_t* px, int bw, int bh, int x, int y, int w, Color c) {
    if (y < 0 || y >= bh) return;
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > bw ? bw : x + w;
    for (int col = x0; col < x1; col++)
        px[y * bw + col] = v;
}

static void px_circle(uint32_t* px, int bw, int bh,
                      int cx, int cy, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= bh) continue;
        for (int dx = -r; dx <= r; dx++) {
            int ppx = cx + dx;
            if (ppx < 0 || ppx >= bw) continue;
            if (dx * dx + dy * dy <= r * r)
                px[py * bw + ppx] = v;
        }
    }
}

static void px_text(uint32_t* px, int bw, int bh,
                    int x, int y, const char* text, Color c, int size = FONT_SIZE) {
    if (g_font)
        g_font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}

static int text_w(const char* text, int size = FONT_SIZE) {
    return g_font ? g_font->measure_text(text, size) : 0;
}

static int font_h(int size = FONT_SIZE) {
    if (!g_font) return 14;
    auto* cache = g_font->get_cache(size);
    return cache->ascent - cache->descent;
}

static void px_button(uint32_t* px, int bw, int bh,
                      int x, int y, int w, int h,
                      const char* label, Color bg, Color fg, int r = BTN_RAD) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    int tw = text_w(label, FONT_SIZE_SM);
    int fh = font_h(FONT_SIZE_SM);
    px_text(px, bw, bh, x + (w - tw) / 2, y + (h - fh) / 2 - 1, label, fg, FONT_SIZE_SM);
}

// ============================================================================
// Bluetooth helpers
// ============================================================================

static void set_status(const char* msg) {
    snprintf(g_status, sizeof(g_status), "%s", msg);
    g_status_time = montauk::get_milliseconds();
}

static void refresh_adapter() {
    montauk::memset(&g_adapter, 0, sizeof(g_adapter));
    int r = montauk::bt_info(&g_adapter);
    g_adapter_ok = (r >= 0 && g_adapter.initialized);
}

static void refresh_devices() {
    g_device_count = montauk::bt_list(g_devices, MAX_CONNECTED);
    if (g_device_count < 0) g_device_count = 0;

    // Resolve names from scan results or use address string
    for (int i = 0; i < g_device_count; i++) {
        bool found = false;
        for (int j = 0; j < g_scan_count; j++) {
            if (memcmp(g_devices[i].bdAddr, g_scan[j].bdAddr, 6) == 0 && g_scan[j].name[0]) {
                montauk::memcpy(g_device_names[i], g_scan[j].name, 64);
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(g_device_names[i], 64, "%02X:%02X:%02X:%02X:%02X:%02X",
                     g_devices[i].bdAddr[0], g_devices[i].bdAddr[1],
                     g_devices[i].bdAddr[2], g_devices[i].bdAddr[3],
                     g_devices[i].bdAddr[4], g_devices[i].bdAddr[5]);
        }
    }
}

static void do_scan() {
    g_scanning = true;
    g_scan_count = 0;
    set_status("Scanning...");
}

static void finish_scan() {
    int r = montauk::bt_scan(g_scan, MAX_SCAN, 3000);
    g_scan_count = r > 0 ? r : 0;
    g_scanning = false;
    char msg[64];
    snprintf(msg, sizeof(msg), "Found %d device%s", g_scan_count, g_scan_count == 1 ? "" : "s");
    set_status(msg);
}

static void do_connect(const uint8_t* addr) {
    int r = montauk::bt_connect(addr);
    if (r >= 0) {
        set_status("Connected");
    } else {
        set_status("Connection failed");
    }
    refresh_devices();
}

static void do_disconnect(const uint8_t* addr) {
    int r = montauk::bt_disconnect(addr);
    if (r >= 0) {
        set_status("Disconnected");
    } else {
        set_status("Disconnect failed");
    }
    refresh_devices();
}

static bool is_connected(const uint8_t* addr) {
    for (int i = 0; i < g_device_count; i++) {
        if (memcmp(g_devices[i].bdAddr, addr, 6) == 0 && g_devices[i].connected)
            return true;
    }
    return false;
}

// ============================================================================
// Format helpers
// ============================================================================

static const char* device_class_str(uint32_t cod) {
    uint32_t major = (cod >> 8) & 0x1F;
    switch (major) {
        case 1:  return "Computer";
        case 2:  return "Phone";
        case 3:  return "Network";
        case 4:  return "Audio/Video";
        case 5:  return "Peripheral";
        case 6:  return "Imaging";
        case 7:  return "Wearable";
        default: return "Device";
    }
}

static void format_addr(char* buf, int len, const uint8_t* addr) {
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static const char* rssi_bar(int8_t rssi) {
    if (rssi >= -50) return "Strong";
    if (rssi >= -70) return "Good";
    if (rssi >= -85) return "Weak";
    return "Very weak";
}

// ============================================================================
// Render
// ============================================================================

static void render(uint32_t* pixels) {
    int W = g_win_w;
    int H = g_win_h;

    // Background
    px_fill(pixels, W, H, 0, 0, W, H, BG_COLOR);

    // Tab bar
    int tab_y = 0;
    px_fill(pixels, W, H, 0, tab_y, W, TAB_H, TAB_BG);
    px_hline(pixels, W, H, 0, tab_y + TAB_H - 1, W, BORDER);

    int tab_w = W / 2;
    const char* tab_labels[] = { "Devices", "Scan" };
    for (int i = 0; i < 2; i++) {
        // Active tab gets white background to merge with content area
        if (g_tab == i)
            px_fill(pixels, W, H, i * tab_w, tab_y, tab_w, TAB_H, BG_COLOR);

        Color tc = (g_tab == i) ? TAB_ACTIVE : TAB_INACTIVE;
        int tw = text_w(tab_labels[i], FONT_SIZE);
        int tx = i * tab_w + (tab_w - tw) / 2;
        px_text(pixels, W, H, tx, tab_y + (TAB_H - font_h(FONT_SIZE)) / 2,
                tab_labels[i], tc, FONT_SIZE);
        // Active indicator underline
        if (g_tab == i) {
            px_fill(pixels, W, H, i * tab_w + 4, tab_y + TAB_H - 3, tab_w - 8, 3, TAB_ACTIVE);
        }
    }

    int content_y = tab_y + TAB_H;
    int content_h = H - content_y;

    // Status bar at bottom
    int list_h = content_h - STATUS_H;

    if (g_tab == TAB_DEVICES) {
        // Connected devices list
        if (g_device_count == 0) {
            const char* msg = "No connected devices";
            int mw = text_w(msg, FONT_SIZE);
            px_text(pixels, W, H, (W - mw) / 2, content_y + list_h / 2 - font_h(FONT_SIZE) / 2,
                    msg, DIM_TEXT, FONT_SIZE);
        } else {
            for (int i = 0; i < g_device_count; i++) {
                int ry = content_y + i * ROW_H - g_scroll;
                if (ry + ROW_H < content_y || ry >= content_y + list_h) continue;

                // Hover highlight
                if (g_hover_row == i)
                    px_fill(pixels, W, H, 0, ry, W, ROW_H, ROW_HOVER);

                // Connected indicator dot
                Color dot_c = g_devices[i].connected ? GREEN : DIM_TEXT;
                px_circle(pixels, W, H, PAD + 6, ry + ROW_H / 2, 5, dot_c);

                // Device name
                px_text(pixels, W, H, PAD + 20, ry + 8,
                        g_device_names[i], TEXT_COLOR, FONT_SIZE);

                // Address below name
                char addr_str[24];
                format_addr(addr_str, sizeof(addr_str), g_devices[i].bdAddr);
                px_text(pixels, W, H, PAD + 20, ry + 8 + font_h(FONT_SIZE) + 2,
                        addr_str, DIM_TEXT, FONT_SIZE_SM);

                // Disconnect button
                if (g_devices[i].connected) {
                    int bx = W - PAD - BTN_W;
                    int by = ry + (ROW_H - BTN_H) / 2;
                    px_button(pixels, W, H, bx, by, BTN_W, BTN_H,
                              "Disconnect", RED, WHITE);
                }

                // Divider
                px_hline(pixels, W, H, PAD, ry + ROW_H - 1, W - 2 * PAD, BORDER);
            }
        }
    } else {
        // Scan tab
        // Scan button at top of content area
        int scan_btn_y = content_y + 12;
        int scan_btn_w = 100;
        int scan_btn_x = (W - scan_btn_w) / 2;
        if (g_scanning) {
            px_button(pixels, W, H, scan_btn_x, scan_btn_y, scan_btn_w, BTN_H,
                      "Scanning...", BORDER, DIM_TEXT);
        } else {
            px_button(pixels, W, H, scan_btn_x, scan_btn_y, scan_btn_w, BTN_H,
                      "Scan", ACCENT, WHITE);
        }

        int list_top = scan_btn_y + BTN_H + 12;
        int scan_list_h = content_y + list_h - list_top;

        if (g_scan_count == 0 && !g_scanning) {
            const char* msg = "Press Scan to find devices";
            int mw = text_w(msg, FONT_SIZE);
            px_text(pixels, W, H, (W - mw) / 2, list_top + scan_list_h / 2 - font_h(FONT_SIZE) / 2,
                    msg, DIM_TEXT, FONT_SIZE);
        } else {
            for (int i = 0; i < g_scan_count; i++) {
                int ry = list_top + i * ROW_H - g_scroll;
                if (ry + ROW_H < list_top || ry >= list_top + scan_list_h) continue;

                // Hover highlight
                if (g_hover_row == i)
                    px_fill(pixels, W, H, 0, ry, W, ROW_H, ROW_HOVER);

                // Device type indicator
                const char* type_str = device_class_str(g_scan[i].classOfDevice);
                px_circle(pixels, W, H, PAD + 6, ry + ROW_H / 2, 5, ACCENT);

                // Device name (or address if unnamed)
                const char* display_name = g_scan[i].name[0] ? g_scan[i].name : "Unknown Device";
                px_text(pixels, W, H, PAD + 20, ry + 4,
                        display_name, TEXT_COLOR, FONT_SIZE);

                // Type + signal below name
                char detail[80];
                char addr_str[24];
                format_addr(addr_str, sizeof(addr_str), g_scan[i].bdAddr);
                snprintf(detail, sizeof(detail), "%s  |  %s  |  %s",
                         type_str, addr_str, rssi_bar(g_scan[i].rssi));
                px_text(pixels, W, H, PAD + 20, ry + 4 + font_h(FONT_SIZE) + 2,
                        detail, DIM_TEXT, FONT_SIZE_SM);

                // Connect/Disconnect button
                bool conn = is_connected(g_scan[i].bdAddr);
                int bx = W - PAD - BTN_W;
                int by = ry + (ROW_H - BTN_H) / 2;
                if (conn) {
                    px_button(pixels, W, H, bx, by, BTN_W, BTN_H,
                              "Disconnect", RED, WHITE);
                } else {
                    px_button(pixels, W, H, bx, by, BTN_W, BTN_H,
                              "Connect", ACCENT, WHITE);
                }

                // Divider
                px_hline(pixels, W, H, PAD, ry + ROW_H - 1, W - 2 * PAD, BORDER);
            }
        }
    }

    // Status bar
    int status_y = H - STATUS_H;
    px_fill(pixels, W, H, 0, status_y, W, STATUS_H, TAB_BG);
    px_hline(pixels, W, H, 0, status_y, W, BORDER);

    // Status bar content: adapter info on the left, status message on the right
    uint64_t now = montauk::get_milliseconds();
    int sy = status_y + (STATUS_H - font_h(FONT_SIZE_SM)) / 2;

    if (g_adapter_ok) {
        char adapter_info[96];
        char addr_str[24];
        format_addr(addr_str, sizeof(addr_str), g_adapter.bdAddr);
        const char* name = g_adapter.name[0] ? g_adapter.name : "Adapter";
        snprintf(adapter_info, sizeof(adapter_info), "%s  |  %s  |  %d connected",
                 name, addr_str, g_device_count);
        px_text(pixels, W, H, PAD, sy, adapter_info, DIM_TEXT, FONT_SIZE_SM);
    } else {
        px_text(pixels, W, H, PAD, sy, "No adapter found", RED, FONT_SIZE_SM);
    }

    // Temporary status message (right-aligned, fades after 5s)
    if (g_status[0] && (now - g_status_time) < 5000) {
        int sw = text_w(g_status, FONT_SIZE_SM);
        px_text(pixels, W, H, W - PAD - sw, sy, g_status, DIM_TEXT, FONT_SIZE_SM);
    }
}

// ============================================================================
// Hit testing
// ============================================================================

static bool handle_click(int mx, int my) {
    int W = g_win_w;

    // Tab bar
    int tab_y = 0;
    if (my >= tab_y && my < tab_y + TAB_H) {
        Tab new_tab = (mx < W / 2) ? TAB_DEVICES : TAB_SCAN;
        if (new_tab != g_tab) {
            g_tab = new_tab;
            g_scroll = 0;
            g_hover_row = -1;
            return true;
        }
        return false;
    }

    int content_y = tab_y + TAB_H;

    if (g_tab == TAB_SCAN) {
        // Scan button
        int scan_btn_y = content_y + 8;
        int scan_btn_w = 100;
        int scan_btn_x = (W - scan_btn_w) / 2;
        if (mx >= scan_btn_x && mx < scan_btn_x + scan_btn_w &&
            my >= scan_btn_y && my < scan_btn_y + BTN_H && !g_scanning) {
            do_scan();
            return true;
        }

        // Scan result rows
        int list_top = scan_btn_y + BTN_H + 12;
        for (int i = 0; i < g_scan_count; i++) {
            int ry = list_top + i * ROW_H - g_scroll;
            if (my >= ry && my < ry + ROW_H) {
                // Check if disconnect/connect button clicked
                int bx = W - PAD - BTN_W;
                int by = ry + (ROW_H - BTN_H) / 2;
                if (mx >= bx && mx < bx + BTN_W && my >= by && my < by + BTN_H) {
                    if (is_connected(g_scan[i].bdAddr)) {
                        do_disconnect(g_scan[i].bdAddr);
                    } else {
                        do_connect(g_scan[i].bdAddr);
                    }
                    return true;
                }
            }
        }
    } else {
        // Device rows
        for (int i = 0; i < g_device_count; i++) {
            int ry = content_y + i * ROW_H - g_scroll;
            if (my >= ry && my < ry + ROW_H) {
                // Disconnect button
                if (g_devices[i].connected) {
                    int bx = W - PAD - BTN_W;
                    int by = ry + (ROW_H - BTN_H) / 2;
                    if (mx >= bx && mx < bx + BTN_W && my >= by && my < by + BTN_H) {
                        do_disconnect(g_devices[i].bdAddr);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

static int get_hover_row(int mx, int my) {
    int content_y = TAB_H;

    if (g_tab == TAB_DEVICES) {
        int count = g_device_count;
        for (int i = 0; i < count; i++) {
            int ry = content_y + i * ROW_H - g_scroll;
            if (my >= ry && my < ry + ROW_H)
                return i;
        }
    } else {
        int list_top = content_y + 8 + BTN_H + 12;
        for (int i = 0; i < g_scan_count; i++) {
            int ry = list_top + i * ROW_H - g_scroll;
            if (my >= ry && my < ry + ROW_H)
                return i;
        }
    }
    return -1;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Load font
    {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (f) {
            montauk::memset(f, 0, sizeof(TrueTypeFont));
            if (!f->init("0:/fonts/Roboto-Medium.ttf")) { montauk::mfree(f); f = nullptr; }
        }
        g_font = f;
    }

    // Query adapter
    refresh_adapter();
    refresh_devices();
    g_status[0] = 0;

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Bluetooth", WIN_W, WIN_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    render(pixels);
    montauk::win_present(win_id);

    // Periodic refresh timer
    uint64_t last_refresh = montauk::get_milliseconds();

    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;

        // Handle pending scan (non-blocking: kick it off after render)
        if (g_scanning) {
            render(pixels);
            montauk::win_present(win_id);
            finish_scan();
            refresh_devices();
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        if (r == 0) {
            // Periodic refresh every 5 seconds
            uint64_t now = montauk::get_milliseconds();
            if (now - last_refresh >= 5000) {
                refresh_adapter();
                refresh_devices();
                last_refresh = now;
                render(pixels);
                montauk::win_present(win_id);
            }
            montauk::sleep_ms(16);
            continue;
        }

        bool redraw = false;

        if (ev.type == 3) break; // close

        // Resize
        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            redraw = true;
        }

        // Keyboard
        if (ev.type == 0 && ev.key.pressed) {
            if (ev.key.scancode == 0x01) break; // Escape
            if (ev.key.ascii == 's' || ev.key.ascii == 'S') {
                if (!g_scanning) { do_scan(); redraw = true; }
            }
            if (ev.key.ascii == '1') { g_tab = TAB_DEVICES; g_scroll = 0; redraw = true; }
            if (ev.key.ascii == '2') { g_tab = TAB_SCAN; g_scroll = 0; redraw = true; }
        }

        // Mouse
        if (ev.type == 1) {
            bool clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);

            int new_hover = get_hover_row(ev.mouse.x, ev.mouse.y);
            if (new_hover != g_hover_row) {
                g_hover_row = new_hover;
                redraw = true;
            }

            if (clicked) {
                if (handle_click(ev.mouse.x, ev.mouse.y))
                    redraw = true;
            }

            // Scroll
            if (ev.mouse.scroll) {
                g_scroll -= ev.mouse.scroll * 20;
                if (g_scroll < 0) g_scroll = 0;
                redraw = true;
            }
        }

        if (redraw) {
            render(pixels);
            montauk::win_present(win_id);
        }
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
