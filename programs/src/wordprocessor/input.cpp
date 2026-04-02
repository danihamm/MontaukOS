/*
 * input.cpp
 * Input handling
 * Copyright (c) 2026 Daniel Hammer
 */

#include "wordprocessor.hpp"
#include <gui/dialogs.hpp>

static void wp_set_status(WordProcessorState* wp, const char* msg) {
    if (!msg) msg = "";
    montauk::strncpy(wp->status_msg, msg, (int)sizeof(wp->status_msg) - 1);
    wp->status_msg[sizeof(wp->status_msg) - 1] = '\0';
}

static void wp_open_pathbar_for_open(WordProcessorState* wp) {
    char path[256] = {};
    char msg[160] = {};
    if (dialogs::open_file("Open Document", wp->filepath, path, sizeof(path), msg, sizeof(msg))) {
        wp_load_file(wp, path);
    } else if (msg[0]) {
        wp_start_pathbar(wp, false, wp->filepath);
    }
}

static void wp_open_print_dialog(WordProcessorState* wp) {
    char msg[160] = {};
    if (wp_print_document(wp, msg, sizeof(msg)) || msg[0])
        wp_set_status(wp, msg);
}

static void wp_make_pdf_suggested_name(WordProcessorState* wp, char* out, int out_len) {
    if (!out || out_len <= 0) return;

    const char* src = (wp && wp->filename[0]) ? wp->filename : "document";
    int last_dot = -1;
    for (int i = 0; src[i]; i++) {
        if (src[i] == '.') last_dot = i;
    }

    int copy_len = (last_dot > 0) ? last_dot : montauk::slen(src);
    if (copy_len > out_len - 5) copy_len = out_len - 5;
    if (copy_len < 0) copy_len = 0;
    montauk::memcpy(out, src, (uint64_t)copy_len);
    out[copy_len] = '\0';
    montauk::strncpy(out + copy_len, ".pdf", out_len - copy_len - 1);
    out[out_len - 1] = '\0';
}

static void wp_open_export_dialog(WordProcessorState* wp) {
    char path[256] = {};
    char suggested_name[128] = {};
    char msg[160] = {};

    wp_make_pdf_suggested_name(wp, suggested_name, sizeof(suggested_name));
    if (dialogs::save_file("Export PDF",
                           wp->filepath,
                           suggested_name,
                           path, sizeof(path),
                           msg, sizeof(msg))) {
        if (wp_export_pdf_document(wp, path, msg, sizeof(msg)) || msg[0])
            wp_set_status(wp, msg);
    } else if (msg[0]) {
        wp_set_status(wp, msg);
    }
}

