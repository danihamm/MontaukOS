/*
 * render.cpp
 * Rendering for the standalone MontaukOS Word Processor
 * Copyright (c) 2026 Daniel Hammer
 */

#include "wordprocessor.hpp"

static int wp_ui_line_height() {
    if (g_ui_font && g_ui_font->valid)
        return g_ui_font->get_line_height(fonts::UI_SIZE);
    return fonts::UI_SIZE;
}

static void wp_draw_ui_text(Canvas& c, int x, int y, const char* text, Color color) {
    draw_text(c, g_ui_font, x, y, text, color, fonts::UI_SIZE);
}

static void wp_draw_ui_button(Canvas& c, int x, int y, int w, int h,
                              const char* label, Color bg, Color fg, int radius = 3) {
    c.fill_rounded_rect(x, y, w, h, radius, bg);
    int tw = text_width(g_ui_font, label, fonts::UI_SIZE);
    int th = wp_ui_line_height();
    draw_text(c, g_ui_font, x + (w - tw) / 2, y + (h - th) / 2, label, fg, fonts::UI_SIZE);
}

static void wp_draw_ui_icon_button(Canvas& c, int x, int y, int w, int h,
                                   const SvgIcon& icon, const char* fallback,
                                   Color bg, Color fg, int radius = 3) {
    c.fill_rounded_rect(x, y, w, h, radius, bg);
    if (icon.pixels) {
        c.icon(x + (w - icon.width) / 2, y + (h - icon.height) / 2, icon);
        return;
    }
    wp_draw_ui_button(c, x, y, w, h, fallback, bg, fg, radius);
}

static int wp_special_char_flyout_x() {
    int dx = WP_BTN_SECTION_X + 24 - WP_SPECIAL_CHAR_FLYOUT_W;
    if (dx + WP_SPECIAL_CHAR_FLYOUT_W > g_win_w) dx = g_win_w - WP_SPECIAL_CHAR_FLYOUT_W;
    return dx < 0 ? 0 : dx;
}

static int wp_divider_flyout_x() {
    int dx = WP_BTN_DIVIDER_X + 24 - WP_DIVIDER_FLYOUT_W;
    if (dx + WP_DIVIDER_FLYOUT_W > g_win_w) dx = g_win_w - WP_DIVIDER_FLYOUT_W;
    return dx < 0 ? 0 : dx;
}

static void wp_draw_divider_sample(Canvas& c, int x, int y, int w, int h,
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

static void wp_fit_ui_text(const char* src, char* out, int out_len, int max_w) {
    if (!src || !src[0]) {
        if (out_len > 0) out[0] = '\0';
        return;
    }

    montauk::strncpy(out, src, out_len - 1);
    out[out_len - 1] = '\0';
    if (text_width(g_ui_font, out, fonts::UI_SIZE) <= max_w) return;

    int slen = montauk::slen(src);
    for (int keep = slen; keep > 1; keep--) {
        char candidate[256];
        candidate[0] = '.';
        candidate[1] = '.';
        int pos = 2;
        const char* tail = src + (slen - keep);
        while (*tail && pos < (int)sizeof(candidate) - 1)
            candidate[pos++] = *tail++;
        candidate[pos] = '\0';
        if (text_width(g_ui_font, candidate, fonts::UI_SIZE) <= max_w) {
            montauk::strncpy(out, candidate, out_len - 1);
            out[out_len - 1] = '\0';
            return;
        }
    }
}

static ParagraphStyle* wp_current_paragraph_style(WordProcessorState* wp) {
    if (wp->paragraph_count <= 0) {
        static ParagraphStyle fallback = { PARA_ALIGN_LEFT, PARA_LIST_NONE, 100, PARA_DIVIDER_NONE, 0, 0, 0, 0 };
        return &fallback;
    }
    int para = wp_find_paragraph_at(wp, wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset));
    if (para < 0) para = 0;
    if (para >= wp->paragraph_count) para = wp->paragraph_count - 1;
    return &wp->paragraphs[para];
}

static const char* wp_align_label(uint8_t align) {
    switch (align) {
    case PARA_ALIGN_CENTER: return "Center";
    case PARA_ALIGN_RIGHT: return "Right";
    default: return "Left";
    }
}

