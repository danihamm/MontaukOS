/*
 * main.cpp
 * MontaukOS Font Preview app
 * Displays TTF font samples at multiple sizes with vertical scrolling
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>

using namespace gui;

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W      = 800;
static constexpr int INIT_H      = 600;
static constexpr int SCROLL_STEP = 40;
static constexpr int PADDING     = 16;
static constexpr int SECTION_GAP = 12;
static constexpr int UI_FONT_SZ  = 13;

static constexpr Color BG_COLOR      = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TEXT_COLOR    = Color::from_rgb(0x33, 0x33, 0x33);
static constexpr Color LABEL_COLOR   = Color::from_rgb(0x88, 0x88, 0x88);
static constexpr Color SEPARATOR     = Color::from_rgb(0xE0, 0xE0, 0xE0);
static constexpr Color ERR_COLOR     = Color::from_rgb(0xCC, 0x33, 0x33);
static constexpr Color SCROLLBAR_BG  = Color::from_rgb(0xE0, 0xE0, 0xE0);
static constexpr Color SCROLLBAR_FG  = Color::from_rgb(0xAA, 0xAA, 0xAA);

static const int PREVIEW_SIZES[] = {12, 16, 20, 24, 32, 48, 72};
static constexpr int NUM_SIZES = 7;

static const char* PANGRAM   = "The quick brown fox jumps over the lazy dog";
static const char* UPPER     = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char* LOWER_NUM = "abcdefghijklmnopqrstuvwxyz 0123456789";

// ============================================================================
// Pre-rendered glyph cache
// ============================================================================

struct PreviewGlyph {
    uint8_t* bitmap;
    int width, height;
    int xoff, yoff;
    int advance;
};

struct SizeCache {
    PreviewGlyph glyphs[128];  // printable ASCII is enough
    int pixel_size;
    int ascent;
    int line_height;
};

static SizeCache g_caches[NUM_SIZES];

// ============================================================================
// App state
// ============================================================================

static int       g_win_w    = INIT_W;
static int       g_win_h    = INIT_H;
static int       g_scroll_y = 0;
static int       g_content_h = 0;

static bool      g_load_ok       = false;

static TrueTypeFont* g_ui_font = nullptr;

static stbtt_fontinfo g_preview_info;
static uint8_t*       g_preview_data = nullptr;

// ============================================================================
// Helpers
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

static const char* basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

static void int_to_str(char* buf, int val) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    bool neg = val < 0;
    if (neg) val = -val;
    char tmp[16];
    int i = 0;
    while (val > 0 && i < 15) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void str_append(char* dst, const char* src, int maxlen) {
    int len = montauk::slen(dst);
    int i = 0;
    while (src[i] && len + i < maxlen - 1) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = '\0';
}

// ============================================================================
// Pre-render all glyphs at all sizes (called once at startup)
// ============================================================================

static void prerrender_glyphs() {
    for (int s = 0; s < NUM_SIZES; s++) {
        SizeCache* sc = &g_caches[s];
        sc->pixel_size = PREVIEW_SIZES[s];
        float scale = stbtt_ScaleForPixelHeight(&g_preview_info, (float)sc->pixel_size);

        int asc, desc, lg;
        stbtt_GetFontVMetrics(&g_preview_info, &asc, &desc, &lg);
        sc->ascent = (int)(asc * scale);
        sc->line_height = (int)((asc - desc + lg) * scale);

        for (int cp = 0; cp < 128; cp++) {
            PreviewGlyph* g = &sc->glyphs[cp];
            g->bitmap = nullptr;
            g->width = g->height = 0;
            g->xoff = g->yoff = 0;
            g->advance = 0;

            if (cp < 0x20) continue;  // skip control chars

            int advance, lsb;
            stbtt_GetCodepointHMetrics(&g_preview_info, cp, &advance, &lsb);
            g->advance = (int)(advance * scale);

            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&g_preview_info, cp, scale, scale,
                                         &x0, &y0, &x1, &y1);
            g->width = x1 - x0;
            g->height = y1 - y0;
            g->xoff = x0;
            g->yoff = y0;

            if (g->width > 0 && g->height > 0) {
                g->bitmap = (uint8_t*)montauk::malloc(g->width * g->height);
                if (g->bitmap) {
                    stbtt_MakeCodepointBitmap(&g_preview_info, g->bitmap,
                        g->width, g->height, g->width, scale, scale, cp);
                }
            }
        }
    }
}

// ============================================================================
// Draw text from pre-rendered cache (fast blit, no rasterisation)
// ============================================================================

static void draw_cached_text(uint32_t* pixels, int buf_w, int buf_h,
                             int x, int y, const char* text,
                             Color color, int size_idx) {
    SizeCache* sc = &g_caches[size_idx];
    int baseline = y + sc->ascent;
    int cx = x;

    for (int i = 0; text[i]; i++) {
        int cp = (unsigned char)text[i];
        if (cp >= 128) continue;
        PreviewGlyph* g = &sc->glyphs[cp];

        if (g->bitmap) {
            int gx = cx + g->xoff;
            int gy = baseline + g->yoff;
            for (int row = 0; row < g->height; row++) {
                int dy = gy + row;
                if (dy < 0 || dy >= buf_h) continue;
                for (int col = 0; col < g->width; col++) {
                    int dx = gx + col;
                    if (dx < 0 || dx >= buf_w) continue;
                    uint8_t alpha = g->bitmap[row * g->width + col];
                    if (alpha == 0) continue;

                    if (alpha == 255) {
                        pixels[dy * buf_w + dx] =
                            0xFF000000 | ((uint32_t)color.r << 16) |
                            ((uint32_t)color.g << 8) | color.b;
                    } else {
                        uint32_t dst = pixels[dy * buf_w + dx];
                        uint8_t dr = (dst >> 16) & 0xFF;
                        uint8_t dg = (dst >> 8) & 0xFF;
                        uint8_t db = dst & 0xFF;
                        uint32_t a = alpha, inv_a = 255 - alpha;
                        uint32_t rr = (a * color.r + inv_a * dr + 128) / 255;
                        uint32_t gg = (a * color.g + inv_a * dg + 128) / 255;
                        uint32_t bb = (a * color.b + inv_a * db + 128) / 255;
                        pixels[dy * buf_w + dx] =
                            0xFF000000 | (rr << 16) | (gg << 8) | bb;
                    }
                }
            }
        }
        cx += g->advance;
    }
}

// ============================================================================
// Content height calculation
// ============================================================================

static int calc_content_height() {
    if (!g_load_ok) return 200;

    int y = PADDING;

    for (int s = 0; s < NUM_SIZES; s++) {
        if (g_ui_font) y += g_ui_font->get_line_height(UI_FONT_SZ) + 4;

        int lh = g_caches[s].line_height;
        y += lh * 3;

        y += SECTION_GAP + 1 + SECTION_GAP;
    }

    y += PADDING;
    return y;
}

// ============================================================================
// Rendering
// ============================================================================

static void render(uint32_t* pixels) {
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, g_win_h, BG_COLOR);

    if (!g_load_ok) {
        if (g_ui_font) {
            g_ui_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                PADDING, g_win_h / 2 - 8, "Error: could not load font", ERR_COLOR, 15);
        }
        return;
    }

    int y = PADDING - g_scroll_y;

    for (int s = 0; s < NUM_SIZES; s++) {
        int lh = g_caches[s].line_height;
        int section_h = (g_ui_font ? g_ui_font->get_line_height(UI_FONT_SZ) + 4 : 0)
                        + lh * 3 + SECTION_GAP + 1 + SECTION_GAP;

        // Skip sections entirely above the viewport
        if (y + section_h < 0) {
            y += section_h;
            continue;
        }
        // Stop once we're entirely below the viewport
        if (y >= g_win_h) break;

        // Size label (e.g. "48px")
        if (g_ui_font) {
            char label[16] = {};
            int_to_str(label, PREVIEW_SIZES[s]);
            str_append(label, "px", 16);
            g_ui_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                PADDING, y, label, LABEL_COLOR, UI_FONT_SZ);
            y += g_ui_font->get_line_height(UI_FONT_SZ) + 4;
        }

        draw_cached_text(pixels, g_win_w, g_win_h,
            PADDING, y, PANGRAM, TEXT_COLOR, s);
        y += lh;

        draw_cached_text(pixels, g_win_w, g_win_h,
            PADDING, y, UPPER, TEXT_COLOR, s);
        y += lh;

        draw_cached_text(pixels, g_win_w, g_win_h,
            PADDING, y, LOWER_NUM, TEXT_COLOR, s);
        y += lh;

        y += SECTION_GAP;
        if (y >= 0 && y < g_win_h) {
            px_fill(pixels, g_win_w, g_win_h,
                PADDING, y, g_win_w - 2 * PADDING, 1, SEPARATOR);
        }
        y += 1 + SECTION_GAP;
    }

    // Scrollbar
    int view_h = g_win_h;
    if (g_content_h > view_h) {
        int sb_x = g_win_w - 6;
        int max_scroll = g_content_h - view_h;
        int thumb_h = (view_h * view_h) / g_content_h;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = (g_scroll_y * (view_h - thumb_h)) / max_scroll;
        px_fill(pixels, g_win_w, g_win_h, sb_x, 0, 4, view_h, SCROLLBAR_BG);
        px_fill(pixels, g_win_w, g_win_h, sb_x, thumb_y, 4, thumb_h, SCROLLBAR_FG);
    }
}

// ============================================================================
// Scroll clamping
// ============================================================================

static void clamp_scroll() {
    int max_scroll = g_content_h - g_win_h;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_y < 0) g_scroll_y = 0;
    if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    char filepath[512] = {};
    int arglen = montauk::getargs(filepath, sizeof(filepath));
    if (arglen <= 0) montauk::strcpy(filepath, "");

    // Load UI font for labels
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_ui_font = load_font("0:/fonts/Roboto-Medium.ttf");

    // Window title from filename
    char title[64] = "Font Preview";
    if (filepath[0]) {
        const char* name = basename(filepath);
        if (name[0]) montauk::strncpy(title, name, 63);
    }

    // Load preview font via stbtt
    if (filepath[0]) {
        int fd = montauk::open(filepath);
        if (fd >= 0) {
            uint64_t size = montauk::getsize(fd);
            if (size > 0 && size <= 1024 * 1024) {
                g_preview_data = (uint8_t*)montauk::malloc(size);
                if (g_preview_data) {
                    montauk::read(fd, g_preview_data, 0, size);
                    if (stbtt_InitFont(&g_preview_info, g_preview_data,
                                       stbtt_GetFontOffsetForIndex(g_preview_data, 0))) {
                        g_load_ok = true;
                    }
                }
            }
            montauk::close(fd);
        }
    }

    // Pre-render all glyphs at all sizes (once, before event loop)
    if (g_load_ok) prerrender_glyphs();

    g_content_h = calc_content_height();

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create(title, INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int       win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    render(pixels);
    montauk::win_present(win_id);

    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;

        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) break;

        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            clamp_scroll();
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        if (ev.type == 0 && ev.key.pressed) {
            bool redraw = false;

            if (ev.key.ascii == 'q' || ev.key.ascii == 'Q' || ev.key.scancode == 0x01)
                break;

            if (ev.key.scancode == 0x48) { g_scroll_y -= SCROLL_STEP; redraw = true; }
            if (ev.key.scancode == 0x50) { g_scroll_y += SCROLL_STEP; redraw = true; }
            if (ev.key.scancode == 0x47) { g_scroll_y = 0; redraw = true; }
            if (ev.key.scancode == 0x4F) { g_scroll_y = g_content_h; redraw = true; }
            if (ev.key.scancode == 0x49) { g_scroll_y -= g_win_h; redraw = true; }
            if (ev.key.scancode == 0x51) { g_scroll_y += g_win_h; redraw = true; }

            if (redraw) {
                clamp_scroll();
                render(pixels);
                montauk::win_present(win_id);
            }
            continue;
        }

        if (ev.type == 1) {
            if (ev.mouse.scroll != 0) {
                g_scroll_y -= ev.mouse.scroll * SCROLL_STEP;
                clamp_scroll();
                render(pixels);
                montauk::win_present(win_id);
            }
            continue;
        }
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
