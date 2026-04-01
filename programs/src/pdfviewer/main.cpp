/*
 * main.cpp
 * PDF Viewer app -- global state, entry point, event loop
 * Copyright (c) 2026 Daniel Hammer
 */

#include "pdfviewer.h"
#include <gui/dialogs.hpp>
#include <gui/standalone.hpp>

// ============================================================================
// Global state definitions
// ============================================================================

int g_win_w = INIT_W;
int g_win_h = INIT_H;

PdfDoc g_doc = {};

int g_current_page = 0;
int g_scroll_y = 0;
float g_zoom = 1.0f;

char g_filepath[256] = {};

bool g_pathbar_open = false;
char g_pathbar_text[256] = {};
int  g_pathbar_len = 0;
int  g_pathbar_cursor = 0;

TrueTypeFont* g_font = nullptr;
TrueTypeFont* g_font_bold = nullptr;
TrueTypeFont* g_font_mono = nullptr;

char g_status_msg[128] = {};

// ============================================================================
// Toolbar hit-testing constants
// ============================================================================

// Toolbar buttons: Open | Print | < > | - +
static constexpr int TB_OPEN_X0 = 4, TB_OPEN_X1 = 40;
static constexpr int TB_PRINT_X0 = 44, TB_PRINT_X1 = 80;
// separator at 88
static constexpr int TB_PREV_X0 = 96, TB_PREV_X1 = 120;
static constexpr int TB_NEXT_X0 = 124, TB_NEXT_X1 = 148;
// separator at 156
static constexpr int TB_ZOUT_X0 = 164, TB_ZOUT_X1 = 188;
static constexpr int TB_ZIN_X0  = 192, TB_ZIN_X1  = 216;

// ============================================================================
// Helpers
// ============================================================================

static int max_scroll() {
    if (!g_doc.valid || g_current_page >= g_doc.page_count) return 0;
    PdfPage* page = &g_doc.pages[g_current_page];
    int page_h = (int)(page->height * g_zoom) + PAGE_MARGIN * 2;
    int pathbar_h = g_pathbar_open ? PATHBAR_H : 0;
    int content_h = g_win_h - TOOLBAR_H - pathbar_h - STATUS_BAR_H;
    int ms = page_h - content_h;
    return ms > 0 ? ms : 0;
}

static void clamp_scroll() {
    int ms = max_scroll();
    if (g_scroll_y < 0) g_scroll_y = 0;
    if (g_scroll_y > ms) g_scroll_y = ms;
}

static const char* file_basename(const char* path) {
    if (!path || !path[0]) return "";
    const char* name = path;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') name = path + i + 1;
    }
    return name;
}

static void go_to_page(int page) {
    if (page < 0) page = 0;
    if (page >= g_doc.page_count) page = g_doc.page_count - 1;
    g_current_page = page;
    g_scroll_y = 0;
}

static void zoom_in() {
    if (g_zoom < 4.0f) {
        g_zoom += 0.25f;
        clamp_scroll();
    }
}

static void zoom_out() {
    if (g_zoom > 0.25f) {
        g_zoom -= 0.25f;
        clamp_scroll();
    }
}

static void open_file(const char* path) {
    str_cpy(g_filepath, path, 256);
    g_current_page = 0;
    g_scroll_y = 0;

    if (load_pdf(path)) {
        // Success
    } else {
        g_filepath[0] = '\0';
    }
}

static void open_inline_pathbar(const char* initial_text) {
    g_pathbar_open = true;
    if (!initial_text) initial_text = "";
    str_cpy(g_pathbar_text, initial_text, sizeof(g_pathbar_text));
    g_pathbar_len = str_len(g_pathbar_text);
    g_pathbar_cursor = g_pathbar_len;
}

static void open_file_dialog() {
    char path[256] = {};
    if (dialogs::open_file("Open PDF", g_filepath, path, sizeof(path), g_status_msg, sizeof(g_status_msg))) {
        open_file(path);
    } else if (g_status_msg[0]) {
        open_inline_pathbar(g_filepath);
    }
}

