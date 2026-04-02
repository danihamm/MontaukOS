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

    wp_recompute_wrap(wp, wp_print_layout_width());
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
    if (para->list_type == PARA_LIST_NONE) return;

    int marker_x = WP_PRINT_MARGIN_X + para->left_indent + para->first_line_indent;
    int min_x = WP_PRINT_MARGIN_X - WP_MARGIN + 4;
    if (marker_x < min_x) marker_x = min_x;

    if (para->list_type == PARA_LIST_BULLET) {
        fill_circle(c, marker_x + 7, py + wl->height / 2, 3, colors::BLACK);
        return;
    }

    if (wp->run_count <= 0) return;
    StyledRun* run = &wp->runs[wl->run_idx < wp->run_count ? wl->run_idx : 0];
    TrueTypeFont* font = wp_get_font(run->font_id, run->flags);
    char label[16];
    snprintf(label, sizeof(label), "%d.", wl->list_number > 0 ? wl->list_number : 1);
    int top_y = py + (wl->height - run->size) / 2;
    draw_text(c, font ? font : g_ui_font, marker_x, top_y, label, colors::BLACK, run->size);
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

            GlyphCache* gc = font->get_cache(r->size);
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
