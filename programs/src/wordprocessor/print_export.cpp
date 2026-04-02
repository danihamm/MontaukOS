/*
 * print_export.cpp
 * Word Processor print export and job submission
 * Copyright (c) 2026 Daniel Hammer
 */

#include "wordprocessor.hpp"
#include <gui/dialogs.hpp>
#include <print/print.hpp>

extern "C" {
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define STBI_WRITE_NO_STDIO
#include <gui/stb_image_write.h>
}

namespace dialogs = gui::dialogs;

static constexpr int WP_PRINT_PAGE_W   = 1275; // 8.5in at 150 DPI
static constexpr int WP_PRINT_PAGE_H   = 1650; // 11in at 150 DPI
static constexpr int WP_PRINT_MARGIN_X = 96;
static constexpr int WP_PRINT_MARGIN_Y = 96;
static constexpr int WP_PRINT_JPEG_QUALITY = 92;

struct WpJpegBuffer {
    uint8_t* data;
    int size;
    int capacity;
    bool failed;
};

static void wp_status_copy(char* out, int out_len, const char* text) {
    if (!out || out_len <= 0) return;
    if (!text) text = "";
    montauk::strncpy(out, text, out_len - 1);
    out[out_len - 1] = '\0';
}

static void wp_jpeg_write_callback(void* context, void* data, int size) {
    WpJpegBuffer* buf = (WpJpegBuffer*)context;
    if (!buf || !data || size <= 0 || buf->failed) return;

    int needed = buf->size + size;
    if (needed > buf->capacity) {
        int new_cap = buf->capacity > 0 ? buf->capacity : 256 * 1024;
        while (new_cap < needed)
            new_cap *= 2;
        uint8_t* grown = (uint8_t*)montauk::realloc(buf->data, new_cap);
        if (!grown) {
            buf->failed = true;
            return;
        }
        buf->data = grown;
        buf->capacity = new_cap;
    }

    montauk::memcpy(buf->data + buf->size, data, size);
    buf->size += size;
}

static int wp_print_layout_width() {
    return (WP_PRINT_PAGE_W - WP_PRINT_MARGIN_X * 2) + WP_MARGIN * 2 + WP_SCROLLBAR_W;
}

static void wp_draw_print_divider(Canvas& c, int x, int y, int w, int h,
                                  uint8_t divider_type, Color color) {
    if (w <= 0 || h <= 0 || divider_type == PARA_DIVIDER_NONE) return;

    int cy = y + h / 2;
    switch (divider_type) {
    case PARA_DIVIDER_SINGLE:
        c.fill_rect(x, cy, w, 1, color);
        break;
    case PARA_DIVIDER_DOUBLE:
        c.fill_rect(x, cy - 2, w, 1, color);
        c.fill_rect(x, cy + 2, w, 1, color);
        break;
    case PARA_DIVIDER_DOTTED:
        for (int dx = x; dx < x + w; dx += 5)
            c.fill_rect(dx, cy, 2, 1, color);
        break;
    case PARA_DIVIDER_DASHED:
        for (int dx = x; dx < x + w; dx += 11) {
            int dash_w = (x + w - dx) < 7 ? (x + w - dx) : 7;
            if (dash_w > 0) c.fill_rect(dx, cy, dash_w, 1, color);
        }
        break;
    case PARA_DIVIDER_HEAVY:
        c.fill_rect(x, cy - 1, w, 3, color);
        break;
    case PARA_DIVIDER_THIN_THICK:
        c.fill_rect(x, cy - 3, w, 1, color);
        c.fill_rect(x, cy, w, 3, color);
        break;
    case PARA_DIVIDER_THICK_THIN:
        c.fill_rect(x, cy - 3, w, 3, color);
        c.fill_rect(x, cy + 2, w, 1, color);
        break;
    }
}

static void wp_make_base_print_name(WordProcessorState* wp, char* out, int out_len) {
    const char* src = wp->filename[0] ? wp->filename : "document";
    int last_dot = -1;
    for (int i = 0; src[i]; i++) {
        if (src[i] == '.') last_dot = i;
    }

    if (last_dot <= 0) {
        wp_status_copy(out, out_len, src);
        return;
    }

    int copy_len = last_dot;
    if (copy_len >= out_len) copy_len = out_len - 1;
    montauk::memcpy(out, src, (uint64_t)copy_len);
    out[copy_len] = '\0';
}

static bool wp_printer_accepts_jpeg(const char* printer_uri, char* err, int err_len) {
    print::ProbeState probe = {};
    char probe_err[160] = {};
    if (!print::probe_printer_uri(printer_uri, &probe, probe_err, sizeof(probe_err)))
        return true;
    if (probe.caps.supports_jpeg)
        return true;

    if (probe.caps.supported_formats[0]) {
        snprintf(err, (size_t)err_len,
                 "Selected printer does not advertise JPEG support (supports: %s)",
                 probe.caps.supported_formats);
    } else {
        wp_status_copy(err, err_len, "Selected printer does not advertise JPEG support");
    }
    return false;
}

static bool wp_build_print_page_map(WordProcessorState* wp,
                                    int** out_line_pages,
                                    int** out_line_y,
                                    int* out_page_count,
                                    char* err, int err_len) {
    if (out_line_pages) *out_line_pages = nullptr;
    if (out_line_y) *out_line_y = nullptr;
    if (out_page_count) *out_page_count = 0;

    wp_recompute_wrap(wp, wp_print_layout_width(), WP_PRINT_DPI);
    if (wp->wrap_line_count <= 0) {
        wp_status_copy(err, err_len, "failed to paginate document");
        return false;
    }

    int* line_pages = (int*)montauk::malloc((uint64_t)wp->wrap_line_count * sizeof(int));
    int* line_y = (int*)montauk::malloc((uint64_t)wp->wrap_line_count * sizeof(int));
    if (!line_pages || !line_y) {
        if (line_pages) montauk::mfree(line_pages);
        if (line_y) montauk::mfree(line_y);
        wp_status_copy(err, err_len, "out of memory while preparing print pages");
        return false;
    }

    int base_y = wp->wrap_lines[0].y;
    int printable_bottom = WP_PRINT_PAGE_H - WP_PRINT_MARGIN_Y;
    int current_page = 0;
    int current_y = WP_PRINT_MARGIN_Y;
    int prev_rel_bottom = 0;

    for (int i = 0; i < wp->wrap_line_count; i++) {
        WrapLine* wl = &wp->wrap_lines[i];
        int rel_y = wl->y - base_y;
        if (rel_y < 0) rel_y = 0;

        int gap_before = (i == 0) ? 0 : (rel_y - prev_rel_bottom);
        if (gap_before < 0) gap_before = 0;

        int line_top = current_y + gap_before;
        if (line_top + wl->height > printable_bottom && current_y > WP_PRINT_MARGIN_Y) {
            current_page++;
            current_y = WP_PRINT_MARGIN_Y;
            line_top = current_y;
        }

        line_pages[i] = current_page;
        line_y[i] = line_top;
        current_y = line_top + wl->height;
        prev_rel_bottom = rel_y + wl->height;
    }

    if (out_line_pages) *out_line_pages = line_pages;
    else montauk::mfree(line_pages);
    if (out_line_y) *out_line_y = line_y;
    else montauk::mfree(line_y);
    if (out_page_count) *out_page_count = current_page + 1;
    return true;
}

