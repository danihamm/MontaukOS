/*
    * main.cpp
    * ZenithOS Desktop Environment - window manager, compositor, and applications
    * Copyright (c) 2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>
#include <zenith/heap.h>
#include <gui/gui.hpp>
#include <gui/framebuffer.hpp>
#include <gui/font.hpp>
#include <gui/draw.hpp>
#include <gui/svg.hpp>
#include <gui/widgets.hpp>
#include <gui/window.hpp>
#include <gui/terminal.hpp>
#include <gui/desktop.hpp>

// Placement new for freestanding environment
inline void* operator new(unsigned long, void* p) { return p; }

using namespace gui;

// ============================================================================
// Minimal snprintf (copied from init/main.cpp pattern)
// ============================================================================

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState { char* buf; int pos; int max; };

static void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

static void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24]; int i = 0;
    const char* digits = "0123456789abcdef";
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    int total = (neg ? 1 : 0) + i;
    if (neg && pad == '0') pf_putc(st, '-');
    for (int w = total; w < width; w++) pf_putc(st, pad);
    if (neg && pad != '0') pf_putc(st, '-');
    while (i > 0) pf_putc(st, tmp[--i]);
}

static int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    PfState st; st.buf = buf; st.pos = 0; st.max = size > 0 ? size - 1 : 0;
    while (*fmt) {
        if (*fmt != '%') { pf_putc(&st, *fmt++); continue; }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            long val = va_arg(ap, int);
            int neg = 0; unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); } else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg); break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            for (int w = slen; w < width; w++) pf_putc(&st, ' ');
            for (int j = 0; j < slen; j++) pf_putc(&st, s[j]);
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); pf_putc(&st, c); break; }
        case '%': pf_putc(&st, '%'); break;
        default: pf_putc(&st, '%'); pf_putc(&st, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (size > 0) { if (st.pos < size) st.buf[st.pos] = '\0'; else st.buf[size - 1] = '\0'; }
    va_end(ap); return st.pos;
}

// ============================================================================
// String helpers
// ============================================================================

static void str_append(char* dst, const char* src, int max) {
    int len = zenith::slen(dst);
    int i = 0;
    while (src[i] && len < max - 1) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

// ============================================================================
// File Manager Application
// ============================================================================

struct FileManagerState {
    char current_path[256];
    char entry_names[64][64];
    int entry_count;
    int selected;
    int scroll_offset;
    bool is_dir[64];
};

static void filemanager_read_dir(FileManagerState* fm) {
    const char* names[64];
    fm->entry_count = zenith::readdir(fm->current_path, names, 64);
    if (fm->entry_count < 0) fm->entry_count = 0;
    for (int i = 0; i < fm->entry_count; i++) {
        zenith::strncpy(fm->entry_names[i], names[i], 63);
        // Heuristic: entries ending with '/' or without '.' are directories
        int len = zenith::slen(fm->entry_names[i]);
        if (len > 0 && fm->entry_names[i][len - 1] == '/') {
            fm->is_dir[i] = true;
            fm->entry_names[i][len - 1] = '\0'; // strip trailing slash
        } else {
            // Check if entry has an extension (has a dot)
            bool has_dot = false;
            for (int j = 0; j < len; j++) {
                if (fm->entry_names[i][j] == '.') { has_dot = true; break; }
            }
            fm->is_dir[i] = !has_dot;
        }
    }
    fm->selected = -1;
    fm->scroll_offset = 0;
}

static void filemanager_navigate(FileManagerState* fm, const char* name) {
    // Build new path
    int path_len = zenith::slen(fm->current_path);
    if (path_len > 0 && fm->current_path[path_len - 1] != '/') {
        str_append(fm->current_path, "/", 256);
    }
    str_append(fm->current_path, name, 256);
    filemanager_read_dir(fm);
}

static void filemanager_go_up(FileManagerState* fm) {
    int len = zenith::slen(fm->current_path);
    if (len <= 3) return; // "0:/" is root

    // Remove trailing slash if present
    if (len > 0 && fm->current_path[len - 1] == '/') {
        fm->current_path[len - 1] = '\0';
        len--;
    }

    // Find last slash
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (fm->current_path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash >= 0) {
        fm->current_path[last_slash + 1] = '\0';
    }
    filemanager_read_dir(fm);
}

static void filemanager_on_draw(Window* win, Framebuffer& fb) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Rect cr = win->content_rect();
    int cw = cr.w;
    int ch = cr.h;

    // Render to content buffer
    uint32_t* pixels = win->content;
    uint32_t bg_px = colors::WINDOW_BG.to_pixel();
    for (int i = 0; i < cw * ch; i++) pixels[i] = bg_px;

    // Draw path bar at top (24px tall, light gray background)
    uint32_t pathbar_px = Color::from_rgb(0xF0, 0xF0, 0xF0).to_pixel();
    for (int y = 0; y < 24 && y < ch; y++)
        for (int x = 0; x < cw; x++)
            pixels[y * cw + x] = pathbar_px;

    // Draw path text
    {
        int tx = 8;
        int ty = 4;
        uint32_t fg_px = colors::TEXT_COLOR.to_pixel();
        for (int ci = 0; fm->current_path[ci] && tx + FONT_WIDTH <= cw; ci++) {
            const uint8_t* glyph = &font_data[(unsigned char)fm->current_path[ci] * FONT_HEIGHT];
            for (int fy = 0; fy < FONT_HEIGHT && ty + fy < ch; fy++) {
                uint8_t bits = glyph[fy];
                for (int fx = 0; fx < FONT_WIDTH; fx++) {
                    if (bits & (0x80 >> fx)) {
                        int dx = tx + fx;
                        int dy = ty + fy;
                        if (dx < cw && dy < ch)
                            pixels[dy * cw + dx] = fg_px;
                    }
                }
            }
            tx += FONT_WIDTH;
        }
    }

    // Draw separator line
    uint32_t sep_px = colors::BORDER.to_pixel();
    if (24 < ch) {
        for (int x = 0; x < cw; x++)
            pixels[24 * cw + x] = sep_px;
    }

    // Draw file entries
    int item_height = 24;
    int start_y = 26;
    int visible_items = (ch - start_y) / item_height;

    for (int i = fm->scroll_offset; i < fm->entry_count && (i - fm->scroll_offset) < visible_items; i++) {
        int iy = start_y + (i - fm->scroll_offset) * item_height;
        if (iy + item_height > ch) break;

        // Highlight selected
        if (i == fm->selected) {
            uint32_t sel_px = colors::MENU_HOVER.to_pixel();
            for (int y = iy; y < iy + item_height && y < ch; y++)
                for (int x = 0; x < cw; x++)
                    pixels[y * cw + x] = sel_px;
        }

        // Draw icon placeholder (small colored square)
        uint32_t icon_px;
        if (fm->is_dir[i]) {
            icon_px = Color::from_rgb(0xFF, 0xBD, 0x2E).to_pixel(); // folder yellow
        } else {
            icon_px = Color::from_rgb(0x90, 0x90, 0x90).to_pixel(); // file gray
        }
        int icon_x = 8;
        int icon_y = iy + 4;
        for (int dy = 0; dy < 16 && icon_y + dy < ch; dy++)
            for (int dx = 0; dx < 16 && icon_x + dx < cw; dx++)
                pixels[(icon_y + dy) * cw + (icon_x + dx)] = icon_px;

        // Draw entry name
        int tx = 30;
        int ty = iy + 4;
        uint32_t text_px = colors::TEXT_COLOR.to_pixel();
        for (int ci = 0; fm->entry_names[i][ci] && tx + FONT_WIDTH <= cw; ci++) {
            const uint8_t* glyph = &font_data[(unsigned char)fm->entry_names[i][ci] * FONT_HEIGHT];
            for (int fy = 0; fy < FONT_HEIGHT && ty + fy < ch; fy++) {
                uint8_t bits = glyph[fy];
                for (int fx = 0; fx < FONT_WIDTH; fx++) {
                    if (bits & (0x80 >> fx)) {
                        int dx = tx + fx;
                        int dy = ty + fy;
                        if (dx < cw && dy < ch)
                            pixels[dy * cw + dx] = text_px;
                    }
                }
            }
            tx += FONT_WIDTH;
        }
    }
}

static void filemanager_on_mouse(Window* win, MouseEvent& ev) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Rect cr = win->content_rect();
    int local_y = ev.y - cr.y;

    if (ev.left_pressed()) {
        int item_height = 24;
        int start_y = 26;
        int clicked_idx = fm->scroll_offset + (local_y - start_y) / item_height;

        if (local_y >= start_y && clicked_idx >= 0 && clicked_idx < fm->entry_count) {
            if (fm->selected == clicked_idx) {
                // Double-click: navigate into directory
                if (fm->is_dir[clicked_idx]) {
                    filemanager_navigate(fm, fm->entry_names[clicked_idx]);
                }
            } else {
                fm->selected = clicked_idx;
            }
        }
    }

    // Scroll handling
    if (ev.scroll != 0) {
        fm->scroll_offset -= ev.scroll;
        if (fm->scroll_offset < 0) fm->scroll_offset = 0;
        int max_off = fm->entry_count - ((cr.h - 26) / 24);
        if (max_off < 0) max_off = 0;
        if (fm->scroll_offset > max_off) fm->scroll_offset = max_off;
    }
}

static void filemanager_on_key(Window* win, const Zenith::KeyEvent& key) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm || !key.pressed) return;

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        filemanager_go_up(fm);
    } else if (key.scancode == 0x48) {
        // Up arrow
        if (fm->selected > 0) fm->selected--;
    } else if (key.scancode == 0x50) {
        // Down arrow
        if (fm->selected < fm->entry_count - 1) fm->selected++;
    } else if (key.ascii == '\n' || key.ascii == '\r') {
        if (fm->selected >= 0 && fm->selected < fm->entry_count) {
            if (fm->is_dir[fm->selected]) {
                filemanager_navigate(fm, fm->entry_names[fm->selected]);
            }
        }
    }
}

static void filemanager_on_close(Window* win) {
    if (win->app_data) {
        zenith::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// System Info Application
// ============================================================================

struct SysInfoState {
    Zenith::SysInfo sys_info;
    Zenith::NetCfg net_cfg;
    uint64_t uptime_ms;
};

static void draw_text_to_pixels(uint32_t* pixels, int pw, int ph,
                                int tx, int ty, const char* text, uint32_t color_px) {
    for (int i = 0; text[i] && tx + (i + 1) * FONT_WIDTH <= pw; i++) {
        const uint8_t* glyph = &font_data[(unsigned char)text[i] * FONT_HEIGHT];
        int cx = tx + i * FONT_WIDTH;
        for (int fy = 0; fy < FONT_HEIGHT && ty + fy < ph; fy++) {
            uint8_t bits = glyph[fy];
            for (int fx = 0; fx < FONT_WIDTH; fx++) {
                if (bits & (0x80 >> fx)) {
                    int dx = cx + fx;
                    int dy = ty + fy;
                    if (dx >= 0 && dx < pw && dy >= 0 && dy < ph)
                        pixels[dy * pw + dx] = color_px;
                }
            }
        }
    }
}

static void sysinfo_on_draw(Window* win, Framebuffer& fb) {
    SysInfoState* si = (SysInfoState*)win->app_data;
    if (!si) return;

    // Refresh uptime
    si->uptime_ms = zenith::get_milliseconds();

    Rect cr = win->content_rect();
    int cw = cr.w;
    int ch = cr.h;
    uint32_t* pixels = win->content;

    // Fill background
    uint32_t bg_px = colors::WINDOW_BG.to_pixel();
    for (int i = 0; i < cw * ch; i++) pixels[i] = bg_px;

    uint32_t text_px = colors::TEXT_COLOR.to_pixel();
    uint32_t accent_px = colors::ACCENT.to_pixel();
    int y = 16;
    int x = 16;
    char line[128];

    // Title
    draw_text_to_pixels(pixels, cw, ch, x, y, "System Information", accent_px);
    y += FONT_HEIGHT + 12;

    // Separator
    uint32_t sep_px = colors::BORDER.to_pixel();
    for (int sx = x; sx < cw - x && y < ch; sx++)
        pixels[y * cw + sx] = sep_px;
    y += 8;

    // OS Name
    snprintf(line, sizeof(line), "OS:       %s", si->sys_info.osName);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // OS Version
    snprintf(line, sizeof(line), "Version:  %s", si->sys_info.osVersion);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // API Version
    snprintf(line, sizeof(line), "API:      %d", (int)si->sys_info.apiVersion);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // Max Processes
    snprintf(line, sizeof(line), "Max PIDs: %d", (int)si->sys_info.maxProcesses);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 12;

    // Uptime
    int up_sec = (int)(si->uptime_ms / 1000);
    int up_min = up_sec / 60;
    int up_hr = up_min / 60;
    snprintf(line, sizeof(line), "Uptime:   %d:%02d:%02d", up_hr, up_min % 60, up_sec % 60);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 12;

    // Network section
    draw_text_to_pixels(pixels, cw, ch, x, y, "Network", accent_px);
    y += FONT_HEIGHT + 8;

    for (int sx = x; sx < cw - x && y < ch; sx++)
        pixels[y * cw + sx] = sep_px;
    y += 8;

    // IP Address
    uint32_t ip = si->net_cfg.ipAddress;
    snprintf(line, sizeof(line), "IP:       %d.%d.%d.%d",
        (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
        (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // Subnet
    uint32_t mask = si->net_cfg.subnetMask;
    snprintf(line, sizeof(line), "Subnet:   %d.%d.%d.%d",
        (int)(mask & 0xFF), (int)((mask >> 8) & 0xFF),
        (int)((mask >> 16) & 0xFF), (int)((mask >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // Gateway
    uint32_t gw = si->net_cfg.gateway;
    snprintf(line, sizeof(line), "Gateway:  %d.%d.%d.%d",
        (int)(gw & 0xFF), (int)((gw >> 8) & 0xFF),
        (int)((gw >> 16) & 0xFF), (int)((gw >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // DNS
    uint32_t dns = si->net_cfg.dnsServer;
    snprintf(line, sizeof(line), "DNS:      %d.%d.%d.%d",
        (int)(dns & 0xFF), (int)((dns >> 8) & 0xFF),
        (int)((dns >> 16) & 0xFF), (int)((dns >> 24) & 0xFF));
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
    y += FONT_HEIGHT + 6;

    // MAC Address
    snprintf(line, sizeof(line), "MAC:      %02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned)si->net_cfg.macAddress[0], (unsigned)si->net_cfg.macAddress[1],
        (unsigned)si->net_cfg.macAddress[2], (unsigned)si->net_cfg.macAddress[3],
        (unsigned)si->net_cfg.macAddress[4], (unsigned)si->net_cfg.macAddress[5]);
    draw_text_to_pixels(pixels, cw, ch, x, y, line, text_px);
}

static void sysinfo_on_close(Window* win) {
    if (win->app_data) {
        zenith::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Terminal Application
// ============================================================================

static void terminal_on_draw(Window* win, Framebuffer& fb) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (!ts) return;

    Rect cr = win->content_rect();
    terminal_render(ts, win->content, cr.w, cr.h);
}

static void terminal_on_mouse(Window* win, MouseEvent& ev) {
    // Terminal doesn't need mouse handling for now
}

static void terminal_on_key(Window* win, const Zenith::KeyEvent& key) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (!ts) return;
    terminal_handle_key(ts, key);
}

static void terminal_on_close(Window* win) {
    TerminalState* ts = (TerminalState*)win->app_data;
    if (ts) {
        if (ts->cells) zenith::mfree(ts->cells);
        zenith::mfree(ts);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Application Launchers
// ============================================================================

static DesktopState* g_desktop;

static void open_terminal(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Terminal", 200, 80, 648, 480);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    Rect cr = win->content_rect();
    int cols = cr.w / FONT_WIDTH;
    int rows = cr.h / FONT_HEIGHT;

    TerminalState* ts = (TerminalState*)zenith::malloc(sizeof(TerminalState));
    zenith::memset(ts, 0, sizeof(TerminalState));
    terminal_init(ts, cols, rows);

    win->app_data = ts;
    win->on_draw = terminal_on_draw;
    win->on_mouse = terminal_on_mouse;
    win->on_key = terminal_on_key;
    win->on_close = terminal_on_close;
}

static void open_filemanager(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Files", 150, 120, 500, 400);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    FileManagerState* fm = (FileManagerState*)zenith::malloc(sizeof(FileManagerState));
    zenith::memset(fm, 0, sizeof(FileManagerState));
    zenith::strcpy(fm->current_path, "0:/");
    fm->selected = -1;
    filemanager_read_dir(fm);

    win->app_data = fm;
    win->on_draw = filemanager_on_draw;
    win->on_mouse = filemanager_on_mouse;
    win->on_key = filemanager_on_key;
    win->on_close = filemanager_on_close;
}

static void open_sysinfo(DesktopState* ds) {
    int idx = desktop_create_window(ds, "System Info", 300, 100, 400, 380);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    SysInfoState* si = (SysInfoState*)zenith::malloc(sizeof(SysInfoState));
    zenith::memset(si, 0, sizeof(SysInfoState));
    zenith::get_info(&si->sys_info);
    zenith::get_netcfg(&si->net_cfg);
    si->uptime_ms = zenith::get_milliseconds();

    win->app_data = si;
    win->on_draw = sysinfo_on_draw;
    win->on_mouse = nullptr;
    win->on_key = nullptr;
    win->on_close = sysinfo_on_close;
}

// App menu click callbacks
static void on_click_terminal(void* ud) {
    DesktopState* ds = (DesktopState*)ud;
    open_terminal(ds);
    ds->app_menu_open = false;
}

static void on_click_filemanager(void* ud) {
    DesktopState* ds = (DesktopState*)ud;
    open_filemanager(ds);
    ds->app_menu_open = false;
}

static void on_click_sysinfo(void* ud) {
    DesktopState* ds = (DesktopState*)ud;
    open_sysinfo(ds);
    ds->app_menu_open = false;
}

// ============================================================================
// Desktop Implementation
// ============================================================================

void gui::desktop_init(DesktopState* ds) {
    ds->screen_w = ds->fb.width();
    ds->screen_h = ds->fb.height();

    // Immediately clear the screen to hide flanterm boot text
    ds->fb.clear(colors::DESKTOP_BG);
    ds->fb.flip();

    ds->window_count = 0;
    ds->focused_window = -1;
    ds->prev_buttons = 0;
    ds->app_menu_open = false;

    zenith::memset(&ds->mouse, 0, sizeof(Zenith::MouseState));
    zenith::set_mouse_bounds(ds->screen_w - 1, ds->screen_h - 1);

    // Load SVG icons (filenames match those copied by scripts/copy_icons.sh)
    ds->icon_terminal    = svg_load("0:/icons/utilities-terminal-symbolic.svg",        20, 20, colors::ICON_COLOR);
    ds->icon_filemanager = svg_load("0:/icons/system-file-manager-symbolic.svg",       20, 20, colors::ICON_COLOR);
    ds->icon_sysinfo     = svg_load("0:/icons/preferences-desktop-apps-symbolic.svg",  20, 20, colors::ICON_COLOR);
    ds->icon_appmenu     = svg_load("0:/icons/view-app-grid-symbolic.svg",             20, 20, colors::PANEL_TEXT);
    ds->icon_folder      = svg_load("0:/icons/folder-symbolic.svg",                    16, 16, Color::from_rgb(0xFF, 0xBD, 0x2E));
    ds->icon_file        = svg_load("0:/icons/text-x-generic-symbolic.svg",            16, 16, colors::ICON_COLOR);
    ds->icon_computer    = svg_load("0:/icons/computer-symbolic.svg",                  20, 20, colors::ICON_COLOR);

    // Open initial terminal window
    open_terminal(ds);
}

int gui::desktop_create_window(DesktopState* ds, const char* title, int x, int y, int w, int h) {
    if (ds->window_count >= MAX_WINDOWS) return -1;

    int idx = ds->window_count;
    Window* win = &ds->windows[idx];
    zenith::memset(win, 0, sizeof(Window));

    zenith::strncpy(win->title, title, MAX_TITLE_LEN);
    win->frame = {x, y, w, h};
    win->state = WIN_NORMAL;
    win->z_order = idx;
    win->focused = true;
    win->dirty = true;
    win->dragging = false;
    win->resizing = false;
    win->saved_frame = win->frame;

    // Allocate content buffer
    Rect cr = win->content_rect();
    win->content_w = cr.w;
    win->content_h = cr.h;
    int buf_size = cr.w * cr.h * 4;
    win->content = (uint32_t*)zenith::alloc(buf_size);
    zenith::memset(win->content, 0xFF, buf_size);

    win->on_draw = nullptr;
    win->on_mouse = nullptr;
    win->on_key = nullptr;
    win->on_close = nullptr;
    win->app_data = nullptr;

    // Unfocus previous window
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        ds->windows[ds->focused_window].focused = false;
    }
    ds->focused_window = idx;
    ds->window_count++;

    return idx;
}

void gui::desktop_close_window(DesktopState* ds, int idx) {
    if (idx < 0 || idx >= ds->window_count) return;

    Window* win = &ds->windows[idx];
    if (win->on_close) win->on_close(win);

    // Free content buffer
    if (win->content) {
        zenith::free(win->content);
        win->content = nullptr;
    }

    // Shift remaining windows down
    for (int i = idx; i < ds->window_count - 1; i++) {
        ds->windows[i] = ds->windows[i + 1];
    }
    ds->window_count--;

    // Fix focused window index
    if (ds->focused_window == idx) {
        ds->focused_window = ds->window_count > 0 ? ds->window_count - 1 : -1;
    } else if (ds->focused_window > idx) {
        ds->focused_window--;
    }

    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        ds->windows[ds->focused_window].focused = true;
    }
}

void gui::desktop_raise_window(DesktopState* ds, int idx) {
    if (idx < 0 || idx >= ds->window_count) return;
    if (idx == ds->window_count - 1) {
        // Already on top, just focus
        if (ds->focused_window >= 0 && ds->focused_window < ds->window_count)
            ds->windows[ds->focused_window].focused = false;
        ds->focused_window = idx;
        ds->windows[idx].focused = true;
        return;
    }

    // Unfocus current
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        ds->windows[ds->focused_window].focused = false;
    }

    // Move window to end (top of stack)
    Window tmp = ds->windows[idx];
    for (int i = idx; i < ds->window_count - 1; i++) {
        ds->windows[i] = ds->windows[i + 1];
    }
    ds->windows[ds->window_count - 1] = tmp;

    ds->focused_window = ds->window_count - 1;
    ds->windows[ds->focused_window].focused = true;
}

void gui::desktop_draw_window(DesktopState* ds, int idx) {
    if (idx < 0 || idx >= ds->window_count) return;
    Window* win = &ds->windows[idx];
    if (win->state == WIN_MINIMIZED || win->state == WIN_CLOSED) return;

    Framebuffer& fb = ds->fb;
    int x = win->frame.x;
    int y = win->frame.y;
    int w = win->frame.w;
    int h = win->frame.h;

    // Draw shadow
    draw_shadow(fb, x, y, w, h, SHADOW_SIZE, colors::SHADOW);

    // Draw window body
    fb.fill_rect(x, y, w, h, colors::WINDOW_BG);

    // Draw titlebar
    Color tb_bg = win->focused ? colors::TITLEBAR_BG : Color::from_rgb(0xE8, 0xE8, 0xE8);
    fb.fill_rect(x, y, w, TITLEBAR_HEIGHT, tb_bg);

    // Draw border
    draw_rect(fb, x, y, w, h, colors::BORDER);
    // Titlebar bottom separator
    draw_hline(fb, x, y + TITLEBAR_HEIGHT - 1, w, colors::BORDER);

    // Draw window buttons (macOS style: close, minimize, maximize)
    Rect close_r = win->close_btn_rect();
    Rect min_r = win->min_btn_rect();
    Rect max_r = win->max_btn_rect();

    fill_circle(fb, close_r.x + BTN_RADIUS, close_r.y + BTN_RADIUS, BTN_RADIUS, colors::CLOSE_BTN);
    fill_circle(fb, min_r.x + BTN_RADIUS, min_r.y + BTN_RADIUS, BTN_RADIUS, colors::MIN_BTN);
    fill_circle(fb, max_r.x + BTN_RADIUS, max_r.y + BTN_RADIUS, BTN_RADIUS, colors::MAX_BTN);

    // Draw title text centered in titlebar (after buttons)
    int title_x = x + 12 + 44 + BTN_RADIUS * 2 + 12; // after buttons
    int title_y = y + (TITLEBAR_HEIGHT - FONT_HEIGHT) / 2;
    int title_w = text_width(win->title);
    // Center in remaining space
    int remaining_w = w - (title_x - x) - 12;
    if (remaining_w > title_w) {
        title_x += (remaining_w - title_w) / 2;
    }
    draw_text(fb, title_x, title_y, win->title, colors::TEXT_COLOR);

    // Call app draw callback to render content
    if (win->on_draw) {
        win->on_draw(win, fb);
    }

    // Blit content buffer to framebuffer
    Rect cr = win->content_rect();
    if (win->content) {
        fb.blit(cr.x, cr.y, cr.w, cr.h, win->content);
    }
}

void gui::desktop_draw_panel(DesktopState* ds) {
    Framebuffer& fb = ds->fb;
    int sw = ds->screen_w;

    // Panel background
    fb.fill_rect(0, 0, sw, PANEL_HEIGHT, colors::PANEL_BG);

    // App menu button (left side)
    int btn_x = 4;
    int btn_y = 2;
    int btn_w = 28;
    int btn_h = 28;

    // Draw grid icon for app menu (3x3 dots)
    if (ds->icon_appmenu.pixels) {
        int ix = btn_x + (btn_w - ds->icon_appmenu.width) / 2;
        int iy = btn_y + (btn_h - ds->icon_appmenu.height) / 2;
        fb.blit_alpha(ix, iy, ds->icon_appmenu.width, ds->icon_appmenu.height, ds->icon_appmenu.pixels);
    } else {
        // Fallback: draw 3x3 grid of small squares
        for (int gr = 0; gr < 3; gr++) {
            for (int gc = 0; gc < 3; gc++) {
                int dx = btn_x + 6 + gc * 6;
                int dy = btn_y + 6 + gr * 6;
                fb.fill_rect(dx, dy, 3, 3, colors::PANEL_TEXT);
            }
        }
    }

    // Window indicator buttons (center area)
    int indicator_x = 40;
    for (int i = 0; i < ds->window_count; i++) {
        Window* win = &ds->windows[i];
        if (win->state == WIN_CLOSED) continue;

        int tw = text_width(win->title);
        int pad = 12;
        int iw = tw + pad * 2;
        if (iw > 150) iw = 150;

        Color btn_bg = (i == ds->focused_window)
            ? Color::from_rgba(0xFF, 0xFF, 0xFF, 0x30)
            : Color::from_rgba(0xFF, 0xFF, 0xFF, 0x10);

        fb.fill_rect_alpha(indicator_x, 4, iw, 24, btn_bg);

        // Truncate title if too long
        char short_title[20];
        zenith::strncpy(short_title, win->title, 18);

        int tx = indicator_x + pad;
        int ty = 4 + (24 - FONT_HEIGHT) / 2;
        draw_text(fb, tx, ty, short_title, colors::PANEL_TEXT);

        indicator_x += iw + 4;
    }

    // Clock (right side)
    Zenith::DateTime dt;
    zenith::gettime(&dt);
    char clock_str[8];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d", (int)dt.Hour, (int)dt.Minute);
    int clock_w = text_width(clock_str);
    int clock_x = sw - clock_w - 12;
    int clock_y = (PANEL_HEIGHT - FONT_HEIGHT) / 2;
    draw_text(fb, clock_x, clock_y, clock_str, colors::PANEL_TEXT);
}

static void desktop_draw_app_menu(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    int menu_x = 4;
    int menu_y = PANEL_HEIGHT + 2;
    int menu_w = 200;
    int item_h = 36;
    int menu_h = item_h * 3 + 8;

    // Menu shadow
    draw_shadow(fb, menu_x, menu_y, menu_w, menu_h, 4, colors::SHADOW);

    // Menu background
    fb.fill_rect(menu_x, menu_y, menu_w, menu_h, colors::MENU_BG);
    draw_rect(fb, menu_x, menu_y, menu_w, menu_h, colors::BORDER);

    // Menu items
    struct MenuItem {
        const char* label;
        SvgIcon* icon;
    };
    MenuItem items[3] = {
        { "Terminal",     &ds->icon_terminal },
        { "Files",        &ds->icon_filemanager },
        { "System Info",  &ds->icon_sysinfo }
    };

    int mx = ds->mouse.x;
    int my = ds->mouse.y;

    for (int i = 0; i < 3; i++) {
        int iy = menu_y + 4 + i * item_h;
        Rect item_rect = {menu_x + 4, iy, menu_w - 8, item_h};

        // Hover highlight
        if (item_rect.contains(mx, my)) {
            fill_rounded_rect(fb, item_rect.x, item_rect.y, item_rect.w, item_rect.h, 4, colors::MENU_HOVER);
        }

        // Icon
        int icon_x = item_rect.x + 8;
        int icon_y = item_rect.y + (item_h - 20) / 2;
        if (items[i].icon && items[i].icon->pixels) {
            fb.blit_alpha(icon_x, icon_y, items[i].icon->width, items[i].icon->height, items[i].icon->pixels);
        }

        // Label
        int tx = icon_x + 28;
        int ty = item_rect.y + (item_h - FONT_HEIGHT) / 2;
        draw_text(fb, tx, ty, items[i].label, colors::TEXT_COLOR);
    }
}

void gui::desktop_compose(DesktopState* ds) {
    Framebuffer& fb = ds->fb;

    // Desktop background
    fb.clear(colors::DESKTOP_BG);

    // Draw windows from bottom to top (index 0 = bottom)
    for (int i = 0; i < ds->window_count; i++) {
        if (ds->windows[i].state != WIN_MINIMIZED && ds->windows[i].state != WIN_CLOSED) {
            desktop_draw_window(ds, i);
        }
    }

    // Draw panel on top of everything
    desktop_draw_panel(ds);

    // Draw app menu if open
    if (ds->app_menu_open) {
        desktop_draw_app_menu(ds);
    }

    // Draw cursor last (on top of everything)
    draw_cursor(fb, ds->mouse.x, ds->mouse.y);
}

void gui::desktop_handle_mouse(DesktopState* ds) {
    int mx = ds->mouse.x;
    int my = ds->mouse.y;
    uint8_t buttons = ds->mouse.buttons;
    uint8_t prev = ds->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);
    bool left_held = (buttons & 0x01);
    bool left_released = !(buttons & 0x01) && (prev & 0x01);

    // Build mouse event
    MouseEvent ev;
    ev.x = mx;
    ev.y = my;
    ev.buttons = buttons;
    ev.prev_buttons = prev;
    ev.scroll = ds->mouse.scrollDelta;

    // Check for ongoing window drags first
    for (int i = 0; i < ds->window_count; i++) {
        Window* win = &ds->windows[i];
        if (win->dragging) {
            if (left_held) {
                win->frame.x = mx - win->drag_offset_x;
                win->frame.y = my - win->drag_offset_y;
                // Clamp to screen
                if (win->frame.x < -win->frame.w + 50) win->frame.x = -win->frame.w + 50;
                if (win->frame.y < 0) win->frame.y = 0;
                if (win->frame.x > ds->screen_w - 50) win->frame.x = ds->screen_w - 50;
                if (win->frame.y > ds->screen_h - 50) win->frame.y = ds->screen_h - 50;
            }
            if (left_released) {
                win->dragging = false;
            }
            return;
        }
    }

    // Handle app menu clicks
    if (ds->app_menu_open && left_pressed) {
        int menu_x = 4;
        int menu_y = PANEL_HEIGHT + 2;
        int menu_w = 200;
        int item_h = 36;
        int menu_h = item_h * 3 + 8;
        Rect menu_rect = {menu_x, menu_y, menu_w, menu_h};

        if (menu_rect.contains(mx, my)) {
            // Which item was clicked?
            int rel_y = my - menu_y - 4;
            int item_idx = rel_y / item_h;
            if (item_idx >= 0 && item_idx < 3) {
                switch (item_idx) {
                case 0: on_click_terminal(ds); break;
                case 1: on_click_filemanager(ds); break;
                case 2: on_click_sysinfo(ds); break;
                }
            }
            return;
        } else {
            ds->app_menu_open = false;
            // Fall through to handle click on other things
        }
    }

    // Panel click check
    if (left_pressed && my < PANEL_HEIGHT) {
        // App menu button
        if (mx < 36) {
            ds->app_menu_open = !ds->app_menu_open;
            return;
        }

        // Window indicator buttons
        int indicator_x = 40;
        for (int i = 0; i < ds->window_count; i++) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_CLOSED) continue;

            int tw = text_width(win->title);
            int pad = 12;
            int iw = tw + pad * 2;
            if (iw > 150) iw = 150;

            Rect btn_rect = {indicator_x, 4, iw, 24};
            if (btn_rect.contains(mx, my)) {
                if (win->state == WIN_MINIMIZED) {
                    win->state = WIN_NORMAL;
                }
                desktop_raise_window(ds, i);
                return;
            }
            indicator_x += iw + 4;
        }
        return;
    }

    // Window interaction: check from top (last) to bottom (first)
    if (left_pressed) {
        for (int i = ds->window_count - 1; i >= 0; i--) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_MINIMIZED || win->state == WIN_CLOSED) continue;

            // Check close button
            Rect close_r = win->close_btn_rect();
            if (close_r.contains(mx, my)) {
                desktop_close_window(ds, i);
                return;
            }

            // Check minimize button
            Rect min_r = win->min_btn_rect();
            if (min_r.contains(mx, my)) {
                win->state = WIN_MINIMIZED;
                if (ds->focused_window == i) {
                    ds->focused_window = -1;
                    // Focus next visible window
                    for (int j = ds->window_count - 1; j >= 0; j--) {
                        if (ds->windows[j].state == WIN_NORMAL || ds->windows[j].state == WIN_MAXIMIZED) {
                            ds->focused_window = j;
                            ds->windows[j].focused = true;
                            break;
                        }
                    }
                }
                return;
            }

            // Check maximize button
            Rect max_r = win->max_btn_rect();
            if (max_r.contains(mx, my)) {
                if (win->state == WIN_MAXIMIZED) {
                    // Restore
                    win->frame = win->saved_frame;
                    win->state = WIN_NORMAL;
                } else {
                    // Maximize
                    win->saved_frame = win->frame;
                    win->frame = {0, PANEL_HEIGHT, ds->screen_w, ds->screen_h - PANEL_HEIGHT};
                    win->state = WIN_MAXIMIZED;
                }
                // Reallocate content buffer for new size
                Rect cr = win->content_rect();
                if (cr.w != win->content_w || cr.h != win->content_h) {
                    if (win->content) zenith::free(win->content);
                    win->content_w = cr.w;
                    win->content_h = cr.h;
                    win->content = (uint32_t*)zenith::alloc(cr.w * cr.h * 4);
                    zenith::memset(win->content, 0xFF, cr.w * cr.h * 4);
                }
                desktop_raise_window(ds, i);
                return;
            }

            // Check titlebar (start drag)
            Rect tb = win->titlebar_rect();
            if (tb.contains(mx, my)) {
                win->dragging = true;
                win->drag_offset_x = mx - win->frame.x;
                win->drag_offset_y = my - win->frame.y;
                desktop_raise_window(ds, ds->window_count - 1 == i ? i : i);
                // After raise, this window is at the end
                // Need to update the reference since raise moved it
                int new_idx = ds->window_count - 1;
                ds->windows[new_idx].dragging = true;
                ds->windows[new_idx].drag_offset_x = mx - ds->windows[new_idx].frame.x;
                ds->windows[new_idx].drag_offset_y = my - ds->windows[new_idx].frame.y;
                return;
            }

            // Check content area
            Rect cr = win->content_rect();
            if (cr.contains(mx, my)) {
                desktop_raise_window(ds, i);
                // Dispatch mouse event to app
                int new_idx = ds->window_count - 1;
                if (ds->windows[new_idx].on_mouse) {
                    ev.x = mx;
                    ev.y = my;
                    ds->windows[new_idx].on_mouse(&ds->windows[new_idx], ev);
                }
                return;
            }

            // Check full frame (catches border clicks)
            if (win->frame.contains(mx, my)) {
                desktop_raise_window(ds, i);
                return;
            }
        }

        // Clicked on desktop background - close app menu
        ds->app_menu_open = false;
    }

    // Handle scroll events on focused window
    if (ev.scroll != 0 && ds->focused_window >= 0) {
        Window* win = &ds->windows[ds->focused_window];
        Rect cr = win->content_rect();
        if (cr.contains(mx, my) && win->on_mouse) {
            win->on_mouse(win, ev);
        }
    }
}

void gui::desktop_handle_keyboard(DesktopState* ds, const Zenith::KeyEvent& key) {
    if (!key.pressed) return;

    // Global shortcuts
    if (key.ctrl && key.alt) {
        if (key.ascii == 't' || key.ascii == 'T') {
            open_terminal(ds);
            return;
        }
        if (key.ascii == 'f' || key.ascii == 'F') {
            open_filemanager(ds);
            return;
        }
        if (key.ascii == 'i' || key.ascii == 'I') {
            open_sysinfo(ds);
            return;
        }
    }

    // Dispatch to focused window
    if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
        Window* win = &ds->windows[ds->focused_window];
        if (win->on_key) {
            win->on_key(win, key);
        }
    }
}

void gui::desktop_run(DesktopState* ds) {
    for (;;) {
        // Poll mouse state
        ds->prev_buttons = ds->mouse.buttons;
        zenith::mouse_state(&ds->mouse);

        // Poll keyboard events
        while (zenith::is_key_available()) {
            Zenith::KeyEvent key;
            zenith::getkey(&key);
            desktop_handle_keyboard(ds, key);
        }

        // Poll terminal I/O for all terminal windows
        for (int i = 0; i < ds->window_count; i++) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_CLOSED) continue;
            if (win->app_data && win->on_draw == terminal_on_draw) {
                TerminalState* ts = (TerminalState*)win->app_data;
                terminal_poll(ts);
            }
        }

        // Handle mouse events
        desktop_handle_mouse(ds);

        // Compose and present
        desktop_compose(ds);
        ds->fb.flip();

        // Target ~60fps
        zenith::sleep_ms(16);
    }
}

// ============================================================================
// Entry Point
// ============================================================================

extern "C" void _start() {
    DesktopState* ds = (DesktopState*)zenith::malloc(sizeof(DesktopState));
    zenith::memset(ds, 0, sizeof(DesktopState));

    // Placement-new the Framebuffer since it has a constructor
    new (&ds->fb) Framebuffer();

    g_desktop = ds;

    desktop_init(ds);
    desktop_run(ds);

    zenith::exit(0);
}