static void wp_commit_pathbar(WordProcessorState* wp) {
    if (!wp->pathbar_text[0]) return;
    if (wp->pathbar_save_mode) {
        wp_set_filepath(wp, wp->pathbar_text);
        wp_save_file(wp);
    } else {
        wp_load_file(wp, wp->pathbar_text);
    }
    wp->show_pathbar = false;
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

static void wp_close_dropdowns(WordProcessorState* wp) {
    wp->font_dropdown_open = false;
    wp->size_dropdown_open = false;
    wp->line_spacing_dropdown_open = false;
    wp->divider_flyout_open = false;
    wp->special_char_flyout_open = false;
}

static void wp_insert_special_char(WordProcessorState* wp, uint8_t ch) {
    if (wp->has_selection) wp_delete_selection(wp);
    wp_insert_char(wp, (char)ch);
    wp_history_checkpoint(wp);
}

static void wp_insert_divider_choice(WordProcessorState* wp, uint8_t divider_type) {
    wp_insert_divider(wp, divider_type);
    wp_history_checkpoint(wp);
}

static int wp_hit_test_text(WordProcessorState* wp, int local_x, int local_y, int edit_y) {
    int click_y = local_y - edit_y + wp->scrollbar.scroll_offset;

    int target_line = wp->wrap_line_count - 1;
    for (int i = 0; i < wp->wrap_line_count; i++) {
        if (click_y >= wp->wrap_lines[i].y &&
            click_y < wp->wrap_lines[i].y + wp->wrap_lines[i].height) {
            target_line = i;
            break;
        }
    }

    WrapLine* wl = &wp->wrap_lines[target_line];
    if (wl->divider_type != PARA_DIVIDER_NONE)
        return wp_wrap_line_start(wp, target_line);

    int click_x = local_x - wl->x;
    int ri = wl->run_idx;
    int ro = wl->run_offset;
    int chars_left = wl->char_count;
    int x = 0;
    int best_abs = wp_wrap_line_start(wp, target_line);

    if (click_x <= 0) return best_abs;

    while (chars_left > 0 && ri < wp->run_count) {
        StyledRun* r = &wp->runs[ri];
        TrueTypeFont* font = wp_get_font(r->font_id, r->flags);
        if (!font || !font->valid) {
            ri++;
            ro = 0;
            continue;
        }

        GlyphCache* gc = font->get_cache(wp_screen_font_pixels(r->size));
        int avail = r->len - ro;
        int to_check = avail < chars_left ? avail : chars_left;

        for (int ci = 0; ci < to_check; ci++) {
            char ch = r->text[ro + ci];
            CachedGlyph* g = (ch >= 32 || ch < 0) && ch != '\n'
                ? font->get_glyph(gc, (unsigned char)ch) : nullptr;
            int char_w = g ? g->advance : 0;

            if (x + char_w / 2 > click_x) return best_abs;
            x += char_w;
            best_abs++;
        }

        chars_left -= to_check;
        ro += to_check;
        if (ro >= r->len) {
            ri++;
            ro = 0;
        }
    }

    return best_abs;
}

void wp_handle_mouse(const Montauk::WinEvent& ev) {
    WordProcessorState* wp = &g_wp;
    int local_x = ev.mouse.x;
    int local_y = ev.mouse.y;
    int edit_y = WP_TOOLBAR_H + (wp->show_pathbar ? WP_PATHBAR_H : 0);
    int text_area_h = g_win_h - edit_y - WP_STATUS_H;

    wp->scrollbar.handle_mouse(local_x, local_y, ev.mouse.buttons, ev.mouse.prev_buttons, ev.mouse.scroll);

    if (wp->font_dropdown_open && wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons)) {
        int dx = WP_FONT_DD_X;
        int dy = WP_TOOLBAR_H;
        int dh = FONT_COUNT * 26 + 4;
        if (local_x >= dx && local_x < dx + 110 && local_y >= dy && local_y < dy + dh) {
            int idx = (local_y - dy - 2) / 26;
            if (idx >= 0 && idx < FONT_COUNT) {
                if (wp->has_selection) wp_apply_style_to_selection(wp, 0, idx);
                wp->cur_font_id = (uint8_t)idx;
                if (wp->has_selection) wp_history_checkpoint(wp);
            }
        }
        wp->font_dropdown_open = false;
        return;
    }

    if (wp->size_dropdown_open && wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons)) {
        int dx = WP_SIZE_DD_X;
        int dy = WP_TOOLBAR_H;
        int dh = WP_SIZE_OPTION_COUNT * 26 + 4;
        if (local_x >= dx && local_x < dx + 56 && local_y >= dy && local_y < dy + dh) {
            int idx = (local_y - dy - 2) / 26;
            if (idx >= 0 && idx < WP_SIZE_OPTION_COUNT) {
                if (wp->has_selection) wp_apply_style_to_selection(wp, 1, WP_SIZE_OPTIONS[idx]);
                wp->cur_size = (uint8_t)WP_SIZE_OPTIONS[idx];
                wp->wrap_dirty = true;
                if (wp->has_selection) wp_history_checkpoint(wp);
            }
        }
        wp->size_dropdown_open = false;
        return;
    }

    if (wp->line_spacing_dropdown_open && wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons)) {
        int dx = WP_LINE_DD_X;
        int dy = WP_TOOLBAR_H;
        int dh = WP_LINE_SPACING_OPTION_COUNT * 26 + 4;
        if (local_x >= dx && local_x < dx + 64 && local_y >= dy && local_y < dy + dh) {
            int idx = (local_y - dy - 2) / 26;
            if (idx >= 0 && idx < WP_LINE_SPACING_OPTION_COUNT) {
                wp_set_line_spacing(wp, WP_LINE_SPACING_OPTIONS[idx]);
                wp_history_checkpoint(wp);
            }
        }
        wp->line_spacing_dropdown_open = false;
        return;
    }

    if (wp->divider_flyout_open && wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons)) {
        int dx = wp_divider_flyout_x();
        int dy = WP_TOOLBAR_H;
        int dh = WP_DIVIDER_OPTION_COUNT * WP_DIVIDER_ROW_H + 4;
        if (local_x >= dx && local_x < dx + WP_DIVIDER_FLYOUT_W && local_y >= dy && local_y < dy + dh) {
            int idx = (local_y - dy - 2) / WP_DIVIDER_ROW_H;
            if (idx >= 0 && idx < WP_DIVIDER_OPTION_COUNT)
                wp_insert_divider_choice(wp, WP_DIVIDER_OPTIONS[idx].type);
        }
        wp->divider_flyout_open = false;
        return;
    }

    if (wp->special_char_flyout_open && wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons)) {
        int dx = wp_special_char_flyout_x();
        int dy = WP_TOOLBAR_H;
        int dh = WP_SPECIAL_CHAR_OPTION_COUNT * WP_SPECIAL_CHAR_ROW_H + 4;
        if (local_x >= dx && local_x < dx + WP_SPECIAL_CHAR_FLYOUT_W && local_y >= dy && local_y < dy + dh) {
            int idx = (local_y - dy - 2) / WP_SPECIAL_CHAR_ROW_H;
            if (idx >= 0 && idx < WP_SPECIAL_CHAR_OPTION_COUNT)
                wp_insert_special_char(wp, WP_SPECIAL_CHAR_OPTIONS[idx].code);
        }
        wp->special_char_flyout_open = false;
        return;
    }

    if (wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons) && local_y < WP_TOOLBAR_H) {
        if (local_x >= WP_BTN_OPEN_X && local_x < WP_BTN_OPEN_X + 24 && local_y >= 6 && local_y < 30) {
            wp_open_pathbar_for_open(wp);
            return;
        }
        if (local_x >= WP_BTN_SAVE_X && local_x < WP_BTN_SAVE_X + 24 && local_y >= 6 && local_y < 30) {
            wp_save_file(wp);
            return;
        }
        if (local_x >= WP_BTN_PRINT_X && local_x < WP_BTN_PRINT_X + 24 && local_y >= 6 && local_y < 30) {
            wp_open_print_dialog(wp);
            return;
        }
        if (local_x >= WP_BTN_EXPORT_X && local_x < WP_BTN_EXPORT_X + 24 && local_y >= 6 && local_y < 30) {
            wp_open_export_dialog(wp);
            return;
        }
        if (local_x >= WP_BTN_UNDO_X && local_x < WP_BTN_UNDO_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_undo(wp);
            return;
        }
        if (local_x >= WP_BTN_REDO_X && local_x < WP_BTN_REDO_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_redo(wp);
            return;
        }
        if (local_x >= WP_BTN_BOLD_X && local_x < WP_BTN_BOLD_X + 24 && local_y >= 6 && local_y < 30) {
            if (wp->has_selection) wp_apply_style_to_selection(wp, 2, 0);
            wp->cur_flags ^= STYLE_BOLD;
            if (wp->has_selection) wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_ITALIC_X && local_x < WP_BTN_ITALIC_X + 24 && local_y >= 6 && local_y < 30) {
            if (wp->has_selection) wp_apply_style_to_selection(wp, 3, 0);
            wp->cur_flags ^= STYLE_ITALIC;
            if (wp->has_selection) wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_FONT_DD_X && local_x < WP_FONT_DD_X + WP_FONT_DD_W && local_y >= 6 && local_y < 30) {
            bool open = !wp->font_dropdown_open;
            wp_close_dropdowns(wp);
            wp->font_dropdown_open = open;
            return;
        }
        if (local_x >= WP_SIZE_DD_X && local_x < WP_SIZE_DD_X + WP_SIZE_DD_W && local_y >= 6 && local_y < 30) {
            bool open = !wp->size_dropdown_open;
            wp_close_dropdowns(wp);
            wp->size_dropdown_open = open;
            return;
        }
        if (local_x >= WP_BTN_ALIGN_L_X && local_x < WP_BTN_ALIGN_L_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_apply_alignment(wp, PARA_ALIGN_LEFT);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_ALIGN_C_X && local_x < WP_BTN_ALIGN_C_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_apply_alignment(wp, PARA_ALIGN_CENTER);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_ALIGN_R_X && local_x < WP_BTN_ALIGN_R_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_apply_alignment(wp, PARA_ALIGN_RIGHT);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_BULLET_X && local_x < WP_BTN_BULLET_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_toggle_list(wp, PARA_LIST_BULLET);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_NUMBER_X && local_x < WP_BTN_NUMBER_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_toggle_list(wp, PARA_LIST_NUMBER);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_OUTDENT_X && local_x < WP_BTN_OUTDENT_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_adjust_paragraph_indent(wp, -WP_PARA_STEP);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_BTN_INDENT_X && local_x < WP_BTN_INDENT_X + 24 && local_y >= 6 && local_y < 30) {
            wp_close_dropdowns(wp);
            wp_adjust_paragraph_indent(wp, WP_PARA_STEP);
            wp_history_checkpoint(wp);
            return;
        }
        if (local_x >= WP_LINE_DD_X && local_x < WP_LINE_DD_X + WP_LINE_DD_W && local_y >= 6 && local_y < 30) {
            bool open = !wp->line_spacing_dropdown_open;
            wp_close_dropdowns(wp);
            wp->line_spacing_dropdown_open = open;
            return;
        }
        if (local_x >= WP_BTN_DIVIDER_X && local_x < WP_BTN_DIVIDER_X + 24 && local_y >= 6 && local_y < 30) {
            bool open = !wp->divider_flyout_open;
            wp_close_dropdowns(wp);
            wp->divider_flyout_open = open;
            return;
        }
        if (local_x >= WP_BTN_SECTION_X && local_x < WP_BTN_SECTION_X + 24 && local_y >= 6 && local_y < 30) {
            bool open = !wp->special_char_flyout_open;
            wp_close_dropdowns(wp);
            wp->special_char_flyout_open = open;
            return;
        }
        return;
    }

    if (wp->show_pathbar && local_y >= WP_TOOLBAR_H && local_y < WP_TOOLBAR_H + WP_PATHBAR_H) {
        if (wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons)) {
            int btn_w = 56;
            int inp_w = g_win_w - 8 - btn_w - 12;
            int btn_x = 8 + inp_w + 6;
            if (local_x >= btn_x && local_x < btn_x + btn_w) {
                wp_commit_pathbar(wp);
            }
        }
        return;
    }

    wp_recompute_wrap(wp, g_win_w, WP_SCREEN_DPI);

    if (wp_left_pressed(ev.mouse.buttons, ev.mouse.prev_buttons) &&
        local_y >= edit_y && local_y < edit_y + text_area_h) {
        wp_close_dropdowns(wp);

        int abs = wp_hit_test_text(wp, local_x, local_y, edit_y);
        wp_pos_to_run(wp, abs, &wp->cursor_run, &wp->cursor_offset);
        wp->sel_anchor = abs;
        wp->sel_end = abs;
        wp->has_selection = false;
        wp->mouse_selecting = true;
        return;
    }

    if (wp->mouse_selecting && wp_left_held(ev.mouse.buttons) && local_y >= edit_y - 20) {
        int abs = wp_hit_test_text(wp, local_x, local_y, edit_y);
        wp->sel_end = abs;
        wp_pos_to_run(wp, abs, &wp->cursor_run, &wp->cursor_offset);
        wp->has_selection = (wp->sel_anchor != wp->sel_end);
        return;
    }

    if (wp->mouse_selecting && wp_left_released(ev.mouse.buttons, ev.mouse.prev_buttons)) {
        wp->mouse_selecting = false;
        return;
    }

    if (ev.mouse.scroll != 0 && local_y >= edit_y && local_y < edit_y + text_area_h) {
        wp->scrollbar.scroll_offset -= ev.mouse.scroll * 40;
        int ms = wp->scrollbar.max_scroll();
        if (wp->scrollbar.scroll_offset < 0) wp->scrollbar.scroll_offset = 0;
        if (wp->scrollbar.scroll_offset > ms) wp->scrollbar.scroll_offset = ms;
    }
}