static void wp_draw_print_list_marker(Canvas& c, WordProcessorState* wp, WrapLine* wl, int py) {
    if (!wl->first_in_paragraph || wl->paragraph_idx < 0 || wl->paragraph_idx >= wp->paragraph_count)
        return;

    ParagraphStyle* para = &wp->paragraphs[wl->paragraph_idx];
    if (para->divider_type != PARA_DIVIDER_NONE || para->list_type == PARA_LIST_NONE) return;

    int marker_x = WP_PRINT_MARGIN_X + wp_scale_layout_units(para->left_indent + para->first_line_indent, WP_PRINT_DPI);
    int min_x = WP_PRINT_MARGIN_X - WP_MARGIN + 4;
    if (marker_x < min_x) marker_x = min_x;

    if (para->list_type == PARA_LIST_BULLET) {
        int bullet_x = marker_x + wp_scale_layout_units(7, WP_PRINT_DPI);
        int bullet_r = wp_scale_layout_units(3, WP_PRINT_DPI);
        if (bullet_r < 2) bullet_r = 2;
        fill_circle(c, bullet_x, py + wl->height / 2, bullet_r, colors::BLACK);
        return;
    }

    if (wp->run_count <= 0) return;
    StyledRun* run = &wp->runs[wl->run_idx < wp->run_count ? wl->run_idx : 0];
    TrueTypeFont* font = wp_get_font(run->font_id, run->flags);
    char label[16];
    snprintf(label, sizeof(label), "%d.", wl->list_number > 0 ? wl->list_number : 1);
    int font_px = wp_print_font_pixels(run->size);
    int top_y = py + (wl->height - font_px) / 2;
    draw_text(c, font ? font : g_ui_font, marker_x, top_y, label, colors::BLACK, font_px);
}

static bool wp_encode_print_page(WordProcessorState* wp,
                                 const int* line_pages,
                                 const int* line_y,
                                 int page_index,
                                 uint8_t** out_data,
                                 int* out_len,
                                 char* err, int err_len) {
    if (out_data) *out_data = nullptr;
    if (out_len) *out_len = 0;

    uint64_t pixel_bytes = (uint64_t)WP_PRINT_PAGE_W * WP_PRINT_PAGE_H * sizeof(uint32_t);
    uint32_t* pixels = (uint32_t*)montauk::malloc(pixel_bytes);
    if (!pixels) {
        wp_status_copy(err, err_len, "out of memory while rendering print page");
        return false;
    }

    Canvas c(pixels, WP_PRINT_PAGE_W, WP_PRINT_PAGE_H);
    c.fill(colors::WHITE);

    for (int li = 0; li < wp->wrap_line_count; li++) {
        if (line_pages[li] != page_index) continue;

        WrapLine* wl = &wp->wrap_lines[li];
        int py = line_y[li];
        wp_draw_print_list_marker(c, wp, wl, py);
        if (wl->divider_type != PARA_DIVIDER_NONE) {
            int divider_x = WP_PRINT_MARGIN_X + (wl->x - WP_MARGIN);
            wp_draw_print_divider(c, divider_x, py, wl->width, wl->height, wl->divider_type, colors::BLACK);
        }

        int chars_left = wl->char_count;
        int ri = wl->run_idx;
        int ro = wl->run_offset;
        int x = WP_PRINT_MARGIN_X + (wl->x - WP_MARGIN);

        int line_abs_start = wp_wrap_line_start(wp, li);
        int char_idx = 0;

        while (chars_left > 0 && ri < wp->run_count) {
            StyledRun* r = &wp->runs[ri];
            TrueTypeFont* font = wp_get_font(r->font_id, r->flags);
            if (!font || !font->valid) {
                ri++;
                ro = 0;
                continue;
            }

            GlyphCache* gc = font->get_cache(wp_print_font_pixels(r->size));
            int baseline = py + wl->baseline;

            int avail = r->len - ro;
            int to_draw = avail < chars_left ? avail : chars_left;

            for (int ci = 0; ci < to_draw; ci++) {
                char ch = r->text[ro + ci];
                (void)line_abs_start;
                (void)char_idx;

                if (ch != '\n' && (ch >= 32 || ch < 0)) {
                    int adv = font->draw_char_to_buffer(
                        c.pixels, c.w, c.h, x, baseline, (unsigned char)ch, colors::BLACK, gc);
                    x += adv;
                }
                char_idx++;
            }

            chars_left -= to_draw;
            ro += to_draw;
            if (ro >= r->len) {
                ri++;
                ro = 0;
            }
        }
    }

    uint64_t rgb_bytes = (uint64_t)WP_PRINT_PAGE_W * WP_PRINT_PAGE_H * 3;
    uint8_t* rgb = (uint8_t*)montauk::malloc(rgb_bytes);
    if (!rgb) {
        montauk::mfree(pixels);
        wp_status_copy(err, err_len, "out of memory while encoding print page");
        return false;
    }

    for (int y = 0; y < WP_PRINT_PAGE_H; y++) {
        uint8_t* dst = rgb + (uint64_t)y * WP_PRINT_PAGE_W * 3;
        for (int x = 0; x < WP_PRINT_PAGE_W; x++) {
            uint32_t px = pixels[(uint64_t)y * WP_PRINT_PAGE_W + x];
            dst[x * 3 + 0] = (uint8_t)((px >> 16) & 0xFF);
            dst[x * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);
            dst[x * 3 + 2] = (uint8_t)(px & 0xFF);
        }
    }
    montauk::mfree(pixels);

    WpJpegBuffer jpeg = {};
    jpeg.capacity = 256 * 1024;
    jpeg.data = (uint8_t*)montauk::malloc(jpeg.capacity);
    if (!jpeg.data) {
        montauk::mfree(rgb);
        wp_status_copy(err, err_len, "out of memory for JPEG output");
        return false;
    }

    int ok = stbi_write_jpg_to_func(wp_jpeg_write_callback, &jpeg,
                                    WP_PRINT_PAGE_W, WP_PRINT_PAGE_H, 3, rgb,
                                    WP_PRINT_JPEG_QUALITY);
    montauk::mfree(rgb);

    if (!ok || jpeg.failed || jpeg.size <= 0) {
        montauk::mfree(jpeg.data);
        wp_status_copy(err, err_len, "JPEG encoding failed");
        return false;
    }

    if (out_data) *out_data = jpeg.data;
    else montauk::mfree(jpeg.data);
    if (out_len) *out_len = jpeg.size;
    return true;
}