static const char* wp_list_label(uint8_t list_type) {
    switch (list_type) {
    case PARA_LIST_BULLET: return "Bullets";
    case PARA_LIST_NUMBER: return "Numbers";
    default: return "Plain";
    }
}

static const char* wp_divider_label(uint8_t divider_type) {
    switch (divider_type) {
    case PARA_DIVIDER_SINGLE: return "Single";
    case PARA_DIVIDER_DOUBLE: return "Double";
    case PARA_DIVIDER_DOTTED: return "Dotted";
    case PARA_DIVIDER_DASHED: return "Dashed";
    case PARA_DIVIDER_HEAVY: return "Heavy";
    case PARA_DIVIDER_THIN_THICK: return "Thin/Thick";
    case PARA_DIVIDER_THICK_THIN: return "Thick/Thin";
    default: return "None";
    }
}

static void wp_draw_list_marker(Canvas& c, WordProcessorState* wp, WrapLine* wl, int py) {
    if (!wl->first_in_paragraph || wl->paragraph_idx < 0 || wl->paragraph_idx >= wp->paragraph_count)
        return;

    ParagraphStyle* para = &wp->paragraphs[wl->paragraph_idx];
    if (para->divider_type != PARA_DIVIDER_NONE || para->list_type == PARA_LIST_NONE) return;

    int marker_x = WP_MARGIN + wp_scale_layout_units(para->left_indent + para->first_line_indent, WP_SCREEN_DPI);
    if (marker_x < 4) marker_x = 4;

    if (para->list_type == PARA_LIST_BULLET) {
        int bullet_x = marker_x + wp_scale_layout_units(7, WP_SCREEN_DPI);
        int bullet_r = wp_scale_layout_units(3, WP_SCREEN_DPI);
        if (bullet_r < 2) bullet_r = 2;
        fill_circle(c, bullet_x, py + wl->height / 2, bullet_r, colors::TEXT_COLOR);
        return;
    }

    char label[16];
    snprintf(label, sizeof(label), "%d.", wl->list_number > 0 ? wl->list_number : 1);
    if (wp->run_count <= 0) return;
    StyledRun* run = &wp->runs[wl->run_idx < wp->run_count ? wl->run_idx : 0];
    TrueTypeFont* font = wp_get_font(run->font_id, run->flags);
    int font_px = wp_screen_font_pixels(run->size);
    int top_y = py + (wl->height - font_px) / 2;
    draw_text(c, font ? font : g_ui_font, marker_x, top_y, label, colors::TEXT_COLOR, font_px);
}

