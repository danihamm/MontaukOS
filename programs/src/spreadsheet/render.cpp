/*
 * render.cpp
 * Rendering — toolbar, grid, status bar, overlays
 * Copyright (c) 2026 Daniel Hammer
 */

#include "spreadsheet.h"

void render(uint32_t* pixels) {
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, g_win_h, BG_COLOR);

    // ---- Toolbar ----
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, TOOLBAR_H, TOOLBAR_BG);
    px_hline(pixels, g_win_w, g_win_h, 0, TOOLBAR_H - 1, g_win_w, GRID_COLOR);

    Cell* cur_cell = &g_cells[g_sel_row][g_sel_col];

    int bx = 4;
    auto tb_btn = [&](int w, bool active, const char* label) {
        Color bg = active ? TB_BTN_ACTIVE : TB_BTN_BG;
        px_fill_rounded(pixels, g_win_w, g_win_h, bx, TB_BTN_Y, w, TB_BTN_SIZE, TB_BTN_RAD, bg);
        if (g_font && label[0]) {
            TrueTypeFont* f = (active && g_font_bold) ? g_font_bold : g_font;
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

    // Open / Save
    tb_btn(36, false, "Open");
    tb_btn(36, false, "Save");
    tb_sep();

    // Cut/Copy/Paste
    tb_btn(28, false, "Cut");
    tb_btn(36, false, "Copy");
    tb_btn(36, false, "Paste");
    tb_sep();

    // Bold
    tb_btn(24, cur_cell->bold, "B");
    tb_sep();

    // Alignment
    tb_btn(24, cur_cell->align == ALIGN_LEFT || (cur_cell->align == ALIGN_AUTO && cur_cell->type == CT_TEXT), "L");
    tb_btn(24, cur_cell->align == ALIGN_CENTER, "C");
    tb_btn(24, cur_cell->align == ALIGN_RIGHT || (cur_cell->align == ALIGN_AUTO && (cur_cell->type == CT_NUMBER || cur_cell->type == CT_FORMULA)), "R");
    tb_sep();

    // Format dropdown
    {
        const char* fmt_labels[] = { "Auto", ".00", "$", "%" };
        int fi = (int)cur_cell->fmt;
        if (fi < 0 || fi > 3) fi = 0;
        char fmt_label[16];
        snprintf(fmt_label, 16, "%s v", fmt_labels[fi]);
        int fmt_w = 64;
        Color bg = g_fmt_dropdown_open ? TB_BTN_ACTIVE : TB_BTN_BG;
        px_fill_rounded(pixels, g_win_w, g_win_h, bx, TB_BTN_Y, fmt_w, TB_BTN_SIZE, TB_BTN_RAD, bg);
        if (g_font) {
            int tw = g_font->measure_text(fmt_label, HEADER_FONT);
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                bx + (fmt_w - tw) / 2, TB_BTN_Y + (TB_BTN_SIZE - HEADER_FONT) / 2,
                fmt_label, HEADER_TEXT, HEADER_FONT);
        }
        bx += fmt_w + 4;
        tb_sep();

        // Undo/Redo
        tb_btn(36, false, "Undo");
        tb_btn(36, false, "Redo");
    }

    // ---- Path bar (Open/Save As) ----
    int pathbar_h = g_pathbar_open ? PATHBAR_H : 0;
    if (g_pathbar_open) {
        int pby = TOOLBAR_H;
        px_fill(pixels, g_win_w, g_win_h, 0, pby, g_win_w, PATHBAR_H, HEADER_BG);
        px_hline(pixels, g_win_w, g_win_h, 0, pby + PATHBAR_H - 1, g_win_w, GRID_COLOR);

        const char* label = g_pathbar_save ? "Save as:" : "Open:";
        if (g_font) {
            int lw = g_font->measure_text(label, HEADER_FONT);
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                8, pby + (PATHBAR_H - HEADER_FONT) / 2, label, HEADER_TEXT, HEADER_FONT);

            int inp_x = 16 + lw;
            int inp_w = g_win_w - inp_x - 8;
            px_fill(pixels, g_win_w, g_win_h, inp_x, pby + 4, inp_w, PATHBAR_H - 8, BG_COLOR);
            px_rect(pixels, g_win_w, g_win_h, inp_x, pby + 4, inp_w, PATHBAR_H - 8, GRID_COLOR);

            if (g_pathbar_text[0])
                g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                    inp_x + 4, pby + (PATHBAR_H - FONT_SIZE) / 2,
                    g_pathbar_text, CELL_TEXT, FONT_SIZE);

            char prefix[256];
            int plen = g_pathbar_cursor < 255 ? g_pathbar_cursor : 255;
            for (int i = 0; i < plen; i++) prefix[i] = g_pathbar_text[i];
            prefix[plen] = '\0';
            int cx = inp_x + 4 + g_font->measure_text(prefix, FONT_SIZE);
            px_fill(pixels, g_win_w, g_win_h, cx, pby + 6, 2, PATHBAR_H - 12, SELECT_BORDER);
        }
    }

    // ---- Formula bar ----
    int fbar_y = TOOLBAR_H + pathbar_h;
    px_fill(pixels, g_win_w, g_win_h, 0, fbar_y, g_win_w, FORMULA_BAR_H, HEADER_BG);
    px_hline(pixels, g_win_w, g_win_h, 0, fbar_y + FORMULA_BAR_H - 1, g_win_w, GRID_COLOR);

    char name_buf[8];
    cell_name(name_buf, g_sel_col, g_sel_row);
    int fbar_ty = fbar_y + (FORMULA_BAR_H - HEADER_FONT) / 2;
    if (g_font)
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h, 8, fbar_ty, name_buf, HEADER_TEXT, HEADER_FONT);

    int sep_x = ROW_HEADER_W + 12;
    px_vline(pixels, g_win_w, g_win_h, sep_x, fbar_y + 4, FORMULA_BAR_H - 8, GRID_COLOR);

    {
        int fx = sep_x + 10;
        const char* display;
        if (g_editing) {
            display = g_edit_buf;
        } else {
            display = cur_cell->input;
        }
        if (g_font && display[0])
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h, fx, fbar_ty, display, CELL_TEXT, FONT_SIZE);

        if (g_editing && g_font) {
            char prefix[CELL_TEXT_MAX];
            int plen = g_edit_cursor < CELL_TEXT_MAX - 1 ? g_edit_cursor : CELL_TEXT_MAX - 1;
            for (int i = 0; i < plen; i++) prefix[i] = g_edit_buf[i];
            prefix[plen] = '\0';
            int cx = fx + g_font->measure_text(prefix, FONT_SIZE);
            px_fill(pixels, g_win_w, g_win_h, cx, fbar_y + 6, 2, FORMULA_BAR_H - 12, SELECT_BORDER);
        }
    }

    int area_y = TOOLBAR_H + pathbar_h + FORMULA_BAR_H;

    // ---- Column headers ----
    px_fill(pixels, g_win_w, g_win_h, 0, area_y, g_win_w, COL_HEADER_H, HEADER_BG);
    px_hline(pixels, g_win_w, g_win_h, 0, area_y + COL_HEADER_H - 1, g_win_w, GRID_COLOR);

    px_fill(pixels, g_win_w, g_win_h, 0, area_y, ROW_HEADER_W, COL_HEADER_H, HEADER_BG);
    px_vline(pixels, g_win_w, g_win_h, ROW_HEADER_W - 1, area_y, COL_HEADER_H, GRID_COLOR);

    for (int c = 0; c < MAX_COLS; c++) {
        int x = col_x(c) - g_scroll_x;
        if (x + g_col_widths[c] <= ROW_HEADER_W) continue;
        if (x >= g_win_w) break;

        {
            int sc0, sr0, sc1, sr1;
            sel_range(&sc0, &sr0, &sc1, &sr1);
            if (c >= sc0 && c <= sc1)
                px_fill(pixels, g_win_w, g_win_h, x, area_y, g_col_widths[c], COL_HEADER_H, SELECT_FILL);
        }

        char label[2] = { (char)('A' + c), '\0' };
        if (g_font) {
            int tw = g_font->measure_text(label, HEADER_FONT);
            int lx = x + (g_col_widths[c] - tw) / 2;
            if (lx >= ROW_HEADER_W)
                g_font->draw_to_buffer(pixels, g_win_w, g_win_h, lx, area_y + (COL_HEADER_H - HEADER_FONT) / 2, label, HEADER_TEXT, HEADER_FONT);
        }

        px_vline(pixels, g_win_w, g_win_h, x + g_col_widths[c] - 1, area_y, COL_HEADER_H, GRID_COLOR);
    }

    // ---- Row headers + grid ----
    int grid_y = area_y + COL_HEADER_H;
    int grid_h = g_win_h - TOOLBAR_H - pathbar_h - FORMULA_BAR_H - COL_HEADER_H - STATUS_BAR_H;

    for (int r = 0; r < MAX_ROWS; r++) {
        int y = grid_y + r * ROW_H - g_scroll_y;
        if (y + ROW_H <= grid_y) continue;
        if (y >= grid_y + grid_h) break;

        // Row header
        {
            int sc0, sr0, sc1, sr1;
            sel_range(&sc0, &sr0, &sc1, &sr1);
            if (r >= sr0 && r <= sr1)
                px_fill(pixels, g_win_w, g_win_h, 0, y, ROW_HEADER_W, ROW_H, SELECT_FILL);
            else
                px_fill(pixels, g_win_w, g_win_h, 0, y, ROW_HEADER_W, ROW_H, HEADER_BG);
        }

        char row_label[8];
        int rv = r + 1;
        if (rv >= 100) { row_label[0] = '0' + rv / 100; row_label[1] = '0' + (rv / 10) % 10; row_label[2] = '0' + rv % 10; row_label[3] = '\0'; }
        else if (rv >= 10) { row_label[0] = '0' + rv / 10; row_label[1] = '0' + rv % 10; row_label[2] = '\0'; }
        else { row_label[0] = '0' + rv; row_label[1] = '\0'; }

        if (g_font) {
            int tw = g_font->measure_text(row_label, HEADER_FONT);
            int lx = (ROW_HEADER_W - tw) / 2;
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h, lx, y + (ROW_H - HEADER_FONT) / 2, row_label, HEADER_TEXT, HEADER_FONT);
        }

        px_vline(pixels, g_win_w, g_win_h, ROW_HEADER_W - 1, y, ROW_H, GRID_COLOR);

        // Cells in this row
        for (int c = 0; c < MAX_COLS; c++) {
            int x = col_x(c) - g_scroll_x;
            if (x + g_col_widths[c] <= ROW_HEADER_W) continue;
            if (x >= g_win_w) break;

            Cell* cell = &g_cells[r][c];

            // Selection fill (drawn before text so text is visible)
            if (g_has_selection) {
                int sc0, sr0, sc1, sr1;
                sel_range(&sc0, &sr0, &sc1, &sr1);
                if (c >= sc0 && c <= sc1 && r >= sr0 && r <= sr1 &&
                    !(c == g_sel_col && r == g_sel_row))
                    px_fill(pixels, g_win_w, g_win_h, x + 1, y + 1, g_col_widths[c] - 2, ROW_H - 2, SELECT_FILL);
            }

            // Cell text
            if (cell->display[0] && g_font) {
                Color col = CELL_TEXT;
                if (cell->type == CT_NUMBER || cell->type == CT_FORMULA) col = NUM_COLOR;
                if (cell->type == CT_ERROR) col = ERR_COLOR;

                TrueTypeFont* cf = (cell->bold && g_font_bold) ? g_font_bold : g_font;

                CellAlign eff_align = cell->align;
                if (eff_align == ALIGN_AUTO) {
                    eff_align = (cell->type == CT_NUMBER || cell->type == CT_FORMULA) ? ALIGN_RIGHT : ALIGN_LEFT;
                }

                int tw = cf->measure_text(cell->display, FONT_SIZE);
                int text_x;
                if (eff_align == ALIGN_RIGHT)
                    text_x = x + g_col_widths[c] - tw - 6;
                else if (eff_align == ALIGN_CENTER)
                    text_x = x + (g_col_widths[c] - tw) / 2;
                else
                    text_x = x + 6;

                {
                    // Clip text to cell boundaries
                    int cell_left = x < ROW_HEADER_W ? ROW_HEADER_W : x;
                    int cell_right = x + g_col_widths[c] - 1;
                    if (cell_right > g_win_w) cell_right = g_win_w;
                    if (text_x < g_win_w && cell_left < cell_right)
                        cf->draw_to_buffer_clipped(pixels, g_win_w, g_win_h,
                            text_x, y + (ROW_H - FONT_SIZE) / 2,
                            cell->display, col, FONT_SIZE,
                            cell_left, y, cell_right - cell_left, ROW_H);
                }
            }

            // Cell right border
            px_vline(pixels, g_win_w, g_win_h, x + g_col_widths[c] - 1, y, ROW_H, GRID_COLOR);
        }

        // Row bottom border
        px_hline(pixels, g_win_w, g_win_h, ROW_HEADER_W, y + ROW_H - 1, g_win_w - ROW_HEADER_W, GRID_COLOR);
    }

    // ---- Selection border ----
    {
        int c0, r0, c1, r1;
        sel_range(&c0, &r0, &c1, &r1);

        int sx = col_x(c0) - g_scroll_x;
        int sy_sel = grid_y + r0 * ROW_H - g_scroll_y;
        int sw = 0;
        for (int c = c0; c <= c1; c++) sw += g_col_widths[c];
        int sh = (r1 - r0 + 1) * ROW_H;

        px_rect(pixels, g_win_w, g_win_h, sx, sy_sel, sw, sh, SELECT_BORDER);
        px_rect(pixels, g_win_w, g_win_h, sx + 1, sy_sel + 1, sw - 2, sh - 2, SELECT_BORDER);
    }

    // ---- Status bar ----
    int sy = g_win_h - STATUS_BAR_H;
    px_fill(pixels, g_win_w, g_win_h, 0, sy, g_win_w, STATUS_BAR_H, STATUS_BG);

    if (g_font) {
        char status[128];
        if (g_filepath[0]) {
            const char* fname = g_filepath;
            for (int i = 0; g_filepath[i]; i++)
                if (g_filepath[i] == '/') fname = g_filepath + i + 1;
            snprintf(status, 128, " %s%s", fname, g_modified ? " *" : "");
        } else {
            snprintf(status, 128, " Untitled%s", g_modified ? " *" : "");
        }
        int sty = sy + (STATUS_BAR_H - HEADER_FONT) / 2;
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h, 6, sty, status, STATUS_TEXT, HEADER_FONT);

        char right[64];
        if (g_has_selection) {
            int c0, r0, c1, r1;
            sel_range(&c0, &r0, &c1, &r1);
            char n0[8], n1[8];
            cell_name(n0, c0, r0);
            cell_name(n1, c1, r1);
            int ncells = (c1 - c0 + 1) * (r1 - r0 + 1);
            snprintf(right, 64, "%s:%s (%d cells) ", n0, n1, ncells);
        } else {
            Cell* sel = &g_cells[g_sel_row][g_sel_col];
            char cname[8];
            cell_name(cname, g_sel_col, g_sel_row);
            if (sel->type == CT_FORMULA || sel->type == CT_NUMBER) {
                char vbuf[32];
                double_to_str(vbuf, 32, sel->value);
                snprintf(right, 64, "%s = %s ", cname, vbuf);
            } else {
                snprintf(right, 64, "%s ", cname);
            }
        }
        int rw = g_font->measure_text(right, HEADER_FONT);
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h, g_win_w - rw - 6, sty, right, STATUS_TEXT, HEADER_FONT);
    }

    // ---- Format dropdown overlay ----
    if (g_fmt_dropdown_open && g_font) {
        int dx = TB_FMT_X0;
        int dy = TOOLBAR_H;
        int dw = 80;
        const char* items[] = { "Auto", "Decimal", "Currency $", "Percent %" };
        int item_count = 4;
        int item_h = 26;
        int dh = item_count * item_h + 4;

        px_fill(pixels, g_win_w, g_win_h, dx, dy, dw, dh, BG_COLOR);
        px_rect(pixels, g_win_w, g_win_h, dx, dy, dw, dh, GRID_COLOR);

        for (int i = 0; i < item_count; i++) {
            int iy = dy + 2 + i * item_h;
            if ((int)cur_cell->fmt == i)
                px_fill(pixels, g_win_w, g_win_h, dx + 2, iy, dw - 4, item_h - 2, SELECT_FILL);
            g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                dx + 8, iy + (item_h - HEADER_FONT) / 2, items[i], CELL_TEXT, HEADER_FONT);
        }
    }
}