void wp_handle_key(const Montauk::KeyEvent& key) {
    WordProcessorState* wp = &g_wp;
    if (!key.pressed) return;

    if (wp->show_pathbar) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            wp_commit_pathbar(wp);
            return;
        }
        if (key.scancode == 0x01) {
            wp->show_pathbar = false;
            return;
        }
        if (key.ascii == '\b' || key.scancode == 0x0E) {
            if (wp->pathbar_cursor > 0) {
                for (int i = wp->pathbar_cursor - 1; i < wp->pathbar_len - 1; i++)
                    wp->pathbar_text[i] = wp->pathbar_text[i + 1];
                wp->pathbar_len--;
                wp->pathbar_cursor--;
                wp->pathbar_text[wp->pathbar_len] = '\0';
            }
            return;
        }
        if (key.scancode == 0x4B) {
            if (wp->pathbar_cursor > 0) wp->pathbar_cursor--;
            return;
        }
        if (key.scancode == 0x4D) {
            if (wp->pathbar_cursor < wp->pathbar_len) wp->pathbar_cursor++;
            return;
        }
        if (key.ascii >= 32 && key.ascii < 127 && wp->pathbar_len < 254) {
            for (int i = wp->pathbar_len; i > wp->pathbar_cursor; i--)
                wp->pathbar_text[i] = wp->pathbar_text[i - 1];
            wp->pathbar_text[wp->pathbar_cursor] = key.ascii;
            wp->pathbar_cursor++;
            wp->pathbar_len++;
            wp->pathbar_text[wp->pathbar_len] = '\0';
        }
        return;
    }

    wp_close_dropdowns(wp);
    wp_recompute_wrap(wp, g_win_w, WP_SCREEN_DPI);

    if (key.ctrl && (key.ascii == 's' || key.ascii == 'S')) {
        wp_save_file(wp);
        return;
    }
    if (key.ctrl && (key.ascii == 'o' || key.ascii == 'O')) {
        wp_open_pathbar_for_open(wp);
        return;
    }
    if (key.ctrl && key.alt && (key.ascii == 'p' || key.ascii == 'P')) {
        wp_open_export_dialog(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == 'p' || key.ascii == 'P')) {
        wp_open_print_dialog(wp);
        return;
    }

    if (key.ctrl && !key.alt &&
        (key.ascii == 'z' || key.ascii == 'Z')) {
        if (key.shift) wp_redo(wp);
        else wp_undo(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == 'y' || key.ascii == 'Y')) {
        wp_redo(wp);
        return;
    }

    if (key.ctrl && key.alt && (key.ascii == 'b' || key.ascii == 'B')) {
        wp_toggle_list(wp, PARA_LIST_BULLET);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && key.alt && (key.ascii == 'n' || key.ascii == 'N')) {
        wp_toggle_list(wp, PARA_LIST_NUMBER);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ctrl && !key.alt && (key.ascii == 'l' || key.ascii == 'L')) {
        wp_apply_alignment(wp, PARA_ALIGN_LEFT);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == 'e' || key.ascii == 'E')) {
        wp_apply_alignment(wp, PARA_ALIGN_CENTER);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == 'r' || key.ascii == 'R')) {
        wp_apply_alignment(wp, PARA_ALIGN_RIGHT);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ctrl && !key.alt && (key.ascii == '1')) {
        wp_set_line_spacing(wp, 100);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == '2')) {
        wp_set_line_spacing(wp, 125);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == '3')) {
        wp_set_line_spacing(wp, 150);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == '4')) {
        wp_set_line_spacing(wp, 200);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ctrl && !key.alt && (key.ascii == '[' || key.ascii == '{')) {
        if (key.shift || key.ascii == '{')
            wp_adjust_paragraph_first_line_indent(wp, -WP_PARA_STEP);
        else
            wp_adjust_paragraph_indent(wp, -WP_PARA_STEP);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == ']' || key.ascii == '}')) {
        if (key.shift || key.ascii == '}')
            wp_adjust_paragraph_first_line_indent(wp, WP_PARA_STEP);
        else
            wp_adjust_paragraph_indent(wp, WP_PARA_STEP);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ctrl && key.alt && key.scancode == 0x48) {
        wp_adjust_paragraph_spacing_before(wp, -WP_SPACE_STEP);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && key.alt && key.scancode == 0x50) {
        wp_adjust_paragraph_spacing_before(wp, WP_SPACE_STEP);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && key.shift && key.scancode == 0x48) {
        wp_adjust_paragraph_spacing_after(wp, -WP_SPACE_STEP);
        wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && key.shift && key.scancode == 0x50) {
        wp_adjust_paragraph_spacing_after(wp, WP_SPACE_STEP);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ctrl && !key.alt && (key.ascii == 'b' || key.ascii == 'B')) {
        if (wp->has_selection) wp_apply_style_to_selection(wp, 2, 0);
        wp->cur_flags ^= STYLE_BOLD;
        if (wp->has_selection) wp_history_checkpoint(wp);
        return;
    }
    if (key.ctrl && !key.alt && (key.ascii == 'i' || key.ascii == 'I')) {
        if (wp->has_selection) wp_apply_style_to_selection(wp, 3, 0);
        wp->cur_flags ^= STYLE_ITALIC;
        if (wp->has_selection) wp_history_checkpoint(wp);
        return;
    }

    if (key.scancode == 0x48) {
        if (key.shift) wp_start_selection(wp);
        else wp_clear_selection(wp);
        wp_cursor_up(wp);
        if (key.shift) wp_update_selection_to_cursor(wp);
        return;
    }
    if (key.scancode == 0x50) {
        if (key.shift) wp_start_selection(wp);
        else wp_clear_selection(wp);
        wp_cursor_down(wp);
        if (key.shift) wp_update_selection_to_cursor(wp);
        return;
    }
    if (key.scancode == 0x4B) {
        if (key.shift) wp_start_selection(wp);
        else if (wp->has_selection) {
            int s, e;
            wp_sel_range(wp, &s, &e);
            wp_pos_to_run(wp, s, &wp->cursor_run, &wp->cursor_offset);
            wp_clear_selection(wp);
            return;
        }
        wp_cursor_left(wp);
        if (key.shift) wp_update_selection_to_cursor(wp);
        return;
    }
    if (key.scancode == 0x4D) {
        if (key.shift) wp_start_selection(wp);
        else if (wp->has_selection) {
            int s, e;
            wp_sel_range(wp, &s, &e);
            wp_pos_to_run(wp, e, &wp->cursor_run, &wp->cursor_offset);
            wp_clear_selection(wp);
            return;
        }
        wp_cursor_right(wp);
        if (key.shift) wp_update_selection_to_cursor(wp);
        return;
    }

    if (key.scancode == 0x47) {
        if (key.shift) wp_start_selection(wp);
        else wp_clear_selection(wp);
        int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
        int line = wp_find_wrap_line(wp, abs);
        int start = wp_wrap_line_start(wp, line);
        wp_pos_to_run(wp, start, &wp->cursor_run, &wp->cursor_offset);
        if (key.shift) wp_update_selection_to_cursor(wp);
        return;
    }
    if (key.scancode == 0x4F) {
        if (key.shift) wp_start_selection(wp);
        else wp_clear_selection(wp);
        int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
        int line = wp_find_wrap_line(wp, abs);
        int start = wp_wrap_line_start(wp, line);
        int end = start + wp->wrap_lines[line].char_count;
        if (end > start) {
            char ch = wp_char_at(wp, end - 1);
            if (ch == '\n') end--;
        }
        wp_pos_to_run(wp, end, &wp->cursor_run, &wp->cursor_offset);
        if (key.shift) wp_update_selection_to_cursor(wp);
        return;
    }

    if (key.ctrl && (key.ascii == 'a' || key.ascii == 'A')) {
        wp->sel_anchor = 0;
        wp->sel_end = wp->total_text_len;
        wp->has_selection = (wp->total_text_len > 0);
        wp_pos_to_run(wp, wp->total_text_len, &wp->cursor_run, &wp->cursor_offset);
        return;
    }

    if (key.scancode == 0x53) {
        if (wp->has_selection) wp_delete_selection(wp);
        else wp_delete_char(wp);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        if (wp->has_selection) wp_delete_selection(wp);
        else wp_backspace(wp);
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ascii == '\n' || key.ascii == '\r') {
        if (wp->has_selection) wp_delete_selection(wp);
        wp_insert_char(wp, '\n');
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ascii == '\t') {
        if (wp->has_selection) wp_delete_selection(wp);
        for (int i = 0; i < 4; i++) wp_insert_char(wp, ' ');
        wp_history_checkpoint(wp);
        return;
    }

    if (key.ascii >= 32 && key.ascii < 127) {
        if (wp->has_selection) wp_delete_selection(wp);
        wp_insert_char(wp, key.ascii);
        wp_history_checkpoint(wp);
    }
}