void wp_render() {
    if (!g_win.pixels) return;

    WordProcessorState* wp = &g_wp;
    Canvas c = g_win.canvas();
    c.fill(colors::WINDOW_BG);

    int sfh = wp_ui_line_height();
    int cursor_abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    ParagraphStyle* cur_para = wp_current_paragraph_style(wp);
    Color toolbar_bg = Color::from_rgb(0xF5, 0xF5, 0xF5);
    Color btn_bg = Color::from_rgb(0xE8, 0xE8, 0xE8);
    Color btn_active = Color::from_rgb(0xC0, 0xD0, 0xE8);

    c.fill_rect(0, 0, c.w, WP_TOOLBAR_H, toolbar_bg);

    c.fill_rounded_rect(WP_BTN_OPEN_X, 6, 24, 24, 3, btn_bg);
    if (g_icon_folder.pixels)
        c.icon(WP_BTN_OPEN_X + 4, 10, g_icon_folder);

    c.fill_rounded_rect(WP_BTN_SAVE_X, 6, 24, 24, 3, btn_bg);
    if (g_icon_save.pixels)
        c.icon(WP_BTN_SAVE_X + 4, 10, g_icon_save);

    c.fill_rounded_rect(WP_BTN_PRINT_X, 6, 24, 24, 3, btn_bg);
    if (g_icon_print.pixels)
        c.icon(WP_BTN_PRINT_X + 4, 10, g_icon_print);

    wp_draw_ui_icon_button(c, WP_BTN_EXPORT_X, 6, 24, 24, g_icon_export_pdf, "P",
                           btn_bg, colors::TEXT_COLOR);

    c.vline(WP_BTN_EXPORT_X + 28, 4, 28, colors::BORDER);

    wp_draw_ui_icon_button(c, WP_BTN_UNDO_X, 6, 24, 24, g_icon_undo, "U", btn_bg, colors::TEXT_COLOR);
    wp_draw_ui_icon_button(c, WP_BTN_REDO_X, 6, 24, 24, g_icon_redo, "R", btn_bg, colors::TEXT_COLOR);

    c.vline(WP_BTN_REDO_X + 28, 4, 28, colors::BORDER);

    Color bold_bg = (wp->cur_flags & STYLE_BOLD) ? btn_active : btn_bg;
    c.fill_rounded_rect(WP_BTN_BOLD_X, 6, 24, 24, 3, bold_bg);
    draw_text(c, g_ui_bold ? g_ui_bold : g_ui_font, WP_BTN_BOLD_X + 7, 8, "B", colors::TEXT_COLOR, fonts::UI_SIZE);

    Color italic_bg = (wp->cur_flags & STYLE_ITALIC) ? btn_active : btn_bg;
    c.fill_rounded_rect(WP_BTN_ITALIC_X, 6, 24, 24, 3, italic_bg);
    {
        TrueTypeFont* italic_font = wp_get_font(FONT_ROBOTO, STYLE_ITALIC);
        draw_text(c, italic_font ? italic_font : g_ui_font, WP_BTN_ITALIC_X + 9, 8, "I", colors::TEXT_COLOR, fonts::UI_SIZE);
    }

    c.fill_rounded_rect(WP_FONT_DD_X, 6, WP_FONT_DD_W, 24, 3, btn_bg);
    wp_draw_ui_text(c, WP_FONT_DD_X + 6, (WP_TOOLBAR_H - sfh) / 2,
                    WP_FONT_NAMES[wp->cur_font_id < FONT_COUNT ? wp->cur_font_id : 0],
                    colors::TEXT_COLOR);

    c.fill_rounded_rect(WP_SIZE_DD_X, 6, WP_SIZE_DD_W, 24, 3, btn_bg);
    {
        char sz[8];
        snprintf(sz, sizeof(sz), "%d", (int)wp->cur_size);
        wp_draw_ui_text(c, WP_SIZE_DD_X + 8, (WP_TOOLBAR_H - sfh) / 2, sz, colors::TEXT_COLOR);
    }

    c.vline(WP_SIZE_DD_X + WP_SIZE_DD_W + 6, 4, 28, colors::BORDER);

    wp_draw_ui_icon_button(c, WP_BTN_ALIGN_L_X, 6, 24, 24, g_icon_align_left, "L",
                           cur_para->align == PARA_ALIGN_LEFT ? btn_active : btn_bg, colors::TEXT_COLOR);
    wp_draw_ui_icon_button(c, WP_BTN_ALIGN_C_X, 6, 24, 24, g_icon_align_center, "C",
                           cur_para->align == PARA_ALIGN_CENTER ? btn_active : btn_bg, colors::TEXT_COLOR);
    wp_draw_ui_icon_button(c, WP_BTN_ALIGN_R_X, 6, 24, 24, g_icon_align_right, "R",
                           cur_para->align == PARA_ALIGN_RIGHT ? btn_active : btn_bg, colors::TEXT_COLOR);
    c.vline(WP_BTN_ALIGN_R_X + 28, 4, 28, colors::BORDER);
    wp_draw_ui_icon_button(c, WP_BTN_BULLET_X, 6, 24, 24, g_icon_list_bullet, "*",
                           cur_para->list_type == PARA_LIST_BULLET ? btn_active : btn_bg, colors::TEXT_COLOR);
    wp_draw_ui_icon_button(c, WP_BTN_NUMBER_X, 6, 24, 24, g_icon_list_number, "1.",
                           cur_para->list_type == PARA_LIST_NUMBER ? btn_active : btn_bg, colors::TEXT_COLOR);
    c.vline(WP_BTN_NUMBER_X + 28, 4, 28, colors::BORDER);
    wp_draw_ui_icon_button(c, WP_BTN_OUTDENT_X, 6, 24, 24, g_icon_indent_less, "<", btn_bg, colors::TEXT_COLOR);
    wp_draw_ui_icon_button(c, WP_BTN_INDENT_X, 6, 24, 24, g_icon_indent_more, ">", btn_bg, colors::TEXT_COLOR);
    c.vline(WP_BTN_INDENT_X + 28, 4, 28, colors::BORDER);

    {
        char spacing[12];
        snprintf(spacing, sizeof(spacing), "%d%%", (int)cur_para->line_spacing);
        c.fill_rounded_rect(WP_LINE_DD_X, 6, WP_LINE_DD_W, 24, 3, btn_bg);
        wp_draw_ui_text(c, WP_LINE_DD_X + 8, (WP_TOOLBAR_H - sfh) / 2, spacing, colors::TEXT_COLOR);
    }

    c.vline(WP_BTN_DIVIDER_X - 4, 4, 28, colors::BORDER);

    Color divider_bg = (wp->divider_flyout_open || cur_para->divider_type != PARA_DIVIDER_NONE) ? btn_active : btn_bg;
    c.fill_rounded_rect(WP_BTN_DIVIDER_X, 6, 24, 24, 3, divider_bg);
    uint8_t divider_preview = cur_para->divider_type != PARA_DIVIDER_NONE
        ? cur_para->divider_type
        : (uint8_t)PARA_DIVIDER_SINGLE;
    wp_draw_divider_sample(c, WP_BTN_DIVIDER_X + 4, 6, 16, 24,
                           divider_preview,
                           colors::TEXT_COLOR);

    c.vline(WP_BTN_SECTION_X - 4, 4, 28, colors::BORDER);

    Color special_bg = wp->special_char_flyout_open ? btn_active : btn_bg;
    c.fill_rounded_rect(WP_BTN_SECTION_X, 6, 24, 24, 3, special_bg);
    {
        char section[2] = { (char)0xA7, '\0' };
        TrueTypeFont* font = wp_get_font(FONT_ROBOTO, 0);
        draw_text(c, font ? font : g_ui_font, WP_BTN_SECTION_X + 6, 8, section, colors::TEXT_COLOR, fonts::UI_SIZE);
    }

    c.hline(0, WP_TOOLBAR_H - 1, c.w, colors::BORDER);

    int edit_y = WP_TOOLBAR_H;
    if (wp->show_pathbar) {
        int pb_y = WP_TOOLBAR_H;
        c.fill_rect(0, pb_y, c.w, WP_PATHBAR_H, Color::from_rgb(0xF0, 0xF0, 0xF0));

        int inp_x = 8;
        int inp_y = pb_y + 4;
        int btn_w = 56;
        int inp_w = c.w - inp_x - btn_w - 12;
        int inp_h = 24;

        c.fill_rect(inp_x, inp_y, inp_w, inp_h, colors::WHITE);
        c.rect(inp_x, inp_y, inp_w, inp_h, colors::ACCENT);

        int text_y = inp_y + (inp_h - sfh) / 2;
        wp_draw_ui_text(c, inp_x + 4, text_y, wp->pathbar_text, colors::TEXT_COLOR);

        char prefix[256];
        int plen = wp->pathbar_cursor;
        if (plen > 255) plen = 255;
        for (int i = 0; i < plen; i++) prefix[i] = wp->pathbar_text[i];
        prefix[plen] = '\0';
        int cx = inp_x + 4 + text_width(g_ui_font, prefix, fonts::UI_SIZE);
        c.fill_rect(cx, inp_y + 3, 2, inp_h - 6, colors::ACCENT);

        int btn_x = inp_x + inp_w + 6;
        wp_draw_ui_button(c, btn_x, inp_y, btn_w, inp_h,
                          wp->pathbar_save_mode ? "Save" : "Open",
                          colors::ACCENT, colors::WHITE);

        c.hline(0, pb_y + WP_PATHBAR_H - 1, c.w, colors::BORDER);
        edit_y = WP_TOOLBAR_H + WP_PATHBAR_H;
    }

    int text_area_h = c.h - edit_y - WP_STATUS_H;
    wp_recompute_wrap(wp, c.w, WP_SCREEN_DPI);

    wp->scrollbar.bounds = {c.w - WP_SCROLLBAR_W, edit_y, WP_SCROLLBAR_W, text_area_h};
    wp->scrollbar.content_height = wp->content_height;
    wp->scrollbar.view_height = text_area_h;

    {
        int ms = wp->scrollbar.max_scroll();
        if (wp->scrollbar.scroll_offset > ms) wp->scrollbar.scroll_offset = ms;
        if (wp->scrollbar.scroll_offset < 0) wp->scrollbar.scroll_offset = 0;
    }

    wp_ensure_cursor_visible(wp, text_area_h);

    int scroll_y = wp->scrollbar.scroll_offset;
    int sel_s = 0;
    int sel_e = 0;
    if (wp->has_selection) wp_sel_range(wp, &sel_s, &sel_e);
    Color sel_bg = Color::from_rgb(0xB0, 0xD0, 0xF0);

    for (int li = 0; li < wp->wrap_line_count; li++) {
        WrapLine* wl = &wp->wrap_lines[li];
        int py = edit_y + wl->y - scroll_y;

        if (py + wl->height <= edit_y) continue;
        if (py >= edit_y + text_area_h) break;

        wp_draw_list_marker(c, wp, wl, py);
        if (wl->divider_type != PARA_DIVIDER_NONE)
            wp_draw_divider_sample(c, wl->x, py, wl->width, wl->height, wl->divider_type, colors::TEXT_COLOR);

        int chars_left = wl->char_count;
        int ri = wl->run_idx;
        int ro = wl->run_offset;
        int x = wl->x;
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

            GlyphCache* gc = font->get_cache(wp_screen_font_pixels(r->size));
            int baseline = py + wl->baseline;

            int avail = r->len - ro;
            int to_draw = avail < chars_left ? avail : chars_left;

            for (int ci = 0; ci < to_draw; ci++) {
                char ch = r->text[ro + ci];
                int abs_ch = line_abs_start + char_idx;

                int char_adv = 0;
                if (ch != '\n' && (ch >= 32 || ch < 0)) {
                    CachedGlyph* g = font->get_glyph(gc, (unsigned char)ch);
                    char_adv = g ? g->advance : 8;
                }

                if (wp->has_selection && abs_ch >= sel_s && abs_ch < sel_e) {
                    int sel_w = char_adv > 0 ? char_adv : 6;
                    c.fill_rect(x, py, sel_w, wl->height, sel_bg);
                }

                if (abs_ch == cursor_abs) {
                    int cur_h = wl->height;
                    if (py >= edit_y && py + cur_h <= edit_y + text_area_h)
                        c.fill_rect(x, py, 2, cur_h, colors::ACCENT);
                }

                if (ch != '\n' && (ch >= 32 || ch < 0)) {
                    if (x < c.w - WP_SCROLLBAR_W - WP_MARGIN) {
                        Color text_col = (wp->has_selection && abs_ch >= sel_s && abs_ch < sel_e)
                            ? Color::from_rgb(0x10, 0x10, 0x10) : colors::TEXT_COLOR;
                        int adv = font->draw_char_to_buffer(
                            c.pixels, c.w, c.h, x, baseline, (unsigned char)ch, text_col, gc);
                        x += adv;
                    }
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

        if (line_abs_start + char_idx == cursor_abs && li == wp->wrap_line_count - 1) {
            int cur_h = wl->height;
            if (py >= edit_y && py + cur_h <= edit_y + text_area_h)
                c.fill_rect(x, py, 2, cur_h, colors::ACCENT);
        }
    }

    if (wp->scrollbar.content_height > wp->scrollbar.view_height) {
        Color sb_fg = (wp->scrollbar.hovered || wp->scrollbar.dragging)
            ? wp->scrollbar.hover_fg : wp->scrollbar.fg;
        int sbx = wp->scrollbar.bounds.x;
        int sby = wp->scrollbar.bounds.y;
        int sbw = wp->scrollbar.bounds.w;
        int sbh = wp->scrollbar.bounds.h;
        c.fill_rect(sbx, sby, sbw, sbh, colors::SCROLLBAR_BG);
        int th = wp->scrollbar.thumb_height();
        int tty = wp->scrollbar.thumb_y();
        c.fill_rect(sbx + 1, tty, sbw - 2, th, sb_fg);
    }

    int status_y = c.h - WP_STATUS_H;
    c.fill_rect(0, status_y, c.w, WP_STATUS_H, Color::from_rgb(0x2B, 0x3E, 0x50));
    int status_text_y = status_y + (WP_STATUS_H - sfh) / 2;

    char status_left[320];
    const char* name = wp->filename[0] ? wp->filename : "Untitled";
    if (cur_para->divider_type != PARA_DIVIDER_NONE) {
        snprintf(status_left, sizeof(status_left), " %s%s | %s %dpt | Divider: %s | %d%%",
                 name, wp->modified ? " *" : "",
                 WP_FONT_NAMES[wp->cur_font_id < FONT_COUNT ? wp->cur_font_id : 0], (int)wp->cur_size,
                 wp_divider_label(cur_para->divider_type), (int)cur_para->line_spacing);
    } else {
        snprintf(status_left, sizeof(status_left), " %s%s | %s %dpt | %s | %s | %d%%",
                 name, wp->modified ? " *" : "",
                 WP_FONT_NAMES[wp->cur_font_id < FONT_COUNT ? wp->cur_font_id : 0], (int)wp->cur_size,
                 wp_align_label(cur_para->align), wp_list_label(cur_para->list_type),
                 (int)cur_para->line_spacing);
    }

    char status_right[32];
    snprintf(status_right, sizeof(status_right), "%d chars ", wp->total_text_len);
    int sr_w = text_width(g_ui_font, status_right, fonts::UI_SIZE);
    char status_combined[320];
    if (wp->status_msg[0]) snprintf(status_combined, sizeof(status_combined), "%s | %s", status_left, wp->status_msg);
    else montauk::strncpy(status_combined, status_left, (int)sizeof(status_combined) - 1);
    status_combined[sizeof(status_combined) - 1] = '\0';

    char status_fit[320];
    wp_fit_ui_text(status_combined, status_fit, sizeof(status_fit), c.w - sr_w - 12);
    wp_draw_ui_text(c, 4, status_text_y, status_fit, colors::PANEL_TEXT);
    wp_draw_ui_text(c, c.w - sr_w - 4, status_text_y, status_right, colors::PANEL_TEXT);

    if (wp->font_dropdown_open) {
        int dx = WP_FONT_DD_X;
        int dy = WP_TOOLBAR_H;
        int dw = 110;
        int dh = FONT_COUNT * 26 + 4;
        c.fill_rect(dx, dy, dw, dh, colors::MENU_BG);
        c.rect(dx, dy, dw, dh, colors::BORDER);
        for (int i = 0; i < FONT_COUNT; i++) {
            int iy = dy + 2 + i * 26;
            if (i == wp->cur_font_id)
                c.fill_rect(dx + 2, iy, dw - 4, 24, colors::MENU_HOVER);
            wp_draw_ui_text(c, dx + 8, iy + (24 - sfh) / 2, WP_FONT_NAMES[i], colors::TEXT_COLOR);
        }
    }

    if (wp->size_dropdown_open) {
        int dx = WP_SIZE_DD_X;
        int dy = WP_TOOLBAR_H;
        int dw = 56;
        int dh = WP_SIZE_OPTION_COUNT * 26 + 4;
        c.fill_rect(dx, dy, dw, dh, colors::MENU_BG);
        c.rect(dx, dy, dw, dh, colors::BORDER);
        for (int i = 0; i < WP_SIZE_OPTION_COUNT; i++) {
            int iy = dy + 2 + i * 26;
            if (WP_SIZE_OPTIONS[i] == wp->cur_size)
                c.fill_rect(dx + 2, iy, dw - 4, 24, colors::MENU_HOVER);
            char sz[8];
            snprintf(sz, sizeof(sz), "%d", WP_SIZE_OPTIONS[i]);
            wp_draw_ui_text(c, dx + 8, iy + (24 - sfh) / 2, sz, colors::TEXT_COLOR);
        }
    }

    if (wp->line_spacing_dropdown_open) {
        int dx = WP_LINE_DD_X;
        int dy = WP_TOOLBAR_H;
        int dw = 64;
        int dh = WP_LINE_SPACING_OPTION_COUNT * 26 + 4;
        c.fill_rect(dx, dy, dw, dh, colors::MENU_BG);
        c.rect(dx, dy, dw, dh, colors::BORDER);
        for (int i = 0; i < WP_LINE_SPACING_OPTION_COUNT; i++) {
            int iy = dy + 2 + i * 26;
            if (WP_LINE_SPACING_OPTIONS[i] == cur_para->line_spacing)
                c.fill_rect(dx + 2, iy, dw - 4, 24, colors::MENU_HOVER);
            char spacing[12];
            snprintf(spacing, sizeof(spacing), "%d%%", WP_LINE_SPACING_OPTIONS[i]);
            wp_draw_ui_text(c, dx + 8, iy + (24 - sfh) / 2, spacing, colors::TEXT_COLOR);
        }
    }

    if (wp->divider_flyout_open) {
        int dx = wp_divider_flyout_x();
        int dy = WP_TOOLBAR_H;
        int dw = WP_DIVIDER_FLYOUT_W;
        int dh = WP_DIVIDER_OPTION_COUNT * WP_DIVIDER_ROW_H + 4;
        c.fill_rect(dx, dy, dw, dh, colors::MENU_BG);
        c.rect(dx, dy, dw, dh, colors::BORDER);
        for (int i = 0; i < WP_DIVIDER_OPTION_COUNT; i++) {
            int iy = dy + 2 + i * WP_DIVIDER_ROW_H;
            if (WP_DIVIDER_OPTIONS[i].type == cur_para->divider_type)
                c.fill_rect(dx + 2, iy, dw - 4, WP_DIVIDER_ROW_H - 2, colors::MENU_HOVER);
            wp_draw_divider_sample(c, dx + 10, iy, 56, WP_DIVIDER_ROW_H,
                                   WP_DIVIDER_OPTIONS[i].type, colors::TEXT_COLOR);
            wp_draw_ui_text(c, dx + 78, iy + (WP_DIVIDER_ROW_H - sfh) / 2,
                            WP_DIVIDER_OPTIONS[i].label, colors::TEXT_COLOR);
        }
    }

    if (wp->special_char_flyout_open) {
        int dx = wp_special_char_flyout_x();
        int dy = WP_TOOLBAR_H;
        int dw = WP_SPECIAL_CHAR_FLYOUT_W;
        int dh = WP_SPECIAL_CHAR_OPTION_COUNT * WP_SPECIAL_CHAR_ROW_H + 4;
        TrueTypeFont* glyph_font = wp_get_font(FONT_ROBOTO, 0);
        c.fill_rect(dx, dy, dw, dh, colors::MENU_BG);
        c.rect(dx, dy, dw, dh, colors::BORDER);
        for (int i = 0; i < WP_SPECIAL_CHAR_OPTION_COUNT; i++) {
            int iy = dy + 2 + i * WP_SPECIAL_CHAR_ROW_H;
            char glyph[2] = { (char)WP_SPECIAL_CHAR_OPTIONS[i].code, '\0' };
            c.fill_rect(dx + 2, iy, dw - 4, WP_SPECIAL_CHAR_ROW_H - 2, colors::MENU_BG);
            draw_text(c, glyph_font ? glyph_font : g_ui_font,
                      dx + 8, iy + (WP_SPECIAL_CHAR_ROW_H - sfh) / 2,
                      glyph, colors::TEXT_COLOR, fonts::UI_SIZE);
            wp_draw_ui_text(c, dx + 32, iy + (WP_SPECIAL_CHAR_ROW_H - sfh) / 2,
                            WP_SPECIAL_CHAR_OPTIONS[i].label, colors::TEXT_COLOR);
        }
    }
}
