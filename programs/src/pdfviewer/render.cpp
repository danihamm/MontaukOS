/*
 * render.cpp
 * UI rendering -- toolbar, page content, status bar
 * Copyright (c) 2026 Daniel Hammer
 */

#include "pdfviewer.h"

void render(uint32_t* pixels) {
    // ---- Background ----
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, g_win_h, BG_COLOR);

    // ---- Path bar (Open dialog) ----
    int pathbar_h = g_pathbar_open ? PATHBAR_H : 0;

    // ---- Page content area ----
    int content_y = TOOLBAR_H + pathbar_h;
    int content_h = g_win_h - content_y - STATUS_BAR_H;

    if (g_doc.valid && g_current_page >= 0 && g_current_page < g_doc.page_count) {
        PdfPage* page = &g_doc.pages[g_current_page];

        int page_w = (int)(page->width * g_zoom);
        int page_h = (int)(page->height * g_zoom);

        // Center page horizontally
        int page_x = (g_win_w - page_w) / 2;
        if (page_x < PAGE_MARGIN) page_x = PAGE_MARGIN;

        int page_y = content_y + PAGE_MARGIN - g_scroll_y;

        // Draw page shadow
        px_fill(pixels, g_win_w, g_win_h,
                page_x + 3, page_y + 3, page_w, page_h, PAGE_SHADOW);

        // Draw page background (clip to content area)
        int clip_y0 = content_y;
        int clip_y1 = content_y + content_h;

        // Draw white page
        for (int row = page_y; row < page_y + page_h; row++) {
            if (row < clip_y0 || row >= clip_y1) continue;
            int x0 = page_x < 0 ? 0 : page_x;
            int x1 = page_x + page_w;
            if (x1 > g_win_w) x1 = g_win_w;
            uint32_t white = PAGE_COLOR.to_pixel();
            for (int col = x0; col < x1; col++)
                pixels[row * g_win_w + col] = white;
        }

        // Draw text items
        for (int i = 0; i < page->item_count; i++) {
            TextItem* item = &page->items[i];

            // Convert PDF coords to screen coords
            // PDF: origin bottom-left, y up
            // Screen: origin top-left, y down
            int sx = page_x + (int)(item->x * g_zoom);
            int sy = page_y + (int)((page->height - item->y) * g_zoom);

            // Skip if outside visible area
            if (sy < clip_y0 - 30 || sy > clip_y1) continue;

            // Choose font: prefer embedded, fall back to system fonts
            TrueTypeFont* font = item->font;
            if (!font) {
                font = g_font;
                if ((item->flags & 1) && g_font_bold) font = g_font_bold;
                if ((item->flags & 4) && g_font_mono) font = g_font_mono;
            }
            if (!font) continue;

            // Scale font size
            int px_size = (int)(item->font_size * g_zoom + 0.5f);
            if (px_size < 4) px_size = 4;
            if (px_size > 120) px_size = 120;

            if (item->font) {
                // Embedded font: pass raw character codes directly
                // (subset fonts use codes 0-N that map through the font's cmap)
                font->draw_to_buffer(pixels, g_win_w, g_win_h,
                    sx, sy, item->text, TEXT_COLOR, px_size);
            } else {
                // System font: filter out non-printable characters
                char render_text[MAX_TEXT_LEN];
                int ri = 0;
                for (int j = 0; item->text[j] && ri < MAX_TEXT_LEN - 1; j++) {
                    char c = item->text[j];
                    if (c >= 32 && c < 127) {
                        render_text[ri++] = c;
                    } else if (c == '\t') {
                        render_text[ri++] = ' ';
                    }
                }
                render_text[ri] = '\0';

                if (ri > 0) {
                    font->draw_to_buffer(pixels, g_win_w, g_win_h,
                        sx, sy, render_text, TEXT_COLOR, px_size);
                }
            }
        }

        // Page border
        if (page_y < clip_y1 && page_y + page_h > clip_y0) {
            px_rect(pixels, g_win_w, g_win_h,
                    page_x, page_y, page_w, page_h,
                    Color::from_rgb(0xAA, 0xAA, 0xAA));
        }
    } else if (!g_doc.valid) {
        // No document loaded - show help text
        if (g_font) {
            const char* msg = "Press Ctrl+O or click Open to load a PDF file";
            int tw = g_font->measure_text(msg, FONT_SIZE);
            int tx = (g_win_w - tw) / 2;
            int ty = content_y + content_h / 2 - FONT_SIZE / 2;
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                tx, ty, msg, Color::from_rgb(0xCC, 0xCC, 0xCC), FONT_SIZE);
        }
    }

    // ---- Toolbar (drawn after content so it stays on top) ----
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, TOOLBAR_H, TOOLBAR_BG);
    px_hline(pixels, g_win_w, g_win_h, 0, TOOLBAR_H - 1, g_win_w, GRID_COLOR);

    int bx = 4;
    auto tb_btn = [&](int w, bool active, const char* label) {
        Color bg = active ? TB_BTN_ACTIVE : TB_BTN_BG;
        px_fill_rounded(pixels, g_win_w, g_win_h, bx, TB_BTN_Y, w, TB_BTN_SIZE, TB_BTN_RAD, bg);
        if (g_font && label[0]) {
            TrueTypeFont* f = g_font;
            int tw = f->measure_text(label, HEADER_FONT);
            f->draw_to_buffer(pixels, g_win_w, g_win_h,
                bx + (w - tw) / 2, TB_BTN_Y + (TB_BTN_SIZE - HEADER_FONT) / 2,
                label, HEADER_TEXT, HEADER_FONT);
        }
        bx += w + 4;
    };
    auto tb_sep = [&]() {
        px_vline(pixels, g_win_w, g_win_h, bx, 6, TOOLBAR_H - 12, TB_SEP_COLOR);
        bx += 8;
    };

    // Open
    tb_btn(36, false, "Open");
    tb_sep();

    // Navigation
    tb_btn(24, false, "<");
    tb_btn(24, false, ">");
    tb_sep();

    // Zoom
    tb_btn(24, false, "-");
    tb_btn(24, false, "+");

    // Zoom percentage label
    {
        char zoom_label[16];
        int pct = (int)(g_zoom * 100 + 0.5f);
        snprintf(zoom_label, 16, "%d%%", pct);
        if (g_font) {
            int tw = g_font->measure_text(zoom_label, HEADER_FONT);
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                bx, TB_BTN_Y + (TB_BTN_SIZE - HEADER_FONT) / 2,
                zoom_label, HEADER_TEXT, HEADER_FONT);
            bx += tw + 8;
        }
    }

    // ---- Path bar (Open dialog, drawn after content) ----
    if (g_pathbar_open) {
        int pby = TOOLBAR_H;
        px_fill(pixels, g_win_w, g_win_h, 0, pby, g_win_w, PATHBAR_H, HEADER_BG);
        px_hline(pixels, g_win_w, g_win_h, 0, pby + PATHBAR_H - 1, g_win_w, GRID_COLOR);

        const char* label = "Open:";
        if (g_font) {
            int lw = g_font->measure_text(label, HEADER_FONT);
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                8, pby + (PATHBAR_H - HEADER_FONT) / 2, label, HEADER_TEXT, HEADER_FONT);

            int inp_x = 16 + lw;
            int inp_w = g_win_w - inp_x - 8;
            px_fill(pixels, g_win_w, g_win_h, inp_x, pby + 4, inp_w, PATHBAR_H - 8, PAGE_COLOR);
            px_rect(pixels, g_win_w, g_win_h, inp_x, pby + 4, inp_w, PATHBAR_H - 8, GRID_COLOR);

            if (g_pathbar_text[0])
                g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                    inp_x + 4, pby + (PATHBAR_H - FONT_SIZE) / 2,
                    g_pathbar_text, TEXT_COLOR, FONT_SIZE);

            // Cursor
            char prefix[256];
            int plen = g_pathbar_cursor < 255 ? g_pathbar_cursor : 255;
            for (int i = 0; i < plen; i++) prefix[i] = g_pathbar_text[i];
            prefix[plen] = '\0';
            int cx = inp_x + 4 + g_font->measure_text(prefix, FONT_SIZE);
            Color cursor_color = Color::from_rgb(0x36, 0x7B, 0xF0);
            px_fill(pixels, g_win_w, g_win_h, cx, pby + 6, 2, PATHBAR_H - 12, cursor_color);
        }
    }

    // ---- Status bar ----
    int sy = g_win_h - STATUS_BAR_H;
    px_fill(pixels, g_win_w, g_win_h, 0, sy, g_win_w, STATUS_BAR_H, STATUS_BG);

    if (g_font) {
        // Left: filename or status message
        char left_text[128];
        if (g_filepath[0]) {
            const char* fname = g_filepath;
            for (int i = 0; g_filepath[i]; i++)
                if (g_filepath[i] == '/') fname = g_filepath + i + 1;
            snprintf(left_text, 128, " %s", fname);
        } else if (g_status_msg[0]) {
            snprintf(left_text, 128, " %s", g_status_msg);
        } else {
            str_cpy(left_text, " PDF Viewer", 128);
        }
        int sty = sy + (STATUS_BAR_H - HEADER_FONT) / 2;
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h, 6, sty, left_text, STATUS_TEXT, HEADER_FONT);

        // Right: page number
        if (g_doc.valid) {
            char right_text[64];
            snprintf(right_text, 64, "Page %d of %d ", g_current_page + 1, g_doc.page_count);
            int rw = g_font->measure_text(right_text, HEADER_FONT);
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                g_win_w - rw - 6, sty, right_text, STATUS_TEXT, HEADER_FONT);
        }
    }
}