bool wp_print_document(WordProcessorState* wp, char* out_status, int out_status_len) {
    if (out_status && out_status_len > 0) out_status[0] = '\0';
    if (!wp) {
        wp_status_copy(out_status, out_status_len, "No document loaded");
        return false;
    }

    char printer_uri[256] = {};
    char printer_name[128] = {};
    char msg[160] = {};
    montauk::strncpy(printer_uri, wp->preferred_printer_uri, (int)sizeof(printer_uri) - 1);
    printer_uri[sizeof(printer_uri) - 1] = '\0';
    montauk::strncpy(printer_name, wp->preferred_printer_name, (int)sizeof(printer_name) - 1);
    printer_name[sizeof(printer_name) - 1] = '\0';
    uint32_t copies = wp->preferred_print_copies > 0 ? wp->preferred_print_copies : 1;

    const char* job_name = wp->filename[0] ? wp->filename : "Untitled document";
    if (!dialogs::configure_print("Print Document",
                                  printer_uri, job_name,
                                  printer_uri, sizeof(printer_uri),
                                  printer_name, sizeof(printer_name),
                                  &copies, msg, sizeof(msg))) {
        if (msg[0]) wp_status_copy(out_status, out_status_len, msg);
        return false;
    }

    montauk::strncpy(wp->preferred_printer_uri, printer_uri, (int)sizeof(wp->preferred_printer_uri) - 1);
    wp->preferred_printer_uri[sizeof(wp->preferred_printer_uri) - 1] = '\0';
    montauk::strncpy(wp->preferred_printer_name, printer_name, (int)sizeof(wp->preferred_printer_name) - 1);
    wp->preferred_printer_name[sizeof(wp->preferred_printer_name) - 1] = '\0';
    wp->preferred_print_copies = copies > 0 ? copies : 1;

    char err[192] = {};
    if (!wp_printer_accepts_jpeg(printer_uri, err, sizeof(err))) {
        wp_status_copy(out_status, out_status_len, err);
        return false;
    }

    int* line_pages = nullptr;
    int* line_y = nullptr;
    int page_count = 0;
    if (!wp_build_print_page_map(wp, &line_pages, &line_y, &page_count, err, sizeof(err))) {
        wp_status_copy(out_status, out_status_len, err);
        return false;
    }

    char base_name[96] = {};
    wp_make_base_print_name(wp, base_name, sizeof(base_name));
    if (!base_name[0]) wp_status_copy(base_name, sizeof(base_name), "document");

    int submitted_pages = 0;
    char first_job_id[64] = {};

    for (int page = 0; page < page_count; page++) {
        uint8_t* jpeg = nullptr;
        int jpeg_len = 0;
        if (!wp_encode_print_page(wp, line_pages, line_y, page, &jpeg, &jpeg_len, err, sizeof(err))) {
            break;
        }

        char page_job_name[128];
        char source_name[128];
        if (page_count > 1) {
            snprintf(page_job_name, sizeof(page_job_name), "%s (Page %d/%d)", job_name, page + 1, page_count);
            snprintf(source_name, sizeof(source_name), "%s-page-%03d.jpg", base_name, page + 1);
        } else {
            wp_status_copy(page_job_name, sizeof(page_job_name), job_name);
            snprintf(source_name, sizeof(source_name), "%s.jpg", base_name);
        }

        char job_id[64] = {};
        bool ok = print::submit_document_buffer_job(
            jpeg, jpeg_len,
            source_name,
            "image/jpeg",
            printer_uri,
            page_job_name,
            (int)(copies > 0 ? copies : 1),
            job_id, sizeof(job_id),
            err, sizeof(err));
        montauk::mfree(jpeg);
        if (!ok) break;

        if (submitted_pages == 0)
            wp_status_copy(first_job_id, sizeof(first_job_id), job_id);
        submitted_pages++;
    }

    montauk::mfree(line_pages);
    montauk::mfree(line_y);

    if (submitted_pages != page_count) {
        if (submitted_pages > 0) {
            char partial[192];
            snprintf(partial, sizeof(partial),
                     "Queued %d of %d pages before failing: %s",
                     submitted_pages, page_count,
                     err[0] ? err : "unknown error");
            wp_status_copy(out_status, out_status_len, partial);
        } else {
            wp_status_copy(out_status, out_status_len, err[0] ? err : "Failed to queue print job");
        }
        return false;
    }

    char summary[192];
    const char* label = printer_name[0] ? printer_name : printer_uri;
    if (page_count == 1) {
        if (copies > 1) {
            snprintf(summary, sizeof(summary),
                     "Queued 1 page to %s (%u copies), job %s",
                     label, (unsigned)copies, first_job_id);
        } else {
            snprintf(summary, sizeof(summary),
                     "Queued print job %s to %s",
                     first_job_id, label);
        }
    } else if (copies > 1) {
        snprintf(summary, sizeof(summary),
                 "Queued %d pages to %s (%u copies each)",
                 page_count, label, (unsigned)copies);
    } else {
        snprintf(summary, sizeof(summary),
                 "Queued %d pages to %s",
                 page_count, label);
    }

    wp_status_copy(out_status, out_status_len, summary);
    return true;
}

struct WpPdfBuffer {
    uint8_t* data;
    int len;
    int cap;
};

struct WpPdfPageStream {
    uint8_t* data;
    int len;
};

struct WpPdfFontDef {
    uint8_t font_id;
    uint8_t style_index;
    char resource_name[8];
    char base_font[48];
    const char* file_path;
    TrueTypeFont* font;
    uint8_t* file_data;
    int file_len;
    int descriptor_flags;
    int italic_angle;
    int bbox_x0;
    int bbox_y0;
    int bbox_x1;
    int bbox_y1;
    int ascent;
    int descent;
    int cap_height;
    int units_per_em;
    int font_obj_num;
    int descriptor_obj_num;
    int file_obj_num;
};

static void wp_pdf_buffer_free(WpPdfBuffer* buf) {
    if (buf && buf->data) {
        montauk::mfree(buf->data);
        buf->data = nullptr;
    }
    if (buf) {
        buf->len = 0;
        buf->cap = 0;
    }
}

static bool wp_pdf_buffer_init(WpPdfBuffer* buf, int initial_cap) {
    if (!buf) return false;
    buf->len = 0;
    buf->cap = initial_cap > 0 ? initial_cap : 1024;
    buf->data = (uint8_t*)montauk::malloc((uint64_t)buf->cap);
    return buf->data != nullptr;
}

static bool wp_pdf_buffer_ensure(WpPdfBuffer* buf, int extra) {
    if (!buf || extra < 0) return false;
    if (buf->len + extra <= buf->cap) return true;

    int new_cap = buf->cap > 0 ? buf->cap : 1024;
    while (new_cap < buf->len + extra)
        new_cap *= 2;

    uint8_t* grown = (uint8_t*)montauk::realloc(buf->data, new_cap);
    if (!grown) return false;
    buf->data = grown;
    buf->cap = new_cap;
    return true;
}

static bool wp_pdf_buffer_append_bytes(WpPdfBuffer* buf, const void* data, int len) {
    if (!buf || !data || len < 0) return false;
    if (len == 0) return true;
    if (!wp_pdf_buffer_ensure(buf, len)) return false;
    montauk::memcpy(buf->data + buf->len, data, (uint64_t)len);
    buf->len += len;
    return true;
}

static bool wp_pdf_buffer_appendf(WpPdfBuffer* buf, const char* fmt, ...) {
    if (!buf || !fmt) return false;

    char stack_buf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap_copy);
        return false;
    }

    bool ok = false;
    if (needed < (int)sizeof(stack_buf)) {
        ok = wp_pdf_buffer_append_bytes(buf, stack_buf, needed);
    } else {
        char* tmp = (char*)montauk::malloc((uint64_t)needed + 1);
        if (tmp) {
            vsnprintf(tmp, (size_t)needed + 1, fmt, ap_copy);
            ok = wp_pdf_buffer_append_bytes(buf, tmp, needed);
            montauk::mfree(tmp);
        }
    }
    va_end(ap_copy);
    return ok;
}