static void print_file_dialog() {
    if (!g_filepath[0]) {
        str_cpy(g_status_msg, "Open a PDF before printing", sizeof(g_status_msg));
        return;
    }

    char job_id[64] = {};
    char msg[128] = {};
    if (dialogs::print_file("Print PDF", g_filepath, file_basename(g_filepath),
                            job_id, sizeof(job_id), msg, sizeof(msg))) {
        if (msg[0]) str_cpy(g_status_msg, msg, sizeof(g_status_msg));
        else str_cpy(g_status_msg, "Print job queued", sizeof(g_status_msg));
    } else if (msg[0]) {
        str_cpy(g_status_msg, msg, sizeof(g_status_msg));
    }
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Load fonts
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_font = load_font("0:/fonts/Roboto-Medium.ttf");
    g_font_bold = load_font("0:/fonts/Roboto-Bold.ttf");
    g_font_mono = load_font("0:/fonts/JetBrainsMono-Regular.ttf");

    // Check for file argument
    char args[512] = {};
    int arglen = montauk::getargs(args, sizeof(args));
    if (arglen > 0 && args[0]) {
        open_file(args);
    }

    // Build window title
    char title[64] = "PDF Viewer";
    if (g_filepath[0]) {
        const char* fname = file_basename(g_filepath);
        snprintf(title, 64, "%s - PDF Viewer", fname);
    }

    WsWindow win;
    if (!win.create(title, INIT_W, INIT_H))
        montauk::exit(1);

    uint32_t* pixels = win.pixels;

    render(pixels);
    win.present();

    while (true) {
        Montauk::WinEvent ev;
        int r = win.poll(&ev);

        if (r < 0) break;

        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        // Close
        if (ev.type == 3) break;

        // Resize
        if (ev.type == 2) {
            g_win_w = win.width;
            g_win_h = win.height;
            pixels = win.pixels;
            clamp_scroll();
            render(pixels);
            win.present();
            continue;
        }

        bool redraw = false;

        // Keyboard
        if (ev.type == 0 && ev.key.pressed) {
            auto& key = ev.key;

            // Path bar input
            if (g_pathbar_open) {
                if (key.ascii == '\n' || key.ascii == '\r') {
                    if (g_pathbar_text[0]) {
                        open_file(g_pathbar_text);
                    }
                    g_pathbar_open = false;
                } else if (key.scancode == 0x01) {
                    g_pathbar_open = false;
                } else if (key.ascii == '\b' || key.scancode == 0x0E) {
                    if (g_pathbar_cursor > 0) {
                        for (int i = g_pathbar_cursor - 1; i < g_pathbar_len - 1; i++)
                            g_pathbar_text[i] = g_pathbar_text[i + 1];
                        g_pathbar_len--;
                        g_pathbar_cursor--;
                        g_pathbar_text[g_pathbar_len] = '\0';
                    }
                } else if (key.scancode == 0x4B) {
                    if (g_pathbar_cursor > 0) g_pathbar_cursor--;
                } else if (key.scancode == 0x4D) {
                    if (g_pathbar_cursor < g_pathbar_len) g_pathbar_cursor++;
                } else if (key.ascii >= 32 && key.ascii < 127 && g_pathbar_len < 254) {
                    for (int i = g_pathbar_len; i > g_pathbar_cursor; i--)
                        g_pathbar_text[i] = g_pathbar_text[i - 1];
                    g_pathbar_text[g_pathbar_cursor] = key.ascii;
                    g_pathbar_cursor++;
                    g_pathbar_len++;
                    g_pathbar_text[g_pathbar_len] = '\0';
                }
                redraw = true;
                goto done_keys;
            }

            // Escape: close
            if (key.scancode == 0x01) {
                break;
            }
            // Ctrl+O: open file
            else if (key.ctrl && (key.ascii == 'o' || key.ascii == 'O' || key.ascii == 15)) {
                open_file_dialog();
                redraw = true;
            }
            // Ctrl+P: print file
            else if (key.ctrl && (key.ascii == 'p' || key.ascii == 'P' || key.ascii == 16)) {
                print_file_dialog();
                redraw = true;
            }
            // Page Down / Right arrow: next page
            else if (key.scancode == 0x51 || key.scancode == 0x4D) {
                if (g_doc.valid) {
                    if (key.scancode == 0x4D && g_scroll_y < max_scroll()) {
                        // Right arrow scrolls down within page
                    } else {
                        go_to_page(g_current_page + 1);
                    }
                    redraw = true;
                }
            }
            // Page Up / Left arrow: previous page
            else if (key.scancode == 0x49 || key.scancode == 0x4B) {
                if (g_doc.valid) {
                    if (key.scancode == 0x4B && g_scroll_y > 0) {
                        // Left arrow scrolls up within page
                    } else {
                        go_to_page(g_current_page - 1);
                    }
                    redraw = true;
                }
            }
            // Up arrow: scroll up
            else if (key.scancode == 0x48) {
                g_scroll_y -= SCROLL_STEP;
                clamp_scroll();
                redraw = true;
            }
            // Down arrow: scroll down
            else if (key.scancode == 0x50) {
                g_scroll_y += SCROLL_STEP;
                clamp_scroll();
                redraw = true;
            }
            // Home: first page
            else if (key.scancode == 0x47) {
                if (g_doc.valid) { go_to_page(0); redraw = true; }
            }
            // End: last page
            else if (key.scancode == 0x4F) {
                if (g_doc.valid) { go_to_page(g_doc.page_count - 1); redraw = true; }
            }
            // Ctrl++ or Ctrl+=: zoom in
            else if (key.ctrl && (key.ascii == '+' || key.ascii == '=')) {
                zoom_in();
                redraw = true;
            }
            // Ctrl+-: zoom out
            else if (key.ctrl && (key.ascii == '-' || key.ascii == '_')) {
                zoom_out();
                redraw = true;
            }
            // Ctrl+0: reset zoom
            else if (key.ctrl && key.ascii == '0') {
                g_zoom = 1.0f;
                clamp_scroll();
                redraw = true;
            }
        }
        done_keys:

        // Mouse
        if (ev.type == 1) {
            int mx = ev.mouse.x;
            int my = ev.mouse.y;
            uint8_t btns = ev.mouse.buttons;
            uint8_t prev = ev.mouse.prev_buttons;
            bool clicked = (btns & 1) && !(prev & 1);

            if (clicked && my < TOOLBAR_H && my >= TB_BTN_Y && my < TB_BTN_Y + TB_BTN_SIZE) {
                if (mx >= TB_OPEN_X0 && mx < TB_OPEN_X1) {
                    open_file_dialog();
                    redraw = true;
                } else if (mx >= TB_PRINT_X0 && mx < TB_PRINT_X1) {
                    print_file_dialog();
                    redraw = true;
                } else if (mx >= TB_PREV_X0 && mx < TB_PREV_X1 && g_doc.valid) {
                    go_to_page(g_current_page - 1);
                    redraw = true;
                } else if (mx >= TB_NEXT_X0 && mx < TB_NEXT_X1 && g_doc.valid) {
                    go_to_page(g_current_page + 1);
                    redraw = true;
                } else if (mx >= TB_ZOUT_X0 && mx < TB_ZOUT_X1) {
                    zoom_out();
                    redraw = true;
                } else if (mx >= TB_ZIN_X0 && mx < TB_ZIN_X1) {
                    zoom_in();
                    redraw = true;
                }
            }

            // Mouse scroll
            if (ev.mouse.scroll != 0) {
                g_scroll_y -= ev.mouse.scroll * SCROLL_STEP;
                clamp_scroll();
                redraw = true;
            }
        }

        if (redraw) {
            render(pixels);
            win.present();
        }
    }

    free_pdf();
    win.destroy();
    montauk::exit(0);
}
