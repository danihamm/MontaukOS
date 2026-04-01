/*
 * test_page_jpeg.cpp
 * JPEG test-page generator for printers that do not accept PDF/text jobs.
 */

#include "test_page_jpeg.hpp"

#include <gui/truetype.hpp>
#include <montauk/string.h>
#include <print/print.hpp>

extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_WRITE_NO_STDIO
#include <gui/stb_image_write.h>
}

namespace gui {
extern const uint8_t font_data[256 * 16];
}

namespace {

static constexpr int PAGE_W = 1240;
static constexpr int PAGE_H = 1754;
static constexpr const char* ROBOTO_MEDIUM_PATH = "0:/fonts/Roboto-Medium.ttf";
static constexpr const char* ROBOTO_BOLD_PATH   = "0:/fonts/Roboto-Bold.ttf";

struct JpegBuffer {
    uint8_t* data;
    int size;
    int capacity;
    bool failed;
};

struct PageFonts {
    gui::TrueTypeFont* medium;
    gui::TrueTypeFont* bold;
};

static void jpeg_write_callback(void* context, void* data, int size) {
    JpegBuffer* buf = (JpegBuffer*)context;
    if (!buf || !data || size <= 0 || buf->failed) return;

    int needed = buf->size + size;
    if (needed > buf->capacity) {
        int new_cap = buf->capacity > 0 ? buf->capacity * 2 : 4096;
        if (new_cap < needed) new_cap = needed;
        uint8_t* new_data = (uint8_t*)realloc(buf->data, (size_t)new_cap);
        if (!new_data) {
            buf->failed = true;
            return;
        }
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, data, (size_t)size);
    buf->size += size;
}

static void fill_rect(uint8_t* rgb, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    if (!rgb || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PAGE_W) w = PAGE_W - x;
    if (y + h > PAGE_H) h = PAGE_H - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = y; yy < y + h; yy++) {
        uint8_t* row = rgb + (yy * PAGE_W + x) * 3;
        for (int xx = 0; xx < w; xx++) {
            row[xx * 3 + 0] = r;
            row[xx * 3 + 1] = g;
            row[xx * 3 + 2] = b;
        }
    }
}

static void draw_rect_outline(uint8_t* rgb, int x, int y, int w, int h,
                              int thickness, uint8_t r, uint8_t g, uint8_t b) {
    fill_rect(rgb, x, y, w, thickness, r, g, b);
    fill_rect(rgb, x, y + h - thickness, w, thickness, r, g, b);
    fill_rect(rgb, x, y, thickness, h, r, g, b);
    fill_rect(rgb, x + w - thickness, y, thickness, h, r, g, b);
}

static void blend_pixel(uint8_t* rgb, int x, int y,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
    if (!rgb || alpha == 0 || x < 0 || x >= PAGE_W || y < 0 || y >= PAGE_H) return;
    uint8_t* p = rgb + (y * PAGE_W + x) * 3;
    if (alpha == 255) {
        p[0] = r;
        p[1] = g;
        p[2] = b;
        return;
    }

    uint32_t a = alpha;
    uint32_t inv = 255 - a;
    p[0] = (uint8_t)((a * r + inv * p[0] + 127) / 255);
    p[1] = (uint8_t)((a * g + inv * p[1] + 127) / 255);
    p[2] = (uint8_t)((a * b + inv * p[2] + 127) / 255);
}

static void draw_char_scaled(uint8_t* rgb, int x, int y, int scale, char ch,
                             uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t* glyph = &gui::font_data[(unsigned char)ch * 16];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            fill_rect(rgb, x + col * scale, y + row * scale, scale, scale, r, g, b);
        }
    }
}

static void draw_text_scaled(uint8_t* rgb, int x, int y, int scale, const char* text,
                             uint8_t r, uint8_t g, uint8_t b) {
    if (!rgb || !text) return;
    for (int i = 0; text[i] != '\0'; i++)
        draw_char_scaled(rgb, x + i * 8 * scale, y, scale, text[i], r, g, b);
}

static bool init_page_font(gui::TrueTypeFont* font, const char* path) {
    if (!font || !path) return false;
    memset(font, 0, sizeof(*font));
    return font->init(path);
}

