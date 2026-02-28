/*
    * main.cpp
    * MontaukOS Wikipedia GUI client - standalone Window Server process
    * Fetches articles via TLS (BearSSL), renders with Roboto TTF
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>
#include <tls/tls.hpp>

extern "C" {
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
}

using namespace gui;
using namespace gui::colors;

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W       = 820;
static constexpr int INIT_H       = 580;
static int           TOOLBAR_H    = 42;
static constexpr int SCROLLBAR_W  = 14;
static int           FONT_SIZE    = 18;
static int           TITLE_SIZE   = 32;
static int           SECTION_SIZE = 24;
static constexpr int TEXT_PAD     = 16;
static constexpr int RESP_MAX     = 131072;
static constexpr int MAX_LINES    = 2000;

static const char WIKI_HOST[] = "en.wikipedia.org";

// ============================================================================
// Display line
// ============================================================================

struct WikiLine {
    char         text[256];
    Color        color;
    int          font_size;
    TrueTypeFont* font;   // which font to render with
};

// ============================================================================
// App state
// ============================================================================

enum class AppPhase { IDLE, LOADING, DONE, ERR };

static AppPhase      g_phase      = AppPhase::IDLE;
static char          g_query[256] = {};
static char          g_status[256] = {};
static int           g_scroll_y   = 0;
static int           g_line_count = 0;
static int           g_line_h     = 24;   // updated after font load
static int           g_win_w      = INIT_W;
static int           g_win_h      = INIT_H;
static char          g_title[512] = {};
static int           g_extract_len = 0;

// Large buffers — heap allocated in _start
static WikiLine*     g_lines      = nullptr;
static char*         g_resp_buf   = nullptr;
static char*         g_extract_buf = nullptr;

// Fonts
static TrueTypeFont* g_font       = nullptr;  // Roboto Medium
static TrueTypeFont* g_font_bold  = nullptr;  // Roboto Bold
static TrueTypeFont* g_font_serif = nullptr;  // NotoSerif SemiBold (headings)

// TLS state (lazy-init on first search)
static bool          g_tls_ready  = false;
static uint32_t      g_server_ip  = 0;
static tls::TrustAnchors g_tas = {nullptr, 0, 0};

// ============================================================================
// Pixel buffer helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x,  y0 = y < 0 ? 0 : y;
    int x1 = x + w,           y1 = y + h;
    if (x1 > bw) x1 = bw;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

static void px_hline(uint32_t* px, int bw, int x, int y, int len, Color c) {
    uint32_t v = c.to_pixel();
    int x1 = x + len;
    if (x < 0) x = 0;
    if (x1 > bw) x1 = bw;
    for (int col = x; col < x1; col++)
        px[y * bw + col] = v;
}

static void px_vline(uint32_t* px, int bw, int x, int y, int len, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = y; row < y + len; row++)
        px[row * bw + x] = v;
}

static void px_rect_outline(uint32_t* px, int bw, int x, int y, int w, int h, Color c) {
    px_hline(px, bw, x,         y,         w, c);
    px_hline(px, bw, x,         y + h - 1, w, c);
    px_vline(px, bw, x,         y,         h, c);
    px_vline(px, bw, x + w - 1, y,         h, c);
}

// ============================================================================
// UI scale
// ============================================================================

static void apply_scale(int scale) {
    switch (scale) {
    case 0: FONT_SIZE=14; TITLE_SIZE=26; SECTION_SIZE=20; TOOLBAR_H=34; break;
    case 2: FONT_SIZE=22; TITLE_SIZE=40; SECTION_SIZE=30; TOOLBAR_H=52; break;
    default: FONT_SIZE=18; TITLE_SIZE=32; SECTION_SIZE=24; TOOLBAR_H=42; break;
    }
    if (g_font) g_line_h = g_font->get_line_height(FONT_SIZE) + 4;
}

// ============================================================================
// HTTPS fetch wrapper
// ============================================================================

static int wiki_fetch(const char* path, char* respBuf, int respMax) {
    static char request[2560];
    int reqLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: MontaukOS/1.0 wikipedia\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, WIKI_HOST);
    return tls::https_fetch(WIKI_HOST, g_server_ip, 443,
                            request, reqLen, g_tas,
                            respBuf, respMax);
}

// ============================================================================
// HTTP parsing
// ============================================================================

static int find_header_end(const char* buf, int len) {
    for (int i = 0; i + 3 < len; i++)
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n')
            return i + 4;
    return -1;
}

static int parse_status_code(const char* buf, int len) {
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    if (i >= len || i + 3 >= len) return -1;
    i++;
    if (buf[i] < '0' || buf[i] > '9') return -1;
    return (buf[i]-'0')*100 + (buf[i+1]-'0')*10 + (buf[i+2]-'0');
}

// ============================================================================
// URL encoding
// ============================================================================

static int url_encode_title(const char* in, char* out, int maxLen) {
    const char hex[] = "0123456789ABCDEF";
    int j = 0;
    for (int i = 0; in[i] && j < maxLen - 4; i++) {
        char c = in[i];
        if (c == ' ') {
            out[j++] = '_';
        } else if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                   c=='-'||c=='_'||c=='.'||c=='~'||c=='('||c==')'||c==',') {
            out[j++] = c;
        } else {
            out[j++] = '%';
            out[j++] = hex[(unsigned char)c >> 4];
            out[j++] = hex[(unsigned char)c & 0x0F];
        }
    }
    out[j] = '\0';
    return j;
}

// ============================================================================
// JSON string extraction
// ============================================================================

static int extract_json_string(const char* buf, int len, const char* key,
                                char* out, int maxOut) {
    int klen = (int)strlen(key);
    for (int i = 0; i < len - klen - 3; i++) {
        if (buf[i] != '"') continue;
        if (memcmp(buf + i + 1, key, klen) != 0) continue;
        if (buf[i + 1 + klen] != '"') continue;
        if (buf[i + 2 + klen] != ':') continue;

        int p = i + 3 + klen;
        while (p < len && (buf[p]==' ' || buf[p]=='\t')) p++;
        if (p >= len || buf[p] != '"') continue;
        p++;

        int j = 0;
        while (p < len && j < maxOut - 4) {
            if (buf[p] == '"') break;
            if (buf[p] == '\\' && p + 1 < len) {
                p++;
                switch (buf[p]) {
                case '"':  out[j++] = '"';  break;
                case '\\': out[j++] = '\\'; break;
                case 'n':  out[j++] = '\n'; break;
                case 'r':  break;
                case 't':  out[j++] = '\t'; break;
                case '/':  out[j++] = '/';  break;
                case 'u': {
                    if (p + 4 < len) {
                        unsigned val = 0;
                        for (int k = 1; k <= 4; k++) {
                            char h = buf[p + k]; val <<= 4;
                            if (h>='0'&&h<='9') val |= h-'0';
                            else if (h>='a'&&h<='f') val |= h-'a'+10;
                            else if (h>='A'&&h<='F') val |= h-'A'+10;
                        }
                        p += 4;
                        if (val < 128) out[j++] = (char)val;
                        else if (val==0x2013||val==0x2014) out[j++] = '-';
                        else if (val==0x2018||val==0x2019) out[j++] = '\'';
                        else if (val==0x201C||val==0x201D) out[j++] = '"';
                        else if (val==0x2026) { out[j++]='.'; out[j++]='.'; out[j++]='.'; }
                        else out[j++] = '?';
                    }
                    break;
                }
                default: out[j++] = buf[p]; break;
                }
            } else {
                out[j++] = buf[p];
            }
            p++;
        }
        out[j] = '\0';
        return j;
    }
    out[0] = '\0';
    return 0;
}

// ============================================================================
// Display line building
// ============================================================================

static void add_line(const char* text, int len, Color color, int size, TrueTypeFont* font) {
    if (g_line_count >= MAX_LINES) return;
    WikiLine* l = &g_lines[g_line_count++];
    int copy = len < 255 ? len : 255;
    memcpy(l->text, text, copy);
    l->text[copy] = '\0';
    l->color     = color;
    l->font_size = size;
    l->font      = font;
}

static void add_empty_line() {
    if (g_line_count >= MAX_LINES) return;
    WikiLine* l = &g_lines[g_line_count++];
    l->text[0]   = '\0';
    l->color     = TEXT_COLOR;
    l->font_size = FONT_SIZE;
    l->font      = g_font;
}

// Word-wrap a text segment into display lines using pixel-width measurement.
static void wrap_text(TrueTypeFont* font, int size, const char* text, int textLen,
                      int max_px, Color color) {
    char  cur[256];
    int   cur_len = 0;
    const char* p   = text;
    const char* end = text + textLen;

    while (p < end) {
        while (p < end && *p == ' ') p++;
        if (p >= end) break;

        const char* word_start = p;
        while (p < end && *p != ' ') p++;
        int word_len = (int)(p - word_start);
        if (word_len <= 0) continue;

        // Build candidate: current line + space + word
        char test[260];
        int test_len = cur_len;
        memcpy(test, cur, cur_len);
        if (cur_len > 0) test[test_len++] = ' ';
        int copy = word_len < (int)(sizeof(test) - test_len - 1)
                 ? word_len : (int)(sizeof(test) - test_len - 1);
        memcpy(test + test_len, word_start, copy);
        test_len += copy;
        test[test_len] = '\0';

        int test_w = font->measure_text(test, size);
        if (test_w <= max_px || cur_len == 0) {
            // Fits — append to current line
            memcpy(cur, test, test_len + 1);
            cur_len = test_len;
        } else {
            // Emit current line and start fresh
            if (cur_len > 0) add_line(cur, cur_len, color, size, font);
            int wl = word_len < 254 ? word_len : 254;
            memcpy(cur, word_start, wl);
            cur_len = wl;
            cur[cur_len] = '\0';
        }
    }
    if (cur_len > 0) add_line(cur, cur_len, color, size, font);
}

static void build_display_lines(const char* title, const char* extract, int extractLen) {
    g_line_count = 0;
    g_scroll_y   = 0;

    // Max pixel width for text (accounting for left pad, right pad, scrollbar)
    int max_px = g_win_w - TEXT_PAD - SCROLLBAR_W - TEXT_PAD;

    // Title — large, serif, black
    if (title && title[0]) {
        TrueTypeFont* tf = g_font_serif ? g_font_serif : g_font;
        wrap_text(tf, TITLE_SIZE, title, (int)strlen(title), max_px, BLACK);
        add_empty_line();
    }

    // Process extract line-by-line
    const char* p   = extract;
    const char* end = extract + extractLen;

    while (p < end && g_line_count < MAX_LINES) {
        const char* line_start = p;
        while (p < end && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (p < end) p++;  // consume '\n'

        if (line_len == 0) {
            add_empty_line();
            continue;
        }

        // Section header: == Title ==
        if (line_len >= 4 && line_start[0] == '=' && line_start[1] == '=') {
            int si = 0, ei = line_len;
            while (si < line_len && line_start[si] == '=') si++;
            while (si < line_len && line_start[si] == ' ') si++;
            while (ei > si && line_start[ei-1] == '=') ei--;
            while (ei > si && line_start[ei-1] == ' ') ei--;
            if (ei > si) {
                add_empty_line();
                TrueTypeFont* tf = g_font_serif ? g_font_serif : g_font;
                wrap_text(tf, SECTION_SIZE, line_start + si, ei - si, max_px, BLACK);
            }
            continue;
        }

        // Regular body text
        wrap_text(g_font, FONT_SIZE, line_start, line_len, max_px, TEXT_COLOR);
    }
}

// ============================================================================
// Network search (blocking)
// ============================================================================

static void do_search(const char* query) {
    // Lazy TLS/DNS init
    if (!g_tls_ready) {
        g_server_ip = montauk::resolve(WIKI_HOST);
        if (g_server_ip == 0) {
            snprintf(g_status, sizeof(g_status),
                     "Error: could not resolve en.wikipedia.org");
            g_phase = AppPhase::ERR; return;
        }
        g_tas = tls::load_trust_anchors();
        if (g_tas.count == 0) {
            snprintf(g_status, sizeof(g_status), "Error: no CA certificates loaded");
            g_phase = AppPhase::ERR; return;
        }
        g_tls_ready = true;
    }

    static char encoded[1024];
    url_encode_title(query, encoded, sizeof(encoded));

    static char path[2048];
    snprintf(path, sizeof(path),
        "/w/api.php?action=query&format=json&formatversion=2"
        "&prop=extracts&explaintext=1&titles=%s", encoded);

    int respLen = wiki_fetch(path, g_resp_buf, RESP_MAX);
    if (respLen <= 0) {
        snprintf(g_status, sizeof(g_status), "Error: no response from Wikipedia");
        g_phase = AppPhase::ERR; return;
    }
    g_resp_buf[respLen] = '\0';

    int headerEnd = find_header_end(g_resp_buf, respLen);
    if (headerEnd < 0) {
        snprintf(g_status, sizeof(g_status), "Error: malformed HTTP response");
        g_phase = AppPhase::ERR; return;
    }

    int status      = parse_status_code(g_resp_buf, headerEnd);
    const char* body = g_resp_buf + headerEnd;
    int bodyLen     = respLen - headerEnd;

    if (status == 404) {
        snprintf(g_status, sizeof(g_status), "Article not found: %s", query);
        g_phase = AppPhase::ERR; return;
    }

    extract_json_string(body, bodyLen, "title", g_title, sizeof(g_title));
    g_extract_len = extract_json_string(body, bodyLen, "extract",
                                        g_extract_buf, RESP_MAX - 1);
    if (g_extract_len == 0) {
        snprintf(g_status, sizeof(g_status), "No content found for: %s", query);
        g_phase = AppPhase::ERR; return;
    }

    build_display_lines(g_title, g_extract_buf, g_extract_len);
    g_phase = AppPhase::DONE;
}

// ============================================================================
// Rendering
// ============================================================================

static void render(uint32_t* pixels) {
    static constexpr Color TOOLBAR_BG = Color::from_rgb(0xF5, 0xF5, 0xF5);
    static constexpr Color HINT_COLOR = Color::from_rgb(0x99, 0x99, 0x99);

    // Background
    px_fill(pixels, g_win_w, 0, 0, g_win_w, g_win_h, WINDOW_BG);

    // ---- Toolbar ----
    px_fill(pixels, g_win_w, 0, 0, g_win_w, TOOLBAR_H, TOOLBAR_BG);
    px_hline(pixels, g_win_w, 0, TOOLBAR_H, g_win_w, BORDER);

    // Search box geometry
    int sb_y = 8, sb_h = TOOLBAR_H - 16;
    int btn_w = 80, btn_gap = 8;
    int sb_x = 8;
    int sb_w = g_win_w - sb_x - btn_gap - btn_w - 8;
    if (sb_w < 80) sb_w = 80;

    px_fill(pixels, g_win_w, sb_x, sb_y, sb_w, sb_h, WHITE);
    px_rect_outline(pixels, g_win_w, sb_x, sb_y, sb_w, sb_h, BORDER);

    // Search box text + cursor
    if (g_font) {
        int ty = sb_y + (sb_h - FONT_SIZE) / 2;
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                               sb_x + 6, ty, g_query, TEXT_COLOR, FONT_SIZE);
        int qw = g_font->measure_text(g_query, FONT_SIZE);
        int cx = sb_x + 6 + qw + 1;
        if (cx < sb_x + sb_w - 4)
            px_vline(pixels, g_win_w, cx, ty + 1, FONT_SIZE - 2, TEXT_COLOR);
    }

    // Search button
    int btn_x = sb_x + sb_w + btn_gap;
    px_fill(pixels, g_win_w, btn_x, sb_y, btn_w, sb_h, ACCENT);
    if (g_font) {
        int stw = g_font->measure_text("Search", FONT_SIZE);
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                               btn_x + (btn_w - stw) / 2,
                               sb_y + (sb_h - FONT_SIZE) / 2,
                               "Search", WHITE, FONT_SIZE);
    }

    // ---- Content area ----
    int cy = TOOLBAR_H + 1;
    int ch = g_win_h - cy;
    if (!g_font) return;

    if (g_phase == AppPhase::IDLE) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            TEXT_PAD, cy + 16,
            "Type a topic and press Enter or click Search.",
            HINT_COLOR, FONT_SIZE);
    } else if (g_phase == AppPhase::LOADING) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            TEXT_PAD, cy + 16,
            "Searching Wikipedia...", HINT_COLOR, FONT_SIZE);
    } else if (g_phase == AppPhase::ERR) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            TEXT_PAD, cy + 16, g_status, CLOSE_BTN, FONT_SIZE);
    } else if (g_phase == AppPhase::DONE && g_line_count > 0) {
        int visible = ch / g_line_h;  // approximate using body line height
        int y       = cy + 8;

        for (int i = g_scroll_y; i < g_line_count && y < g_win_h; i++) {
            WikiLine& l  = g_lines[i];
            int lh = g_font->get_line_height(l.font_size) + 4;
            if (y + lh > g_win_h) break;
            if (l.text[0] != '\0') {
                l.font->draw_to_buffer(pixels, g_win_w, g_win_h,
                                       TEXT_PAD, y, l.text, l.color, l.font_size);
            }
            y += lh;
        }

        // Scrollbar
        if (g_line_count > visible) {
            int sbx = g_win_w - SCROLLBAR_W;
            px_fill(pixels, g_win_w, sbx, cy, SCROLLBAR_W, ch, SCROLLBAR_BG);
            int max_sc  = g_line_count - visible;
            int thumb_h = (visible * ch) / g_line_count;
            if (thumb_h < 20) thumb_h = 20;
            int thumb_y = cy + (g_scroll_y * (ch - thumb_h)) / (max_sc > 0 ? max_sc : 1);
            px_fill(pixels, g_win_w, sbx + 2, thumb_y,
                    SCROLLBAR_W - 4, thumb_h, SCROLLBAR_FG);
        }
    }
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Allocate large buffers from heap
    g_lines       = (WikiLine*)montauk::malloc(MAX_LINES * sizeof(WikiLine));
    g_resp_buf    = (char*)malloc(RESP_MAX + 1);
    g_extract_buf = (char*)malloc(RESP_MAX + 1);
    if (!g_lines || !g_resp_buf || !g_extract_buf) montauk::exit(1);

    // Load fonts
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_font       = load_font("0:/fonts/Roboto-Medium.ttf");
    g_font_bold  = load_font("0:/fonts/Roboto-Bold.ttf");
    g_font_serif = load_font("0:/fonts/NotoSerif-SemiBold.ttf");
    if (!g_font) montauk::exit(1);

    g_line_h = g_font->get_line_height(FONT_SIZE) + 4;

    apply_scale(montauk::win_getscale());

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Wikipedia", INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int      win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    render(pixels);
    montauk::win_present(win_id);

    bool search_pending = false;

    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;  // window closed / error

        if (r == 0) {
            // No event — idle at ~60 fps
            montauk::sleep_ms(16);
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        // ---- Handle event ----
        if (ev.type == 3) break;  // close

        if (ev.type == 4) {
            apply_scale(ev.scale.scale);
            if (g_phase == AppPhase::DONE && g_line_count > 0) {
                build_display_lines(g_title, g_extract_buf, g_extract_len);
            }
        }

        if (ev.type == 2) {
            int new_w = ev.resize.w;
            int new_h = ev.resize.h;
            if (new_w > 0 && new_h > 0 && (new_w != g_win_w || new_h != g_win_h)) {
                uint64_t new_va = montauk::win_resize(win_id, new_w, new_h);
                if (new_va != 0) {
                    pixels = (uint32_t*)(uintptr_t)new_va;
                    g_win_w = new_w;
                    g_win_h = new_h;
                    if (g_phase == AppPhase::DONE && g_line_count > 0) {
                        build_display_lines(g_title, g_extract_buf, g_extract_len);
                    }
                }
            }

        } else if (ev.type == 0 && ev.key.pressed) {
            uint8_t ascii = ev.key.ascii;
            uint8_t scan  = ev.key.scancode;

            if (ascii == '\n' || ascii == '\r') {
                search_pending = true;
            } else if (ascii == '\b' || scan == 0x0E) {
                int len = (int)strlen(g_query);
                if (len > 0) g_query[len - 1] = '\0';
            } else if (ascii >= 32 && ascii < 127) {
                int len = (int)strlen(g_query);
                if (len < 254) { g_query[len] = ascii; g_query[len + 1] = '\0'; }
            } else if (g_phase == AppPhase::DONE) {
                // Navigation keys
                int visible = (g_win_h - TOOLBAR_H - 1) / g_line_h;
                int max_sc  = g_line_count - visible;
                if (max_sc < 0) max_sc = 0;
                if      (scan == 0x48) { if (g_scroll_y > 0) g_scroll_y--; }
                else if (scan == 0x50) { if (g_scroll_y < max_sc) g_scroll_y++; }
                else if (scan == 0x49) { g_scroll_y -= visible; if (g_scroll_y < 0) g_scroll_y = 0; }
                else if (scan == 0x51) { g_scroll_y += visible; if (g_scroll_y > max_sc) g_scroll_y = max_sc; }
                else if (scan == 0x47) { g_scroll_y = 0; }
                else if (scan == 0x4F) { g_scroll_y = max_sc; }
            }

        } else if (ev.type == 1) {
            // Mouse
            int mx = ev.mouse.x, my = ev.mouse.y;
            bool just_clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);

            // Search button hit-test
            int sb_h  = TOOLBAR_H - 16;
            int btn_w = 80;
            int sb_w  = g_win_w - 8 - 8 - btn_w - 8;
            if (sb_w < 80) sb_w = 80;
            int btn_x = 8 + sb_w + 8;
            if (just_clicked && mx >= btn_x && mx < btn_x + btn_w &&
                my >= 8 && my < 8 + sb_h) {
                search_pending = true;
            }

            // Scroll wheel
            if (ev.mouse.scroll != 0 && g_phase == AppPhase::DONE) {
                int visible = (g_win_h - TOOLBAR_H - 1) / g_line_h;
                int max_sc  = g_line_count - visible;
                if (max_sc < 0) max_sc = 0;
                g_scroll_y += ev.mouse.scroll * 3;
                if (g_scroll_y < 0)       g_scroll_y = 0;
                if (g_scroll_y > max_sc)  g_scroll_y = max_sc;
            }
        }

        // Trigger search if requested and query is non-empty
        if (search_pending && g_query[0] != '\0') {
            search_pending = false;
            g_phase = AppPhase::LOADING;
            render(pixels);
            montauk::win_present(win_id);
            do_search(g_query);  // blocking
        }

        render(pixels);
        montauk::win_present(win_id);
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