static bool wp_pdf_buffer_append_hex_string(WpPdfBuffer* buf, const char* text, int len) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    if (!buf || !text || len < 0) return false;
    if (!wp_pdf_buffer_ensure(buf, len * 2 + 2)) return false;
    buf->data[buf->len++] = '<';
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        buf->data[buf->len++] = (uint8_t)HEX[(ch >> 4) & 0x0F];
        buf->data[buf->len++] = (uint8_t)HEX[ch & 0x0F];
    }
    buf->data[buf->len++] = '>';
    return true;
}

static constexpr int WP_PDF_POINT_SCALE = 100;

static int wp_pdf_div_round_nearest(int64_t numer, int denom) {
    if (denom <= 0) return 0;
    if (numer >= 0) return (int)((numer + denom / 2) / denom);
    return (int)((numer - denom / 2) / denom);
}

static int wp_pdf_points_hundredths_from_print_px(int px) {
    return wp_pdf_div_round_nearest((int64_t)px * 72 * WP_PDF_POINT_SCALE, WP_PRINT_DPI);
}

static int wp_pdf_page_width_points_hundredths() {
    return wp_pdf_points_hundredths_from_print_px(WP_PRINT_PAGE_W);
}

static int wp_pdf_page_height_points_hundredths() {
    return wp_pdf_points_hundredths_from_print_px(WP_PRINT_PAGE_H);
}

static int wp_pdf_y_points_hundredths_from_print_top(int y_px) {
    return wp_pdf_page_height_points_hundredths() - wp_pdf_points_hundredths_from_print_px(y_px);
}

static bool wp_pdf_buffer_append_number_hundredths(WpPdfBuffer* buf, int value_hundredths) {
    // The freestanding printf implementation used here does not substitute %f reliably.
    if (!buf) return false;

    bool negative = value_hundredths < 0;
    int magnitude = negative ? -value_hundredths : value_hundredths;
    int whole = magnitude / WP_PDF_POINT_SCALE;
    int frac = magnitude % WP_PDF_POINT_SCALE;

    char tmp[32];
    int wrote = 0;
    if (frac == 0) {
        wrote = snprintf(tmp, sizeof(tmp), negative ? "-%d" : "%d", whole);
    } else if ((frac % 10) == 0) {
        wrote = snprintf(tmp, sizeof(tmp), negative ? "-%d.%d" : "%d.%d", whole, frac / 10);
    } else {
        wrote = snprintf(tmp, sizeof(tmp), negative ? "-%d.%02d" : "%d.%02d", whole, frac);
    }
    if (wrote <= 0 || wrote >= (int)sizeof(tmp)) return false;
    return wp_pdf_buffer_append_bytes(buf, tmp, wrote);
}

static int wp_pdf_glyph_advance_hundredths(const WpPdfFontDef* def, int font_points, unsigned char ch) {
    if (!def || !def->font || !def->font->valid || def->units_per_em <= 0 || font_points <= 0)
        return 0;

    int advance = 0;
    int lsb = 0;
    int cp = decode_single_byte_codepoint(ch);
    stbtt_GetCodepointHMetrics(&def->font->info, cp, &advance, &lsb);
    return wp_pdf_div_round_nearest((int64_t)advance * font_points * WP_PDF_POINT_SCALE,
                                    def->units_per_em);
}

static int wp_pdf_style_index(uint8_t flags) {
    int idx = 0;
    if (flags & STYLE_BOLD) idx |= 1;
    if (flags & STYLE_ITALIC) idx |= 2;
    return idx;
}

static const char* wp_pdf_font_path(uint8_t font_id, uint8_t style_index) {
    static const char* PATHS[FONT_COUNT][4] = {
        {
            "0:/fonts/Roboto-Medium.ttf",
            "0:/fonts/Roboto-Bold.ttf",
            "0:/fonts/Roboto-Italic.ttf",
            "0:/fonts/Roboto-BoldItalic.ttf",
        },
        {
            "0:/fonts/NotoSerif-Regular.ttf",
            "0:/fonts/NotoSerif-SemiBold.ttf",
            "0:/fonts/NotoSerif-Italic.ttf",
            "0:/fonts/NotoSerif-BoldItalic.ttf",
        },
        {
            "0:/fonts/C059-Roman.ttf",
            "0:/fonts/C059-Bold.ttf",
            "0:/fonts/C059-Italic.ttf",
            "0:/fonts/C059-Bold.ttf",
        },
    };

    if (font_id >= FONT_COUNT || style_index > 3) return nullptr;
    return PATHS[font_id][style_index];
}

static const char* wp_pdf_base_font_name(uint8_t font_id, uint8_t style_index) {
    static const char* NAMES[FONT_COUNT][4] = {
        {
            "Roboto-Medium",
            "Roboto-Bold",
            "Roboto-Italic",
            "Roboto-BoldItalic",
        },
        {
            "NotoSerif-Regular",
            "NotoSerif-SemiBold",
            "NotoSerif-Italic",
            "NotoSerif-BoldItalic",
        },
        {
            "C059-Roman",
            "C059-Bold",
            "C059-Italic",
            "C059-Bold",
        },
    };

    if (font_id >= FONT_COUNT || style_index > 3) return nullptr;
    return NAMES[font_id][style_index];
}

static int wp_pdf_descriptor_flags(uint8_t font_id, uint8_t style_index) {
    int flags = 32; // Nonsymbolic
    if (font_id == FONT_NOTOSERIF || font_id == FONT_C059)
        flags |= 2; // Serif
    if (style_index == 2 || style_index == 3)
        flags |= 64; // Italic
    return flags;
}

static int wp_pdf_units_per_em(TrueTypeFont* font) {
    if (!font || !font->valid || font->info.head <= 0 || !font->info.data)
        return 1000;
    uint8_t* upem_ptr = font->info.data + font->info.head + 18;
    int upem = ((int)upem_ptr[0] << 8) | (int)upem_ptr[1];
    return upem > 0 ? upem : 1000;
}