static gui::TrueTypeFont* load_page_font(const char* path) {
    gui::TrueTypeFont* font = (gui::TrueTypeFont*)montauk::malloc(sizeof(gui::TrueTypeFont));
    if (!font) return nullptr;
    if (!init_page_font(font, path)) {
        montauk::mfree(font);
        return nullptr;
    }
    return font;
}

static void free_page_font(gui::TrueTypeFont* font) {
    if (!font) return;
    for (int i = 0; i < font->cache_count; i++) {
        for (int g = 0; g < 256; g++) {
            if (font->caches[i].glyphs[g].bitmap) {
                montauk::mfree(font->caches[i].glyphs[g].bitmap);
                font->caches[i].glyphs[g].bitmap = nullptr;
            }
        }
    }
    if (font->data) {
        montauk::free(font->data);
        font->data = nullptr;
    }
    montauk::mfree(font);
}

static void destroy_page_fonts(PageFonts* fonts) {
    if (!fonts) return;
    free_page_font(fonts->medium);
    free_page_font(fonts->bold);
    fonts->medium = nullptr;
    fonts->bold = nullptr;
}

static PageFonts load_page_fonts() {
    PageFonts fonts = {};
    fonts.medium = load_page_font(ROBOTO_MEDIUM_PATH);
    fonts.bold = load_page_font(ROBOTO_BOLD_PATH);
    return fonts;
}

static gui::TrueTypeFont* pick_font(PageFonts* fonts, bool bold) {
    if (!fonts) return nullptr;
    if (bold && fonts->bold && fonts->bold->valid) return fonts->bold;
    if (fonts->medium && fonts->medium->valid) return fonts->medium;
    if (fonts->bold && fonts->bold->valid) return fonts->bold;
    return nullptr;
}

static int measure_text(uint8_t* rgb, PageFonts* fonts, bool bold,
                        int pixel_size, int fallback_scale, const char* text) {
    (void)rgb;
    if (!text) return 0;
    gui::TrueTypeFont* font = pick_font(fonts, bold);
    if (font && font->valid) return font->measure_text(text, pixel_size);
    return (int)strlen(text) * 8 * fallback_scale;
}

static void draw_text(uint8_t* rgb, PageFonts* fonts, bool bold,
                      int x, int y, int pixel_size, int fallback_scale,
                      const char* text, uint8_t r, uint8_t g, uint8_t b) {
    if (!rgb || !text) return;

    gui::TrueTypeFont* font = pick_font(fonts, bold);
    if (!font || !font->valid) {
        draw_text_scaled(rgb, x, y, fallback_scale, text, r, g, b);
        return;
    }

    gui::GlyphCache* gc = font->get_cache(pixel_size);
    if (!gc) {
        draw_text_scaled(rgb, x, y, fallback_scale, text, r, g, b);
        return;
    }

    int cx = x;
    int baseline = y + gc->ascent;
    for (int i = 0; text[i] != '\0'; i++) {
        gui::CachedGlyph* glyph = font->get_glyph(gc, (unsigned char)text[i]);
        if (!glyph) continue;

        if (glyph->bitmap) {
            int gx = cx + glyph->xoff;
            int gy = baseline + glyph->yoff;
            for (int row = 0; row < glyph->height; row++) {
                for (int col = 0; col < glyph->width; col++) {
                    uint8_t alpha = glyph->bitmap[row * glyph->width + col];
                    if (alpha == 0) continue;
                    blend_pixel(rgb, gx + col, gy + row, r, g, b, alpha);
                }
            }
        }
        cx += glyph->advance;
    }
}

static void draw_centered_text(uint8_t* rgb, PageFonts* fonts, bool bold,
                               int y, int pixel_size, int fallback_scale,
                               const char* text, uint8_t r, uint8_t g, uint8_t b) {
    if (!text) return;
    int width = measure_text(rgb, fonts, bold, pixel_size, fallback_scale, text);
    int x = (PAGE_W - width) / 2;
    draw_text(rgb, fonts, bold, x, y, pixel_size, fallback_scale, text, r, g, b);
}

} // namespace

