/*
 * main.cpp
 * MontaukOS Weather app - standalone Window Server process
 * Fetches current weather from wttr.in via HTTPS (BearSSL)
 * Displays temperature, description, feels like, and location
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/svg.hpp>
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

static constexpr int INIT_W     = 380;
static constexpr int INIT_H     = 280;
static constexpr int HEADER_H   = 160;
static constexpr int FOOTER_H   = 50;
static constexpr int ICON_SIZE  = 80;
static constexpr int ICON_X     = 28;
static constexpr int ICON_Y     = 40;
static constexpr int INFO_X     = ICON_X + ICON_SIZE + 20;  // 128
static constexpr int TEMP_Y     = 40;
static int           TEMP_SIZE  = 40;
static constexpr int DESC_Y     = 92;
static int           DESC_SIZE  = 17;
static constexpr int FEELS_Y    = 116;
static int           LABEL_SIZE = 15;
static constexpr int RESP_MAX   = 65536;

static const char WTTR_HOST[] = "wttr.in";

// ============================================================================
// App state
// ============================================================================

enum class AppPhase { IDLE, LOADING, DONE, ERR };

static AppPhase g_phase    = AppPhase::IDLE;
static char     g_temp[32]       = {};
static char     g_desc[128]      = {};
static char     g_feels[48]      = {};
static char     g_location[128]  = {};
static char     g_status[256]    = {};
static int      g_win_w          = INIT_W;
static int      g_win_h          = INIT_H;

static char*    g_resp_buf       = nullptr;

// Fonts
static TrueTypeFont* g_font      = nullptr;  // Roboto Medium
static TrueTypeFont* g_font_bold = nullptr;  // Roboto Bold

// Weather icon (loaded per-fetch based on weather code)
static SvgIcon  g_icon           = {nullptr, 0, 0};
static char     g_icon_name[64]  = {};

// TLS state (lazy-init on first fetch)
static bool     g_tls_ready = false;
static uint32_t g_server_ip = 0;
static tls::TrustAnchors g_tas = {nullptr, 0, 0};

// ============================================================================
// Pixel buffer helpers
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

static void px_hline(uint32_t* px, int bw, int bh, int x, int y, int len, Color c) {
    if (y < 0 || y >= bh) return;
    uint32_t v = c.to_pixel();
    int x1 = x + len > bw ? bw : x + len;
    for (int col = x < 0 ? 0 : x; col < x1; col++)
        px[y * bw + col] = v;
}

// Integer square root (Newton's method).
static int isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

// Fill a rounded rectangle directly onto the pixel buffer.
static void px_fill_rounded(uint32_t* px, int bw, int bh,
                              int x, int y, int w, int h, int r, Color c) {
    if (r <= 0) { px_fill(px, bw, bh, x, y, w, h, c); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    uint32_t v = c.to_pixel();
    // Corner arc centers
    int cx_l = x + r;
    int cx_r = x + w - 1 - r;

    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= bh) continue;

        int left, right;
        if (row < y + r) {
            int dy = y + r - row;             // 1..r
            int dx = isqrt(r * r - dy * dy);
            left  = cx_l - dx;
            right = cx_r + dx + 1;
        } else if (row >= y + h - r) {
            int dy = row - (y + h - 1 - r);  // 1..r
            int dx = isqrt(r * r - dy * dy);
            left  = cx_l - dx;
            right = cx_r + dx + 1;
        } else {
            left  = x;
            right = x + w;
        }

        if (left  < 0)   left  = 0;
        if (right > bw)  right = bw;
        for (int col = left; col < right; col++)
            px[row * bw + col] = v;
    }
}

// Alpha-composite an SvgIcon onto the pixel buffer.
static void draw_icon(uint32_t* px, int bw, int bh,
                      int dx, int dy, const SvgIcon& icon) {
    if (!icon.pixels) return;
    for (int row = 0; row < icon.height; row++) {
        int ty = dy + row;
        if (ty < 0 || ty >= bh) continue;
        for (int col = 0; col < icon.width; col++) {
            int tx = dx + col;
            if (tx < 0 || tx >= bw) continue;
            uint32_t s = icon.pixels[row * icon.width + col];
            uint8_t sa = (s >> 24) & 0xFF;
            if (sa == 0) continue;
            if (sa == 255) { px[ty * bw + tx] = s; continue; }
            uint8_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            uint32_t d  = px[ty * bw + tx];
            uint8_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t inv = 255 - sa;
            px[ty * bw + tx] = 0xFF000000u
                | (((sr * sa + dr * inv) / 255u) << 16)
                | (((sg * sa + dg * inv) / 255u) << 8)
                |  ((sb * sa + db * inv) / 255u);
        }
    }
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
// JSON parsing
// ============================================================================

// Extract value of a simple JSON string field: "key":"value"
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
        while (p < len && j < maxOut - 1 && buf[p] != '"') {
            if (buf[p] == '\\' && p + 1 < len) { p++; out[j++] = buf[p]; }
            else out[j++] = buf[p];
            p++;
        }
        out[j] = '\0';
        return j;
    }
    out[0] = '\0';
    return 0;
}

// Find the first occurrence of needle in buf[0..len-1]. Returns offset or -1.
static int find_substr(const char* buf, int len, const char* needle) {
    int nlen = (int)strlen(needle);
    if (nlen > len) return -1;
    for (int i = 0; i <= len - nlen; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) return i;
    }
    return -1;
}

// Extract the "value" string within a named JSON array-of-objects field.
// e.g., "weatherDesc":[{"value":"Partly cloudy"}]  →  "Partly cloudy"
// Works by finding section_key first, then the first "value" after it.
static int extract_array_value(const char* buf, int len, const char* section_key,
                                char* out, int maxOut) {
    int pos = find_substr(buf, len, section_key);
    if (pos < 0) { out[0] = '\0'; return 0; }
    return extract_json_string(buf + pos, len - pos, "value", out, maxOut);
}

// Parse a signed integer from a decimal string.
static int parse_int(const char* s) {
    int sign = 1, val = 0, i = 0;
    if (s[0] == '-') { sign = -1; i = 1; }
    while (s[i] >= '0' && s[i] <= '9') val = val * 10 + (s[i++] - '0');
    return sign * val;
}

// ============================================================================
// Weather code → icon filename
// Maps wttr.in WMO-style weather codes to Flat-Remix panel icon names.
// ============================================================================

static const char* weather_code_to_icon(int code) {
    switch (code) {
    case 113:                                                   return "weather-clear.svg";
    case 116:                                                   return "weather-few-clouds.svg";
    case 119:                                                   return "weather-clouds.svg";
    case 122:                                                   return "weather-overcast.svg";
    case 143:                                                   return "weather-mist.svg";
    case 248: case 260:                                         return "weather-fog.svg";
    case 176: case 263: case 266: case 353:                     return "weather-showers-scattered.svg";
    case 293: case 296: case 299: case 302:
    case 305: case 308: case 356: case 359:                     return "weather-showers.svg";
    case 179: case 362: case 365: case 368:                     return "weather-snow-scattered.svg";
    case 323: case 326: case 329: case 332:
    case 335: case 338: case 371: case 374:                     return "weather-snow.svg";
    case 227: case 230:                                         return "weather-snow.svg";
    case 182: case 311: case 314: case 317: case 320:           return "weather-snow-rain.svg";
    case 185: case 281: case 284:                               return "weather-freezing-rain.svg";
    case 350: case 377:                                         return "weather-hail.svg";
    case 200: case 386: case 389: case 392: case 395:           return "weather-storm.svg";
    default:                                                    return "weather-none-available.svg";
    }
}

// Load the weather icon for the given icon filename (caches the last result).
static void load_weather_icon(const char* icon_name) {
    if (strcmp(g_icon_name, icon_name) == 0 && g_icon.pixels) return;

    if (g_icon.pixels) svg_free(g_icon);

    static char path[80];
    snprintf(path, sizeof(path), "0:/icons/%s", icon_name);
    // ColorScheme-Text paths (main shape) use this fill_color; accent classes
    // (ColorScheme-Highlight=blue, ColorScheme-NeutralText=yellow, etc.) use their
    // CSS-defined colors as parsed from the SVG's <style> block by svg_load.
    static constexpr Color ICON_FG = Color::from_rgb(0x5C, 0x61, 0x6C); // dark neutral
    g_icon = svg_load(path, ICON_SIZE, ICON_SIZE, ICON_FG);

    int i = 0;
    while (icon_name[i] && i < 63) { g_icon_name[i] = icon_name[i]; i++; }
    g_icon_name[i] = '\0';
}

// ============================================================================
// UI scale
// ============================================================================

static void apply_scale(int scale) {
    switch (scale) {
    case 0: TEMP_SIZE=32; DESC_SIZE=14; LABEL_SIZE=12; break;
    case 2: TEMP_SIZE=50; DESC_SIZE=21; LABEL_SIZE=19; break;
    default: TEMP_SIZE=40; DESC_SIZE=17; LABEL_SIZE=15; break;
    }
}

// ============================================================================
// Network fetch  (blocking — called from the event loop)
// ============================================================================

static void do_fetch() {
    // Lazy init: resolve DNS and load CA certificates once
    if (!g_tls_ready) {
        g_server_ip = montauk::resolve(WTTR_HOST);
        if (g_server_ip == 0) {
            snprintf(g_status, sizeof(g_status),
                     "Error: could not resolve %s", WTTR_HOST);
            g_phase = AppPhase::ERR; return;
        }
        g_tas = tls::load_trust_anchors();
        if (g_tas.count == 0) {
            snprintf(g_status, sizeof(g_status), "Error: no CA certificates loaded");
            g_phase = AppPhase::ERR; return;
        }
        g_tls_ready = true;
    }

    static char request[512];
    int reqLen = snprintf(request, sizeof(request),
        "GET /?format=j1 HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: MontaukOS/1.0 weather\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        WTTR_HOST);

    int respLen = tls::https_fetch(WTTR_HOST, g_server_ip, 443,
                                   request, reqLen, g_tas,
                                   g_resp_buf, RESP_MAX);
    if (respLen <= 0) {
        snprintf(g_status, sizeof(g_status), "Error: no response from server");
        g_phase = AppPhase::ERR; return;
    }
    g_resp_buf[respLen] = '\0';

    int headerEnd = find_header_end(g_resp_buf, respLen);
    if (headerEnd < 0) {
        snprintf(g_status, sizeof(g_status), "Error: malformed HTTP response");
        g_phase = AppPhase::ERR; return;
    }

    int status = parse_status_code(g_resp_buf, headerEnd);
    if (status != 200) {
        snprintf(g_status, sizeof(g_status), "Error: HTTP %d from server", status);
        g_phase = AppPhase::ERR; return;
    }

    const char* body   = g_resp_buf + headerEnd;
    int          bodyLen = respLen - headerEnd;

    // Extract core weather fields
    static char temp_raw[16], feels_raw[16], code_raw[8];
    extract_json_string(body, bodyLen, "temp_C",      temp_raw,  sizeof(temp_raw));
    extract_json_string(body, bodyLen, "FeelsLikeC",  feels_raw, sizeof(feels_raw));
    extract_json_string(body, bodyLen, "weatherCode", code_raw,  sizeof(code_raw));

    extract_array_value(body, bodyLen, "\"weatherDesc\"", g_desc, sizeof(g_desc));

    static char area[64], country[64];
    extract_array_value(body, bodyLen, "\"areaName\"", area,    sizeof(area));
    extract_array_value(body, bodyLen, "\"country\"",  country, sizeof(country));

    // Degree sign is U+00B0 = 0xB0 in Latin-1 (single byte, within the 256-entry glyph cache)
    snprintf(g_temp,  sizeof(g_temp),  "%s\xb0""C", temp_raw);
    snprintf(g_feels, sizeof(g_feels), "Feels like: %s\xb0""C", feels_raw);

    if (area[0] && country[0])
        snprintf(g_location, sizeof(g_location), "%s, %s", area, country);
    else if (area[0])
        snprintf(g_location, sizeof(g_location), "%s", area);
    else
        snprintf(g_location, sizeof(g_location), "Unknown location");

    // Load matching weather icon
    int code = parse_int(code_raw);
    load_weather_icon(weather_code_to_icon(code));

    g_phase = AppPhase::DONE;
}

// ============================================================================
// Theme colors
// ============================================================================

static constexpr Color CONTENT_BG = Color::from_rgb(0xFF, 0xFF, 0xFF); // white
static constexpr Color FOOTER_BG  = Color::from_rgb(0xF5, 0xF5, 0xF5); // light gray
static constexpr Color DIVIDER    = Color::from_rgb(0xCC, 0xCC, 0xCC); // subtle border
static constexpr Color DARK_TEXT  = Color::from_rgb(0x33, 0x33, 0x33);
static constexpr Color MID_TEXT   = Color::from_rgb(0x88, 0x88, 0x88);
static constexpr Color HINT_TEXT  = Color::from_rgb(0x99, 0x99, 0x99);
static constexpr Color ERR_TEXT   = Color::from_rgb(0xCC, 0x22, 0x22);
static constexpr Color BTN_BG     = Color::from_rgb(0x36, 0x7B, 0xF0); // accent blue
static constexpr Color WHITE_TEXT = Color::from_rgb(0xFF, 0xFF, 0xFF);

// ============================================================================
// Rendering
// ============================================================================

static constexpr int BTN_W      = 110;
static constexpr int BTN_H      = 28;
static constexpr int BTN_RADIUS = 6;

static void render(uint32_t* pixels) {
    // ── Background ───────────────────────────────────────────────────────────
    px_fill(pixels, g_win_w, g_win_h, 0, 0,
            g_win_w, g_win_h - FOOTER_H, CONTENT_BG);
    px_fill(pixels, g_win_w, g_win_h, 0, g_win_h - FOOTER_H,
            g_win_w, FOOTER_H, FOOTER_BG);

    // Divider between content and location strip
    px_hline(pixels, g_win_w, g_win_h, 0, HEADER_H,           g_win_w, DIVIDER);
    // Divider above footer
    px_hline(pixels, g_win_w, g_win_h, 0, g_win_h - FOOTER_H, g_win_w, DIVIDER);

    if (!g_font) return;

    // ── Main content area (y=0..HEADER_H) ────────────────────────────────────
    if (g_phase == AppPhase::LOADING) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            20, HEADER_H / 2 - 9,
            "Fetching weather data...", HINT_TEXT, 18);

    } else if (g_phase == AppPhase::ERR) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            20, 20, g_status, ERR_TEXT, 15);

    } else if (g_phase == AppPhase::IDLE) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            20, HEADER_H / 2 - 9,
            "Click Refresh to check weather.", HINT_TEXT, 18);

    } else {  // DONE
        // Weather icon
        draw_icon(pixels, g_win_w, g_win_h, ICON_X, ICON_Y, g_icon);

        // Temperature (large bold)
        TrueTypeFont* temp_font = g_font_bold ? g_font_bold : g_font;
        temp_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            INFO_X, TEMP_Y, g_temp, DARK_TEXT, TEMP_SIZE);

        // Weather description
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            INFO_X, DESC_Y, g_desc, DARK_TEXT, DESC_SIZE);

        // Feels like
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            INFO_X, FEELS_Y, g_feels, MID_TEXT, LABEL_SIZE);
    }

    // ── Location strip (y=HEADER_H..g_win_h-FOOTER_H) ────────────────────────
    if (g_phase == AppPhase::DONE) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            20, HEADER_H + 14, g_location, DARK_TEXT, LABEL_SIZE);
    }

    // ── Refresh button (rounded, in footer) ───────────────────────────────────
    int btn_x = (g_win_w - BTN_W) / 2;
    int btn_y = g_win_h - FOOTER_H + (FOOTER_H - BTN_H) / 2;

    px_fill_rounded(pixels, g_win_w, g_win_h,
                    btn_x, btn_y, BTN_W, BTN_H, BTN_RADIUS, BTN_BG);

    if (g_phase == AppPhase::LOADING) {
        const char* lbl = "Loading...";
        int sw = g_font->measure_text(lbl, 14);
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            btn_x + (BTN_W - sw) / 2, btn_y + (BTN_H - 14) / 2,
            lbl, WHITE_TEXT, 14);
    } else {
        const char* lbl = "Refresh";
        int sw = g_font->measure_text(lbl, 15);
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            btn_x + (BTN_W - sw) / 2, btn_y + (BTN_H - 15) / 2,
            lbl, WHITE_TEXT, 15);
    }
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Allocate response buffer from heap
    g_resp_buf = (char*)malloc(RESP_MAX + 1);
    if (!g_resp_buf) montauk::exit(1);

    // Load fonts
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_font      = load_font("0:/fonts/Roboto-Medium.ttf");
    g_font_bold = load_font("0:/fonts/Roboto-Bold.ttf");
    if (!g_font) montauk::exit(1);

    apply_scale(montauk::win_getscale());

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("Weather", INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int       win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    // Initial fetch on startup
    g_phase = AppPhase::LOADING;
    render(pixels);
    montauk::win_present(win_id);
    do_fetch();

    // Event loop
    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;

        if (r == 0) {
            montauk::sleep_ms(16);
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        if (ev.type == 3) break;  // close

        if (ev.type == 4) {
            apply_scale(ev.scale.scale);
        }

        if (ev.type == 1) {
            // Mouse — check for Refresh button click
            bool just_clicked = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
            if (just_clicked && g_phase != AppPhase::LOADING) {
                int btn_x = (g_win_w - BTN_W) / 2;
                int btn_y = g_win_h - FOOTER_H + (FOOTER_H - BTN_H) / 2;
                int mx = ev.mouse.x, my = ev.mouse.y;
                if (mx >= btn_x && mx < btn_x + BTN_W &&
                    my >= btn_y && my < btn_y + BTN_H) {
                    g_phase = AppPhase::LOADING;
                    render(pixels);
                    montauk::win_present(win_id);
                    do_fetch();
                }
            }
        }

        render(pixels);
        montauk::win_present(win_id);
    }

    if (g_icon.pixels) svg_free(g_icon);
    montauk::win_destroy(win_id);
    montauk::exit(0);
}