static bool wp_pdf_path_has_suffix(const char* path, const char* suffix) {
    if (!path || !suffix) return false;
    int plen = montauk::slen(path);
    int slen = montauk::slen(suffix);
    if (slen > plen) return false;
    for (int i = 0; i < slen; i++) {
        char a = path[plen - slen + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static bool wp_pdf_normalize_path(const char* path, char* out, int out_len,
                                  char* err, int err_len) {
    if (!out || out_len <= 0) return false;
    out[0] = '\0';

    if (!path || !path[0]) {
        wp_status_copy(err, err_len, "Choose a PDF filename");
        return false;
    }

    if (wp_pdf_path_has_suffix(path, ".pdf")) {
        wp_status_copy(out, out_len, path);
        return true;
    }

    int plen = montauk::slen(path);
    if (plen + 4 >= out_len) {
        wp_status_copy(err, err_len, "PDF filename is too long");
        return false;
    }

    wp_status_copy(out, out_len, path);
    montauk::strncpy(out + plen, ".pdf", out_len - plen - 1);
    out[out_len - 1] = '\0';
    return true;
}

static bool wp_pdf_load_font_data(WpPdfFontDef* def, char* err, int err_len) {
    if (!def || !def->file_path) return false;
    int fd = montauk::open(def->file_path);
    if (fd < 0) {
        snprintf(err, (size_t)err_len, "Cannot open font %s", def->file_path);
        return false;
    }

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > 4 * 1024 * 1024) {
        montauk::close(fd);
        snprintf(err, (size_t)err_len, "Font file is invalid: %s", def->file_path);
        return false;
    }

    def->file_data = (uint8_t*)montauk::malloc(size);
    if (!def->file_data) {
        montauk::close(fd);
        wp_status_copy(err, err_len, "Out of memory while reading a font");
        return false;
    }

    int got = montauk::read(fd, def->file_data, 0, size);
    montauk::close(fd);
    if (got < 0 || (uint64_t)got != size) {
        montauk::mfree(def->file_data);
        def->file_data = nullptr;
        snprintf(err, (size_t)err_len, "Failed to read font %s", def->file_path);
        return false;
    }

    def->file_len = got;
    return true;
}

static bool wp_pdf_prepare_font_def(WpPdfFontDef* def,
                                    uint8_t font_id,
                                    uint8_t style_index,
                                    int resource_index,
                                    char* err, int err_len) {
    if (!def) return false;
    montauk::memset(def, 0, sizeof(*def));

    def->font_id = font_id;
    def->style_index = style_index;
    snprintf(def->resource_name, sizeof(def->resource_name), "F%d", resource_index);

    const char* base_name = wp_pdf_base_font_name(font_id, style_index);
    const char* file_path = wp_pdf_font_path(font_id, style_index);
    if (!base_name || !file_path) {
        wp_status_copy(err, err_len, "Unsupported font selection in document");
        return false;
    }

    def->file_path = file_path;
    wp_status_copy(def->base_font, sizeof(def->base_font), base_name);

    uint8_t style_flags = 0;
    if (style_index & 1) style_flags |= STYLE_BOLD;
    if (style_index & 2) style_flags |= STYLE_ITALIC;
    def->font = wp_get_font(font_id, style_flags);
    if (!def->font || !def->font->valid) {
        snprintf(err, (size_t)err_len, "Failed to load font %s", base_name);
        return false;
    }

    def->descriptor_flags = wp_pdf_descriptor_flags(font_id, style_index);
    def->italic_angle = (style_index == 2 || style_index == 3) ? -12 : 0;
    def->units_per_em = wp_pdf_units_per_em(def->font);

    stbtt_GetFontBoundingBox(&def->font->info,
                             &def->bbox_x0, &def->bbox_y0,
                             &def->bbox_x1, &def->bbox_y1);
    int asc = 0;
    int desc = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&def->font->info, &asc, &desc, &line_gap);
    (void)line_gap;
    def->ascent = (asc * 1000 + def->units_per_em / 2) / def->units_per_em;
    def->descent = (desc * 1000 - def->units_per_em / 2) / def->units_per_em;
    def->cap_height = def->ascent > 0 ? def->ascent : 700;

    return wp_pdf_load_font_data(def, err, err_len);
}

static void wp_pdf_free_fonts(WpPdfFontDef* defs, int count) {
    if (!defs) return;
    for (int i = 0; i < count; i++) {
        if (defs[i].file_data) {
            montauk::mfree(defs[i].file_data);
            defs[i].file_data = nullptr;
            defs[i].file_len = 0;
        }
    }
}

static bool wp_pdf_collect_fonts(WordProcessorState* wp,
                                 WpPdfFontDef* defs,
                                 int* out_count,
                                 char* err, int err_len) {
    if (out_count) *out_count = 0;
    if (!wp || !defs) return false;

    bool used[FONT_COUNT][4] = {};
    for (int i = 0; i < wp->run_count; i++) {
        StyledRun* run = &wp->runs[i];
        if (run->len <= 0) continue;
        int font_id = run->font_id < FONT_COUNT ? run->font_id : FONT_ROBOTO;
        int style_index = wp_pdf_style_index(run->flags);
        used[font_id][style_index] = true;
    }

    int count = 0;
    for (int font_id = 0; font_id < FONT_COUNT; font_id++) {
        for (int style_index = 0; style_index < 4; style_index++) {
            if (!used[font_id][style_index]) continue;
            if (!wp_pdf_prepare_font_def(&defs[count],
                                         (uint8_t)font_id,
                                         (uint8_t)style_index,
                                         count + 1,
                                         err, err_len)) {
                wp_pdf_free_fonts(defs, count + 1);
                return false;
            }
            count++;
        }
    }

    if (out_count) *out_count = count;
    return true;
}

static const WpPdfFontDef* wp_pdf_find_font_def(const WpPdfFontDef* defs,
                                                int count,
                                                uint8_t font_id,
                                                uint8_t flags) {
    int want_style = wp_pdf_style_index(flags);
    int want_font = font_id < FONT_COUNT ? font_id : FONT_ROBOTO;
    for (int i = 0; i < count; i++) {
        if (defs[i].font_id == want_font && defs[i].style_index == want_style)
            return &defs[i];
    }
    return nullptr;
}

static bool wp_pdf_append_text_op(WpPdfBuffer* buf,
                                  const WpPdfFontDef* font,
                                  int font_points,
                                  int x_pt_hundredths,
                                  int baseline_pt_hundredths,
                                  const char* text,
                                  int text_len) {
    if (!buf || !font || !text || text_len <= 0) return true;
    return wp_pdf_buffer_appendf(buf, "BT\n/%s %d Tf\n1 0 0 1 ",
                                 font->resource_name, font_points)
        && wp_pdf_buffer_append_number_hundredths(buf, x_pt_hundredths)
        && wp_pdf_buffer_appendf(buf, " ")
        && wp_pdf_buffer_append_number_hundredths(buf, baseline_pt_hundredths)
        && wp_pdf_buffer_appendf(buf, " Tm\n")
        && wp_pdf_buffer_append_hex_string(buf, text, text_len)
        && wp_pdf_buffer_appendf(buf, " Tj\nET\n");
}

static bool wp_pdf_append_stroke_line(WpPdfBuffer* buf,
                                      int x0_px, int y0_px,
                                      int x1_px, int y1_px,
                                      int thickness_px) {
    if (!buf) return false;
    int width_pt_hundredths = wp_pdf_points_hundredths_from_print_px(thickness_px);
    if (width_pt_hundredths < 25) width_pt_hundredths = 25;
    return wp_pdf_buffer_append_number_hundredths(buf, width_pt_hundredths)
        && wp_pdf_buffer_appendf(buf, " w\n")
        && wp_pdf_buffer_append_number_hundredths(buf, wp_pdf_points_hundredths_from_print_px(x0_px))
        && wp_pdf_buffer_appendf(buf, " ")
        && wp_pdf_buffer_append_number_hundredths(buf, wp_pdf_y_points_hundredths_from_print_top(y0_px))
        && wp_pdf_buffer_appendf(buf, " m\n")
        && wp_pdf_buffer_append_number_hundredths(buf, wp_pdf_points_hundredths_from_print_px(x1_px))
        && wp_pdf_buffer_appendf(buf, " ")
        && wp_pdf_buffer_append_number_hundredths(buf, wp_pdf_y_points_hundredths_from_print_top(y1_px))
        && wp_pdf_buffer_appendf(buf, " l\nS\n");
}

static bool wp_pdf_append_divider_ops(WpPdfBuffer* buf,
                                      int x_px, int y_px, int w_px, int h_px,
                                      uint8_t divider_type) {
    if (!buf || w_px <= 0 || h_px <= 0 || divider_type == PARA_DIVIDER_NONE) return true;

    int cy = y_px + h_px / 2;
    switch (divider_type) {
    case PARA_DIVIDER_SINGLE:
        return wp_pdf_append_stroke_line(buf, x_px, cy, x_px + w_px, cy, 1);
    case PARA_DIVIDER_DOUBLE:
        return wp_pdf_append_stroke_line(buf, x_px, cy - 2, x_px + w_px, cy - 2, 1)
            && wp_pdf_append_stroke_line(buf, x_px, cy + 2, x_px + w_px, cy + 2, 1);
    case PARA_DIVIDER_DOTTED:
        for (int dx = x_px; dx < x_px + w_px; dx += 5) {
            int dot_w = (x_px + w_px - dx) < 2 ? (x_px + w_px - dx) : 2;
            if (dot_w > 0 &&
                !wp_pdf_append_stroke_line(buf, dx, cy, dx + dot_w, cy, 1))
                return false;
        }
        return true;
    case PARA_DIVIDER_DASHED:
        for (int dx = x_px; dx < x_px + w_px; dx += 11) {
            int dash_w = (x_px + w_px - dx) < 7 ? (x_px + w_px - dx) : 7;
            if (dash_w > 0 &&
                !wp_pdf_append_stroke_line(buf, dx, cy, dx + dash_w, cy, 1))
                return false;
        }
        return true;
    case PARA_DIVIDER_HEAVY:
        return wp_pdf_append_stroke_line(buf, x_px, cy, x_px + w_px, cy, 3);
    case PARA_DIVIDER_THIN_THICK:
        return wp_pdf_append_stroke_line(buf, x_px, cy - 3, x_px + w_px, cy - 3, 1)
            && wp_pdf_append_stroke_line(buf, x_px, cy, x_px + w_px, cy, 3);
    case PARA_DIVIDER_THICK_THIN:
        return wp_pdf_append_stroke_line(buf, x_px, cy - 3, x_px + w_px, cy - 3, 3)
            && wp_pdf_append_stroke_line(buf, x_px, cy + 2, x_px + w_px, cy + 2, 1);
    default:
        return true;
    }
}

static int wp_pdf_measure_line_width_hundredths(WordProcessorState* wp,
                                                const WrapLine* wl,
                                                const WpPdfFontDef* defs,
                                                int def_count) {
    if (!wp || !wl) return 0;

    int width = 0;
    int chars_left = wl->char_count;
    int ri = wl->run_idx;
    int ro = wl->run_offset;

    while (chars_left > 0 && ri < wp->run_count) {
        StyledRun* run = &wp->runs[ri];
        const WpPdfFontDef* font_def = wp_pdf_find_font_def(defs, def_count, run->font_id, run->flags);
        int avail = run->len - ro;
        int to_measure = avail < chars_left ? avail : chars_left;

        for (int ci = 0; ci < to_measure; ci++) {
            unsigned char ch = (unsigned char)run->text[ro + ci];
            if (ch != '\n' && (ch >= 32 || (char)ch < 0))
                width += wp_pdf_glyph_advance_hundredths(font_def, run->size, ch);
        }

        chars_left -= to_measure;
        ro += to_measure;
        if (ro >= run->len) {
            ri++;
            ro = 0;
        }
    }

    return width;
}

static bool wp_pdf_append_list_marker(WpPdfBuffer* buf,
                                      WordProcessorState* wp,
                                      WrapLine* wl,
                                      int py,
                                      const WpPdfFontDef* defs,
                                      int def_count) {
    if (!buf || !wp || !wl || !wl->first_in_paragraph ||
        wl->paragraph_idx < 0 || wl->paragraph_idx >= wp->paragraph_count)
        return true;

    ParagraphStyle* para = &wp->paragraphs[wl->paragraph_idx];
    if (para->divider_type != PARA_DIVIDER_NONE || para->list_type == PARA_LIST_NONE)
        return true;
    if (wp->run_count <= 0) return true;

    StyledRun* run = &wp->runs[wl->run_idx < wp->run_count ? wl->run_idx : 0];
    const WpPdfFontDef* font_def = wp_pdf_find_font_def(defs, def_count, run->font_id, run->flags);
    if (!font_def) return true;

    int marker_x = WP_PRINT_MARGIN_X + wp_scale_layout_units(para->left_indent + para->first_line_indent, WP_PRINT_DPI);
    int min_x = WP_PRINT_MARGIN_X - WP_MARGIN + 4;
    if (marker_x < min_x) marker_x = min_x;

    int baseline_pt_hundredths = wp_pdf_y_points_hundredths_from_print_top(py + wl->baseline);
    int marker_x_pt_hundredths = wp_pdf_points_hundredths_from_print_px(marker_x);

    if (para->list_type == PARA_LIST_BULLET) {
        char bullet[1] = { (char)0x95 };
        return wp_pdf_append_text_op(buf, font_def, run->size,
                                     marker_x_pt_hundredths, baseline_pt_hundredths,
                                     bullet, 1);
    }

    char label[16];
    int label_len = snprintf(label, sizeof(label), "%d.", wl->list_number > 0 ? wl->list_number : 1);
    if (label_len <= 0) return true;
    return wp_pdf_append_text_op(buf, font_def, run->size,
                                 marker_x_pt_hundredths, baseline_pt_hundredths,
                                 label, label_len);
}

static bool wp_pdf_build_page_stream(WordProcessorState* wp,
                                     const int* line_pages,
                                     const int* line_y,
                                     int page_index,
                                     const WpPdfFontDef* defs,
                                     int def_count,
                                     WpPdfPageStream* out_stream,
                                     char* err, int err_len) {
    if (!out_stream) return false;
    out_stream->data = nullptr;
    out_stream->len = 0;

    WpPdfBuffer buf = {};
    if (!wp_pdf_buffer_init(&buf, 4096)) {
        wp_status_copy(err, err_len, "Out of memory while building PDF");
        return false;
    }

    bool ok = wp_pdf_buffer_appendf(&buf, "0 g\n0 G\n");
    for (int li = 0; ok && li < wp->wrap_line_count; li++) {
        if (line_pages[li] != page_index) continue;

        WrapLine* wl = &wp->wrap_lines[li];
        int py = line_y[li];

        ok = wp_pdf_append_list_marker(&buf, wp, wl, py, defs, def_count);
        if (!ok) break;

        if (wl->divider_type != PARA_DIVIDER_NONE) {
            int divider_x = WP_PRINT_MARGIN_X + (wl->x - WP_MARGIN);
            ok = wp_pdf_append_divider_ops(&buf, divider_x, py, wl->width, wl->height, wl->divider_type);
            if (!ok) break;
        }

        int chars_left = wl->char_count;
        int ri = wl->run_idx;
        int ro = wl->run_offset;
        int x_pt_hundredths = wp_pdf_points_hundredths_from_print_px(WP_PRINT_MARGIN_X + (wl->x - WP_MARGIN));
        if (wl->paragraph_idx >= 0 && wl->paragraph_idx < wp->paragraph_count) {
            ParagraphStyle* para = &wp->paragraphs[wl->paragraph_idx];
            if (para->align == PARA_ALIGN_CENTER || para->align == PARA_ALIGN_RIGHT) {
                int line_width_pt_hundredths = wp_pdf_measure_line_width_hundredths(wp, wl, defs, def_count);
                int width_delta = wp_pdf_points_hundredths_from_print_px(wl->width) - line_width_pt_hundredths;
                if (para->align == PARA_ALIGN_CENTER) x_pt_hundredths += width_delta / 2;
                else x_pt_hundredths += width_delta;
            }
        }

        while (ok && chars_left > 0 && ri < wp->run_count) {
            StyledRun* run = &wp->runs[ri];
            const WpPdfFontDef* font_def = wp_pdf_find_font_def(defs, def_count, run->font_id, run->flags);
            if (!font_def) {
                ri++;
                ro = 0;
                continue;
            }

            int avail = run->len - ro;
            int to_draw = avail < chars_left ? avail : chars_left;

            char segment[512];
            int seg_len = 0;
            int seg_x_pt_hundredths = x_pt_hundredths;
            int baseline_px = py + wl->baseline;

            for (int ci = 0; ci < to_draw; ci++) {
                char ch = run->text[ro + ci];
                int advance_hundredths = 0;
                if (ch != '\n' && (ch >= 32 || ch < 0))
                    advance_hundredths = wp_pdf_glyph_advance_hundredths(font_def, run->size, (unsigned char)ch);

                if (ch != '\n' && (ch >= 32 || ch < 0)) {
                    if (seg_len >= (int)sizeof(segment)) {
                        ok = wp_pdf_append_text_op(&buf, font_def, run->size,
                                                   seg_x_pt_hundredths,
                                                   wp_pdf_y_points_hundredths_from_print_top(baseline_px),
                                                   segment, seg_len);
                        if (!ok) break;
                        seg_len = 0;
                        seg_x_pt_hundredths = x_pt_hundredths;
                    }
                    segment[seg_len++] = ch;
                }

                x_pt_hundredths += advance_hundredths;
            }

            if (seg_len > 0) {
                ok = wp_pdf_append_text_op(&buf, font_def, run->size,
                                           seg_x_pt_hundredths,
                                           wp_pdf_y_points_hundredths_from_print_top(baseline_px),
                                           segment, seg_len);
            }

            chars_left -= to_draw;
            ro += to_draw;
            if (ro >= run->len) {
                ri++;
                ro = 0;
            }
        }
    }

    if (!ok) {
        wp_status_copy(err, err_len, "Failed while writing PDF page data");
        wp_pdf_buffer_free(&buf);
        return false;
    }

    out_stream->data = buf.data;
    out_stream->len = buf.len;
    return true;
}

static bool wp_pdf_append_font_widths(WpPdfBuffer* pdf, const WpPdfFontDef* def) {
    if (!pdf || !def || !def->font || !def->font->valid) return false;
    if (!wp_pdf_buffer_appendf(pdf, "/Widths [")) return false;

    for (int ch = 32; ch <= 255; ch++) {
        int advance = 0;
        int lsb = 0;
        int cp = decode_single_byte_codepoint(ch);
        stbtt_GetCodepointHMetrics(&def->font->info, cp, &advance, &lsb);
        int width = (advance * 1000 + def->units_per_em / 2) / def->units_per_em;
        if (width < 0) width = 0;
        if (!wp_pdf_buffer_appendf(pdf, "%d%s", width, ch == 255 ? "" : " "))
            return false;
    }

    return wp_pdf_buffer_appendf(pdf, "] ");
}

bool wp_export_pdf_document(WordProcessorState* wp, const char* path, char* out_status, int out_status_len) {
    if (out_status && out_status_len > 0) out_status[0] = '\0';
    if (!wp) {
        wp_status_copy(out_status, out_status_len, "No document loaded");
        return false;
    }

    char final_path[256] = {};
    char err[192] = {};
    if (!wp_pdf_normalize_path(path, final_path, sizeof(final_path), err, sizeof(err))) {
        wp_status_copy(out_status, out_status_len, err);
        return false;
    }

    int* line_pages = nullptr;
    int* line_y = nullptr;
    int page_count = 0;
    if (!wp_build_print_page_map(wp, &line_pages, &line_y, &page_count, err, sizeof(err))) {
        wp_status_copy(out_status, out_status_len, err);
        return false;
    }

    bool success = false;
    WpPdfFontDef fonts[FONT_COUNT * 4] = {};
    int font_count = 0;
    WpPdfPageStream* page_streams = nullptr;
    int* page_obj_nums = nullptr;
    int* content_obj_nums = nullptr;
    int* xref = nullptr;
    WpPdfBuffer pdf = {};
    int next_obj = 0;
    int total_objects = 0;

    if (!wp_pdf_collect_fonts(wp, fonts, &font_count, err, sizeof(err))) {
        wp_status_copy(out_status, out_status_len, err);
        goto cleanup;
    }

    page_streams = (WpPdfPageStream*)montauk::malloc((uint64_t)page_count * sizeof(WpPdfPageStream));
    if (!page_streams) {
        wp_status_copy(out_status, out_status_len, "Out of memory while exporting PDF");
        goto cleanup;
    }
    montauk::memset(page_streams, 0, (uint64_t)page_count * sizeof(WpPdfPageStream));

    for (int page = 0; page < page_count; page++) {
        if (!wp_pdf_build_page_stream(wp, line_pages, line_y, page, fonts, font_count,
                                      &page_streams[page], err, sizeof(err))) {
            wp_status_copy(out_status, out_status_len, err);
            goto cleanup;
        }
    }

    next_obj = 3;
    for (int i = 0; i < font_count; i++) {
        fonts[i].font_obj_num = next_obj++;
        fonts[i].descriptor_obj_num = next_obj++;
        fonts[i].file_obj_num = next_obj++;
    }

    page_obj_nums = (int*)montauk::malloc((uint64_t)page_count * sizeof(int));
    content_obj_nums = (int*)montauk::malloc((uint64_t)page_count * sizeof(int));
    if (!page_obj_nums || !content_obj_nums) {
        wp_status_copy(out_status, out_status_len, "Out of memory while exporting PDF");
        goto cleanup;
    }
    for (int page = 0; page < page_count; page++) {
        page_obj_nums[page] = next_obj++;
        content_obj_nums[page] = next_obj++;
    }

    total_objects = next_obj - 1;
    xref = (int*)montauk::malloc((uint64_t)(total_objects + 1) * sizeof(int));
    if (!xref) {
        wp_status_copy(out_status, out_status_len, "Out of memory while exporting PDF");
        goto cleanup;
    }
    montauk::memset(xref, 0, (uint64_t)(total_objects + 1) * sizeof(int));

    if (!wp_pdf_buffer_init(&pdf, 16384)) {
        wp_status_copy(out_status, out_status_len, "Out of memory while exporting PDF");
        goto cleanup;
    }

    if (!wp_pdf_buffer_appendf(&pdf, "%%PDF-1.4\n%%MontaukOS\n")) {
        wp_status_copy(out_status, out_status_len, "Failed to build PDF output");
        goto cleanup;
    }

    xref[1] = pdf.len;
    if (!wp_pdf_buffer_appendf(&pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) {
        wp_status_copy(out_status, out_status_len, "Failed to build PDF output");
        goto cleanup;
    }

    xref[2] = pdf.len;
    if (!wp_pdf_buffer_appendf(&pdf, "2 0 obj\n<< /Type /Pages /Kids [")) {
        wp_status_copy(out_status, out_status_len, "Failed to build PDF output");
        goto cleanup;
    }
    for (int page = 0; page < page_count; page++) {
        if (!wp_pdf_buffer_appendf(&pdf, "%d 0 R%s", page_obj_nums[page], page == page_count - 1 ? "" : " ")) {
            wp_status_copy(out_status, out_status_len, "Failed to build PDF output");
            goto cleanup;
        }
    }
    if (!wp_pdf_buffer_appendf(&pdf, "] /Count %d >>\nendobj\n", page_count)) {
        wp_status_copy(out_status, out_status_len, "Failed to build PDF output");
        goto cleanup;
    }

    for (int i = 0; i < font_count; i++) {
        WpPdfFontDef* def = &fonts[i];

        xref[def->font_obj_num] = pdf.len;
        if (!wp_pdf_buffer_appendf(&pdf,
                                   "%d 0 obj\n<< /Type /Font /Subtype /TrueType /Name /%s "
                                   "/BaseFont /%s /FirstChar 32 /LastChar 255 ",
                                   def->font_obj_num, def->resource_name, def->base_font)
            || !wp_pdf_append_font_widths(&pdf, def)
            || !wp_pdf_buffer_appendf(&pdf,
                                      "/Encoding /WinAnsiEncoding /FontDescriptor %d 0 R >>\nendobj\n",
                                      def->descriptor_obj_num)) {
            wp_status_copy(out_status, out_status_len, "Failed to encode PDF font data");
            goto cleanup;
        }

        xref[def->descriptor_obj_num] = pdf.len;
        if (!wp_pdf_buffer_appendf(&pdf,
                                   "%d 0 obj\n<< /Type /FontDescriptor /FontName /%s "
                                   "/Flags %d /FontBBox [%d %d %d %d] /ItalicAngle %d "
                                   "/Ascent %d /Descent %d /CapHeight %d /StemV 80 "
                                   "/FontFile2 %d 0 R >>\nendobj\n",
                                   def->descriptor_obj_num, def->base_font,
                                   def->descriptor_flags,
                                   def->bbox_x0, def->bbox_y0, def->bbox_x1, def->bbox_y1,
                                   def->italic_angle, def->ascent, def->descent,
                                   def->cap_height, def->file_obj_num)) {
            wp_status_copy(out_status, out_status_len, "Failed to encode PDF font data");
            goto cleanup;
        }

        xref[def->file_obj_num] = pdf.len;
        if (!wp_pdf_buffer_appendf(&pdf, "%d 0 obj\n<< /Length %d >>\nstream\n",
                                   def->file_obj_num, def->file_len)
            || !wp_pdf_buffer_append_bytes(&pdf, def->file_data, def->file_len)
            || !wp_pdf_buffer_appendf(&pdf, "\nendstream\nendobj\n")) {
            wp_status_copy(out_status, out_status_len, "Failed to encode embedded PDF fonts");
            goto cleanup;
        }
    }

    for (int page = 0; page < page_count; page++) {
        xref[page_obj_nums[page]] = pdf.len;
        if (!wp_pdf_buffer_appendf(&pdf,
                                   "%d 0 obj\n<< /Type /Page /Parent 2 0 R "
                                   "/MediaBox [0 0 ",
                                   page_obj_nums[page])
            || !wp_pdf_buffer_append_number_hundredths(&pdf, wp_pdf_page_width_points_hundredths())
            || !wp_pdf_buffer_appendf(&pdf, " ")
            || !wp_pdf_buffer_append_number_hundredths(&pdf, wp_pdf_page_height_points_hundredths())
            || !wp_pdf_buffer_appendf(&pdf, "] /Resources <<")) {
            wp_status_copy(out_status, out_status_len, "Failed to encode PDF pages");
            goto cleanup;
        }

        if (font_count > 0) {
            if (!wp_pdf_buffer_appendf(&pdf, " /Font <<")) {
                wp_status_copy(out_status, out_status_len, "Failed to encode PDF pages");
                goto cleanup;
            }
            for (int i = 0; i < font_count; i++) {
                if (!wp_pdf_buffer_appendf(&pdf, " /%s %d 0 R",
                                           fonts[i].resource_name, fonts[i].font_obj_num)) {
                    wp_status_copy(out_status, out_status_len, "Failed to encode PDF pages");
                    goto cleanup;
                }
            }
            if (!wp_pdf_buffer_appendf(&pdf, " >>")) {
                wp_status_copy(out_status, out_status_len, "Failed to encode PDF pages");
                goto cleanup;
            }
        }

        if (!wp_pdf_buffer_appendf(&pdf, " >> /Contents %d 0 R >>\nendobj\n", content_obj_nums[page])) {
            wp_status_copy(out_status, out_status_len, "Failed to encode PDF pages");
            goto cleanup;
        }

        xref[content_obj_nums[page]] = pdf.len;
        if (!wp_pdf_buffer_appendf(&pdf, "%d 0 obj\n<< /Length %d >>\nstream\n",
                                   content_obj_nums[page], page_streams[page].len)
            || !wp_pdf_buffer_append_bytes(&pdf, page_streams[page].data, page_streams[page].len)
            || !wp_pdf_buffer_appendf(&pdf, "\nendstream\nendobj\n")) {
            wp_status_copy(out_status, out_status_len, "Failed to encode PDF pages");
            goto cleanup;
        }
    }

    {
        int xref_pos = pdf.len;
        if (!wp_pdf_buffer_appendf(&pdf, "xref\n0 %d\n", total_objects + 1)
            || !wp_pdf_buffer_appendf(&pdf, "%010d 65535 f \n", 0)) {
            wp_status_copy(out_status, out_status_len, "Failed to finalize PDF output");
            goto cleanup;
        }

        for (int i = 1; i <= total_objects; i++) {
            if (!wp_pdf_buffer_appendf(&pdf, "%010d 00000 n \n", xref[i])) {
                wp_status_copy(out_status, out_status_len, "Failed to finalize PDF output");
                goto cleanup;
            }
        }

        if (!wp_pdf_buffer_appendf(&pdf,
                                   "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n",
                                   total_objects + 1, xref_pos)) {
            wp_status_copy(out_status, out_status_len, "Failed to finalize PDF output");
            goto cleanup;
        }
    }

    {
        int fd = montauk::fcreate(final_path);
        if (fd < 0) {
            snprintf(err, sizeof(err), "Cannot create %s", final_path);
            wp_status_copy(out_status, out_status_len, err);
            goto cleanup;
        }
        int wrote = montauk::fwrite(fd, pdf.data, 0, (uint64_t)pdf.len);
        montauk::close(fd);
        if (wrote < 0 || wrote != pdf.len) {
            snprintf(err, sizeof(err), "Failed to write %s", final_path);
            wp_status_copy(out_status, out_status_len, err);
            goto cleanup;
        }
    }

    {
        char summary[192];
        snprintf(summary, sizeof(summary),
                 "Exported PDF to %s (%d page%s)",
                 final_path, page_count, page_count == 1 ? "" : "s");
        wp_status_copy(out_status, out_status_len, summary);
    }
    success = true;

cleanup:
    if (page_streams) {
        for (int i = 0; i < page_count; i++) {
            if (page_streams[i].data)
                montauk::mfree(page_streams[i].data);
        }
        montauk::mfree(page_streams);
    }
    if (page_obj_nums) montauk::mfree(page_obj_nums);
    if (content_obj_nums) montauk::mfree(content_obj_nums);
    if (xref) montauk::mfree(xref);
    if (line_pages) montauk::mfree(line_pages);
    if (line_y) montauk::mfree(line_y);
    wp_pdf_free_fonts(fonts, font_count);
    wp_pdf_buffer_free(&pdf);

    return success;
}