bool generate_test_page_jpeg(uint8_t** out_data, int* out_len) {
    if (out_data) *out_data = nullptr;
    if (out_len) *out_len = 0;

    uint8_t* rgb = (uint8_t*)malloc((size_t)PAGE_W * PAGE_H * 3);
    if (!rgb) return false;
    memset(rgb, 0xFF, (size_t)PAGE_W * PAGE_H * 3);

    PageFonts fonts = load_page_fonts();

    draw_centered_text(rgb, &fonts, true, 50, 52, 3,
                       "Montauk Operating System", 0x22, 0x22, 0x22);
    draw_centered_text(rgb, &fonts, false, 112, 26, 2,
                       "Printer Test Page for IPP Printers", 0x66, 0x66, 0x66);
    fill_rect(rgb, 120, 170, PAGE_W - 240, 4, 0x29, 0x8F, 0x45);

    fill_rect(rgb, 80, 220, PAGE_W - 160, 240, 0xFA, 0xFA, 0xFA);
    draw_rect_outline(rgb, 80, 220, PAGE_W - 160, 240, 3, 0xD9, 0xD9, 0xD9);

    char timestamp[32];
    print::now_string(timestamp, sizeof(timestamp));

    char line[128];
    snprintf(line, sizeof(line), "Generated: %s", timestamp);
    draw_text(rgb, &fonts, false, 120, 268, 30, 2, line, 0x22, 0x22, 0x22);
    draw_text(rgb, &fonts, false, 120, 320, 30, 2, "Format: image/jpeg", 0x22, 0x22, 0x22);
    draw_text(rgb, &fonts, false, 120, 372, 30, 2, "If you can read this, printing works.", 0x22, 0x22, 0x22);

    draw_text(rgb, &fonts, true, 100, 530, 32, 2, "Color Check", 0x22, 0x22, 0x22);
    draw_rect_outline(rgb, 100, 590, PAGE_W - 200, 150, 3, 0xD0, 0xD0, 0xD0);
    fill_rect(rgb, 130, 625, 150, 72, 0x30, 0x30, 0x30);
    fill_rect(rgb, 320, 625, 150, 72, 0x25, 0xA8, 0xD8);
    fill_rect(rgb, 510, 625, 150, 72, 0xD8, 0x3C, 0x82);
    fill_rect(rgb, 700, 625, 150, 72, 0xE9, 0xCF, 0x4A);
    fill_rect(rgb, 890, 625, 150, 72, 0x47, 0x98, 0x5E);
    draw_text(rgb, &fonts, false, 150, 706, 18, 1, "Black", 0x33, 0x33, 0x33);
    draw_text(rgb, &fonts, false, 352, 706, 18, 1, "Cyan", 0x33, 0x33, 0x33);
    draw_text(rgb, &fonts, false, 518, 706, 18, 1, "Magenta", 0x33, 0x33, 0x33);
    draw_text(rgb, &fonts, false, 715, 706, 18, 1, "Yellow", 0x33, 0x33, 0x33);
    draw_text(rgb, &fonts, false, 932, 706, 18, 1, "Green", 0x33, 0x33, 0x33);

    draw_text(rgb, &fonts, true, 100, 792, 32, 2, "Grayscale", 0x22, 0x22, 0x22);
    for (int i = 0; i < 10; i++) {
        uint8_t v = (uint8_t)(i * 255 / 9);
        fill_rect(rgb, 100 + i * ((PAGE_W - 200) / 10), 855,
                  (PAGE_W - 200) / 10, 72, v, v, v);
    }
    draw_rect_outline(rgb, 100, 855, PAGE_W - 200, 72, 3, 0xC8, 0xC8, 0xC8);

    draw_text(rgb, &fonts, true, 100, 998, 32, 2, "Alignment", 0x22, 0x22, 0x22);
    for (int i = 0; i < 5; i++) {
        int yy = 1070 + i * 60;
        fill_rect(rgb, 100, yy, PAGE_W - 200, 3, 0x11, 0x11, 0x11);
        fill_rect(rgb, 100 + i * 40, yy - 15, 3, 33, 0x11, 0x11, 0x11);
    }

    JpegBuffer jpeg = {};
    int ok = stbi_write_jpg_to_func(jpeg_write_callback, &jpeg, PAGE_W, PAGE_H, 3, rgb, 82);
    destroy_page_fonts(&fonts);
    free(rgb);
    if (!ok || jpeg.failed || !jpeg.data || jpeg.size <= 0) {
        free(jpeg.data);
        return false;
    }

    if (out_data) *out_data = jpeg.data;
    else free(jpeg.data);
    if (out_len) *out_len = jpeg.size;
    return true;
}
