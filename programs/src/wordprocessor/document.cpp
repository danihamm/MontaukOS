/*
 * document.cpp
 * Document model, wrap layout, font loading, and file I/O
 * Copyright (c) 2026 Daniel Hammer
 */

#include "wordprocessor.hpp"
#include <gui/dialogs.hpp>

static inline int wp_min_int(int a, int b) {
    return a < b ? a : b;
}

static void wp_init_run(StyledRun* r, uint8_t font_id, uint8_t size, uint8_t flags) {
    r->cap = 64;
    r->text = (char*)montauk::malloc(r->cap);
    r->text[0] = '\0';
    r->len = 0;
    r->font_id = font_id;
    r->size = size;
    r->flags = flags;
}

static void wp_free_run(StyledRun* r) {
    if (r->text) montauk::mfree(r->text);
    r->text = nullptr;
    r->len = 0;
    r->cap = 0;
}

static void wp_ensure_run_cap(StyledRun* r, int needed) {
    if (r->len + needed <= r->cap) return;
    int new_cap = r->cap * 2;
    if (new_cap < r->len + needed) new_cap = r->len + needed;
    r->text = (char*)montauk::realloc(r->text, new_cap);
    r->cap = new_cap;
}

static bool wp_same_style(const StyledRun* a, uint8_t font_id, uint8_t size, uint8_t flags) {
    return a->font_id == font_id && a->size == size && a->flags == flags;
}

void wp_init_paragraph_style(ParagraphStyle* para) {
    para->align = PARA_ALIGN_LEFT;
    para->list_type = PARA_LIST_NONE;
    para->line_spacing = 100;
    para->_pad = 0;
    para->left_indent = 0;
    para->first_line_indent = 0;
    para->space_before = 0;
    para->space_after = 0;
}

static void wp_font_at(WordProcessorState* wp, int abs_pos,
                       TrueTypeFont** out_font, GlyphCache** out_gc) {
    if (wp->run_count <= 0) {
        *out_font = wp_get_font(wp->cur_font_id, wp->cur_flags);
        *out_gc = (*out_font && (*out_font)->valid) ? (*out_font)->get_cache(wp->cur_size) : nullptr;
        return;
    }

    int r, o;
    wp_pos_to_run(wp, abs_pos, &r, &o);
    if (r >= wp->run_count) r = wp->run_count - 1;
    if (r < 0) r = 0;
    StyledRun* run = &wp->runs[r];
    *out_font = wp_get_font(run->font_id, run->flags);
    *out_gc = (*out_font && (*out_font)->valid) ? (*out_font)->get_cache(run->size) : nullptr;
}

static void wp_free_runs(WordProcessorState* wp) {
    for (int i = 0; i < wp->run_count; i++)
        wp_free_run(&wp->runs[i]);
    wp->run_count = 0;
    wp->total_text_len = 0;
}

static void wp_free_snapshot(UndoSnapshot* snap) {
    if (snap->data) montauk::mfree(snap->data);
    montauk::memset(snap, 0, sizeof(*snap));
}

static void wp_clear_history(WordProcessorState* wp) {
    for (int i = 0; i < wp->undo_count; i++)
        wp_free_snapshot(&wp->undo[i]);
    wp->undo_count = 0;
    wp->undo_pos = 0;
}

static void wp_insert_paragraph_style(WordProcessorState* wp, int at, const ParagraphStyle* style) {
    if (wp->paragraph_count >= WP_MAX_PARAGRAPHS) return;
    if (at < 0) at = 0;
    if (at > wp->paragraph_count) at = wp->paragraph_count;
    for (int i = wp->paragraph_count; i > at; i--)
        wp->paragraphs[i] = wp->paragraphs[i - 1];
    wp->paragraphs[at] = *style;
    wp->paragraph_count++;
}

static void wp_remove_paragraph_style(WordProcessorState* wp, int at) {
    if (wp->paragraph_count <= 1) {
        wp->paragraph_count = 1;
        wp_init_paragraph_style(&wp->paragraphs[0]);
        return;
    }
    if (at < 0 || at >= wp->paragraph_count) return;
    for (int i = at; i < wp->paragraph_count - 1; i++)
        wp->paragraphs[i] = wp->paragraphs[i + 1];
    wp->paragraph_count--;
}

static int wp_count_paragraphs_in_text(WordProcessorState* wp) {
    int count = 1;
    for (int i = 0; i < wp->total_text_len; i++) {
        if (wp_char_at(wp, i) == '\n') {
            count++;
            if (count > WP_MAX_PARAGRAPHS) return WP_MAX_PARAGRAPHS + 1;
        }
    }
    return count;
}

static void wp_sync_paragraph_count(WordProcessorState* wp) {
    int needed = wp_count_paragraphs_in_text(wp);
    if (needed <= 0) needed = 1;
    if (needed > WP_MAX_PARAGRAPHS) needed = WP_MAX_PARAGRAPHS;

    if (wp->paragraph_count <= 0) {
        wp->paragraph_count = 1;
        wp_init_paragraph_style(&wp->paragraphs[0]);
    }

    if (wp->paragraph_count < needed) {
        ParagraphStyle style = wp->paragraphs[wp->paragraph_count - 1];
        while (wp->paragraph_count < needed)
            wp_insert_paragraph_style(wp, wp->paragraph_count, &style);
    } else if (wp->paragraph_count > needed) {
        wp->paragraph_count = needed;
    }
}

static void wp_on_insert_newline(WordProcessorState* wp, int abs_pos) {
    if (wp->paragraph_count >= WP_MAX_PARAGRAPHS) return;
    int para = wp_find_paragraph_at(wp, abs_pos);
    if (para < 0) para = 0;
    if (para >= wp->paragraph_count) para = wp->paragraph_count - 1;
    ParagraphStyle style = wp->paragraphs[para];
    wp_insert_paragraph_style(wp, para + 1, &style);
}

static void wp_on_delete_newline(WordProcessorState* wp, int abs_pos) {
    int para = wp_find_paragraph_at(wp, abs_pos);
    if (para + 1 < wp->paragraph_count)
        wp_remove_paragraph_style(wp, para + 1);
}

static void wp_default_line_metrics(WordProcessorState* wp, int abs_pos, int* out_height, int* out_ascent) {
    TrueTypeFont* font = nullptr;
    GlyphCache* gc = nullptr;
    if (wp->total_text_len > 0) {
        int pos = abs_pos;
        if (pos >= wp->total_text_len) pos = wp->total_text_len - 1;
        if (pos < 0) pos = 0;
        wp_font_at(wp, pos, &font, &gc);
    } else {
        font = wp_get_font(wp->cur_font_id, wp->cur_flags);
        gc = (font && font->valid) ? font->get_cache(wp->cur_size) : nullptr;
    }

    if (gc) {
        *out_height = gc->line_height;
        *out_ascent = gc->ascent;
    } else {
        *out_height = WP_DEFAULT_SIZE;
        *out_ascent = WP_DEFAULT_SIZE;
    }
}

static int wp_char_advance_at(WordProcessorState* wp, int abs_pos, char ch) {
    if (ch == '\n') return 0;
    if (!(ch >= 32 || ch < 0)) return 0;

    TrueTypeFont* font = nullptr;
    GlyphCache* gc = nullptr;
    wp_font_at(wp, abs_pos, &font, &gc);
    if (!font || !gc) return 8;

    CachedGlyph* g = font->get_glyph(gc, (unsigned char)ch);
    return g ? g->advance : 8;
}

static void wp_measure_line_range(WordProcessorState* wp, int start, int count,
                                  int* out_width, int* out_height, int* out_ascent) {
    int width = 0;
    int height = 0;
    int ascent = 0;

    for (int i = 0; i < count; i++) {
        char ch = wp_char_at(wp, start + i);
        if (ch != '\n')
            width += wp_char_advance_at(wp, start + i, ch);

        TrueTypeFont* font = nullptr;
        GlyphCache* gc = nullptr;
        wp_font_at(wp, start + i, &font, &gc);
        if (gc) {
            if (gc->line_height > height) height = gc->line_height;
            if (gc->ascent > ascent) ascent = gc->ascent;
        }
    }

    if (height == 0)
        wp_default_line_metrics(wp, start, &height, &ascent);

    *out_width = width;
    *out_height = height;
    *out_ascent = ascent;
}

static int wp_line_spacing_advance(int height, int spacing_percent) {
    int advance = (height * spacing_percent + 99) / 100;
    return advance < height ? height : advance;
}

static int wp_effective_text_indent(const ParagraphStyle* para, bool first_line) {
    int indent = para->left_indent;
    if (indent < 0) indent = 0;
    if (first_line && para->list_type == PARA_LIST_NONE)
        indent += para->first_line_indent;
    if (indent < 0) indent = 0;
    return indent;
}

static bool wp_ensure_wrap_capacity(WordProcessorState* wp, int needed) {
    if (needed <= wp->wrap_line_cap && wp->wrap_lines)
        return true;

    int new_cap = wp->wrap_line_cap > 0 ? wp->wrap_line_cap : 128;
    while (new_cap < needed && new_cap < WP_MAX_WRAP_LINES)
        new_cap *= 2;
    if (new_cap > WP_MAX_WRAP_LINES) new_cap = WP_MAX_WRAP_LINES;
    if (new_cap < needed) return false;

    void* mem = nullptr;
    if (wp->wrap_lines)
        mem = montauk::realloc(wp->wrap_lines, new_cap * (int)sizeof(WrapLine));
    else
        mem = montauk::malloc(new_cap * (int)sizeof(WrapLine));
    if (!mem) return false;

    wp->wrap_lines = (WrapLine*)mem;
    wp->wrap_line_cap = new_cap;
    return true;
}

static void wp_merge_adjacent(WordProcessorState* wp) {
    int i = 0;
    while (i < wp->run_count - 1) {
        StyledRun* a = &wp->runs[i];
        StyledRun* b = &wp->runs[i + 1];
        if (a->font_id == b->font_id && a->size == b->size && a->flags == b->flags) {
            int old_a_len = a->len;
            wp_ensure_run_cap(a, b->len);
            montauk::memcpy(a->text + a->len, b->text, b->len);
            a->len += b->len;

            if (wp->cursor_run == i + 1) {
                wp->cursor_run = i;
                wp->cursor_offset = old_a_len + wp->cursor_offset;
            } else if (wp->cursor_run > i + 1) {
                wp->cursor_run--;
            }

            wp_free_run(b);
            for (int j = i + 1; j < wp->run_count - 1; j++)
                wp->runs[j] = wp->runs[j + 1];
            wp->run_count--;
        } else {
            i++;
        }
    }

    i = 0;
    while (i < wp->run_count && wp->run_count > 1) {
        if (wp->runs[i].len == 0) {
            if (wp->cursor_run == i) {
                if (i < wp->run_count - 1) {
                    wp->cursor_run = i;
                    wp->cursor_offset = 0;
                } else {
                    wp->cursor_run = i - 1;
                    wp->cursor_offset = wp->runs[i - 1].len;
                }
            } else if (wp->cursor_run > i) {
                wp->cursor_run--;
            }
            wp_free_run(&wp->runs[i]);
            for (int j = i; j < wp->run_count - 1; j++)
                wp->runs[j] = wp->runs[j + 1];
            wp->run_count--;
        } else {
            i++;
        }
    }
}

static int wp_split_at(WordProcessorState* wp, int abs_pos) {
    int r, o;
    wp_pos_to_run(wp, abs_pos, &r, &o);
    if (o == 0) return r;
    if (o >= wp->runs[r].len) return r + 1;
    if (wp->run_count >= WP_MAX_RUNS) return r;

    StyledRun* src = &wp->runs[r];
    int after_len = src->len - o;

    StyledRun after;
    wp_init_run(&after, src->font_id, src->size, src->flags);
    wp_ensure_run_cap(&after, after_len);
    montauk::memcpy(after.text, src->text + o, after_len);
    after.len = after_len;

    src->len = o;

    for (int i = wp->run_count - 1; i > r; i--)
        wp->runs[i + 1] = wp->runs[i];
    wp->runs[r + 1] = after;
    wp->run_count++;

    if (wp->cursor_run == r && wp->cursor_offset > o) {
        wp->cursor_run = r + 1;
        wp->cursor_offset -= o;
    } else if (wp->cursor_run > r) {
        wp->cursor_run++;
    }

    return r + 1;
}

void wp_load_fonts() {
    if (g_wp_fonts.loaded) return;

    auto load = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) {
            montauk::mfree(f);
            return nullptr;
        }
        return f;
    };

    g_wp_fonts.fonts[FONT_ROBOTO][0] = fonts::system_font ? fonts::system_font : load("0:/fonts/Roboto-Medium.ttf");
    g_wp_fonts.fonts[FONT_ROBOTO][1] = fonts::system_bold ? fonts::system_bold : load("0:/fonts/Roboto-Bold.ttf");
    g_wp_fonts.fonts[FONT_ROBOTO][2] = load("0:/fonts/Roboto-Italic.ttf");
    g_wp_fonts.fonts[FONT_ROBOTO][3] = load("0:/fonts/Roboto-BoldItalic.ttf");

    g_wp_fonts.fonts[FONT_NOTOSERIF][0] = load("0:/fonts/NotoSerif-Regular.ttf");
    g_wp_fonts.fonts[FONT_NOTOSERIF][1] = load("0:/fonts/NotoSerif-SemiBold.ttf");
    g_wp_fonts.fonts[FONT_NOTOSERIF][2] = load("0:/fonts/NotoSerif-Italic.ttf");
    g_wp_fonts.fonts[FONT_NOTOSERIF][3] = load("0:/fonts/NotoSerif-BoldItalic.ttf");

    g_wp_fonts.fonts[FONT_C059][0] = load("0:/fonts/C059-Roman.ttf");
    g_wp_fonts.fonts[FONT_C059][1] = load("0:/fonts/C059-Bold.ttf");
    g_wp_fonts.fonts[FONT_C059][2] = load("0:/fonts/C059-Italic.ttf");
    g_wp_fonts.fonts[FONT_C059][3] = load("0:/fonts/C059-Bold.ttf");

    g_ui_font = g_wp_fonts.fonts[FONT_ROBOTO][0];
    g_ui_bold = g_wp_fonts.fonts[FONT_ROBOTO][1] ? g_wp_fonts.fonts[FONT_ROBOTO][1] : g_ui_font;
    g_wp_fonts.loaded = true;
}

TrueTypeFont* wp_get_font(int font_id, uint8_t flags) {
    if (font_id < 0 || font_id >= FONT_COUNT) font_id = 0;

    int variant = 0;
    if ((flags & STYLE_BOLD) && (flags & STYLE_ITALIC)) variant = 3;
    else if (flags & STYLE_BOLD) variant = 1;
    else if (flags & STYLE_ITALIC) variant = 2;

    TrueTypeFont* f = g_wp_fonts.fonts[font_id][variant];
    if (f && f->valid) return f;

    f = g_wp_fonts.fonts[font_id][0];
    if (f && f->valid) return f;

    return g_ui_font ? g_ui_font : fonts::system_font;
}

void wp_init_empty_document(WordProcessorState* wp) {
    wp_init_run(&wp->runs[0], FONT_ROBOTO, WP_DEFAULT_SIZE, 0);
    wp->run_count = 1;
    wp->total_text_len = 0;

    wp->cursor_run = 0;
    wp->cursor_offset = 0;

    wp->sel_anchor = 0;
    wp->sel_end = 0;
    wp->has_selection = false;
    wp->mouse_selecting = false;

    wp->cur_font_id = FONT_ROBOTO;
    wp->cur_size = WP_DEFAULT_SIZE;
    wp->cur_flags = 0;

    wp->scrollbar.init(0, 0, WP_SCROLLBAR_W, 100);
    wp->content_height = 0;

    wp->wrap_lines = nullptr;
    wp->wrap_line_count = 0;
    wp->wrap_line_cap = 0;
    wp->wrap_dirty = true;
    wp->last_wrap_width = 0;

    wp->paragraph_count = 1;
    wp_init_paragraph_style(&wp->paragraphs[0]);

    wp->modified = false;
    wp->filepath[0] = '\0';
    wp->filename[0] = '\0';

    wp->show_pathbar = false;
    wp->pathbar_save_mode = false;
    wp->pathbar_text[0] = '\0';
    wp->pathbar_cursor = 0;
    wp->pathbar_len = 0;

    wp->font_dropdown_open = false;
    wp->size_dropdown_open = false;
    wp->line_spacing_dropdown_open = false;

    wp->undo_count = 0;
    wp->undo_pos = 0;
    montauk::memset(wp->undo, 0, sizeof(wp->undo));

    wp_history_reset(wp);
}

void wp_free_document(WordProcessorState* wp) {
    wp_free_runs(wp);
    if (wp->wrap_lines) {
        montauk::mfree(wp->wrap_lines);
        wp->wrap_lines = nullptr;
    }
    wp->wrap_line_count = 0;
    wp->wrap_line_cap = 0;
    wp_clear_history(wp);
}

int wp_abs_pos(WordProcessorState* wp, int run, int offset) {
    int pos = 0;
    for (int i = 0; i < run && i < wp->run_count; i++)
        pos += wp->runs[i].len;
    pos += offset;
    return pos;
}

void wp_pos_to_run(WordProcessorState* wp, int abs_pos, int* out_run, int* out_offset) {
    if (abs_pos <= 0) {
        *out_run = 0;
        *out_offset = 0;
        return;
    }

    int pos = 0;
    for (int i = 0; i < wp->run_count; i++) {
        if (pos + wp->runs[i].len >= abs_pos || i == wp->run_count - 1) {
            *out_run = i;
            *out_offset = abs_pos - pos;
            if (*out_offset > wp->runs[i].len) *out_offset = wp->runs[i].len;
            return;
        }
        pos += wp->runs[i].len;
    }

    *out_run = 0;
    *out_offset = 0;
}

char wp_char_at(WordProcessorState* wp, int abs_pos) {
    int r, o;
    wp_pos_to_run(wp, abs_pos, &r, &o);
    if (r < wp->run_count && o < wp->runs[r].len)
        return wp->runs[r].text[o];
    return '\0';
}

int wp_find_paragraph_at(WordProcessorState* wp, int abs_pos) {
    if (wp->paragraph_count <= 1 || abs_pos <= 0) return 0;
    if (abs_pos > wp->total_text_len) abs_pos = wp->total_text_len;

    int para = 0;
    for (int i = 0; i < abs_pos && i < wp->total_text_len; i++) {
        if (wp_char_at(wp, i) == '\n') {
            para++;
            if (para >= wp->paragraph_count - 1)
                return wp->paragraph_count - 1;
        }
    }
    return para;
}

void wp_selected_paragraph_range(WordProcessorState* wp, int* out_start_para, int* out_end_para) {
    if (!out_start_para || !out_end_para) return;

    if (wp->has_selection) {
        int sel_s, sel_e;
        wp_sel_range(wp, &sel_s, &sel_e);
        if (sel_s != sel_e) {
            *out_start_para = wp_find_paragraph_at(wp, sel_s);
            int end_abs = sel_e > 0 ? sel_e - 1 : 0;
            if (end_abs < sel_s) end_abs = sel_s;
            *out_end_para = wp_find_paragraph_at(wp, end_abs);
            return;
        }
    }

    int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    *out_start_para = wp_find_paragraph_at(wp, abs);
    *out_end_para = *out_start_para;
}

void wp_insert_char(WordProcessorState* wp, char c) {
    if (wp->total_text_len >= WP_MAX_TEXT - 1) return;
    if (c == '\n' && wp->paragraph_count >= WP_MAX_PARAGRAPHS) return;

    int insert_abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);

    StyledRun* cur = &wp->runs[wp->cursor_run];
    if (wp_same_style(cur, wp->cur_font_id, wp->cur_size, wp->cur_flags)) {
        wp_ensure_run_cap(cur, 1);
        for (int i = cur->len; i > wp->cursor_offset; i--)
            cur->text[i] = cur->text[i - 1];
        cur->text[wp->cursor_offset] = c;
        cur->len++;
        wp->cursor_offset++;
        wp->total_text_len++;
        wp->modified = true;
        wp->wrap_dirty = true;
        if (c == '\n') wp_on_insert_newline(wp, insert_abs);
        return;
    }

    if (wp->cursor_offset == 0) {
        if (wp->run_count >= WP_MAX_RUNS) return;
        for (int i = wp->run_count; i > wp->cursor_run; i--)
            wp->runs[i] = wp->runs[i - 1];
        wp->run_count++;
        StyledRun* nr = &wp->runs[wp->cursor_run];
        wp_init_run(nr, wp->cur_font_id, wp->cur_size, wp->cur_flags);
        wp_ensure_run_cap(nr, 1);
        nr->text[0] = c;
        nr->len = 1;
        wp->cursor_offset = 1;
        wp->total_text_len++;
    } else if (wp->cursor_offset == cur->len) {
        if (wp->run_count >= WP_MAX_RUNS) return;
        int insert_idx = wp->cursor_run + 1;
        for (int i = wp->run_count; i > insert_idx; i--)
            wp->runs[i] = wp->runs[i - 1];
        wp->run_count++;
        StyledRun* nr = &wp->runs[insert_idx];
        wp_init_run(nr, wp->cur_font_id, wp->cur_size, wp->cur_flags);
        wp_ensure_run_cap(nr, 1);
        nr->text[0] = c;
        nr->len = 1;
        wp->cursor_run = insert_idx;
        wp->cursor_offset = 1;
        wp->total_text_len++;
    } else {
        if (wp->run_count + 2 > WP_MAX_RUNS) return;
        int split_at = wp->cursor_offset;
        int after_len = cur->len - split_at;

        StyledRun after;
        wp_init_run(&after, cur->font_id, cur->size, cur->flags);
        wp_ensure_run_cap(&after, after_len);
        montauk::memcpy(after.text, cur->text + split_at, after_len);
        after.len = after_len;

        cur->len = split_at;

        int new_idx = wp->cursor_run + 1;
        for (int i = wp->run_count - 1; i >= new_idx; i--)
            wp->runs[i + 2] = wp->runs[i];

        StyledRun* nr = &wp->runs[new_idx];
        wp_init_run(nr, wp->cur_font_id, wp->cur_size, wp->cur_flags);
        wp_ensure_run_cap(nr, 1);
        nr->text[0] = c;
        nr->len = 1;

        wp->runs[new_idx + 1] = after;
        wp->run_count += 2;
        wp->cursor_run = new_idx;
        wp->cursor_offset = 1;
        wp->total_text_len++;
    }

    wp->modified = true;
    wp->wrap_dirty = true;
    if (c == '\n') wp_on_insert_newline(wp, insert_abs);
}

void wp_backspace(WordProcessorState* wp) {
    if (wp->cursor_run == 0 && wp->cursor_offset == 0) return;

    int removed_abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset) - 1;
    char removed = '\0';
    if (removed_abs >= 0)
        removed = wp_char_at(wp, removed_abs);

    if (wp->cursor_offset > 0) {
        StyledRun* r = &wp->runs[wp->cursor_run];
        for (int i = wp->cursor_offset - 1; i < r->len - 1; i++)
            r->text[i] = r->text[i + 1];
        r->len--;
        wp->cursor_offset--;
    } else {
        wp->cursor_run--;
        StyledRun* r = &wp->runs[wp->cursor_run];
        if (r->len > 0) r->len--;
        wp->cursor_offset = r->len;
    }

    wp->total_text_len--;
    wp->modified = true;
    wp->wrap_dirty = true;
    if (removed == '\n') wp_on_delete_newline(wp, removed_abs);
    wp_merge_adjacent(wp);
    wp_sync_paragraph_count(wp);
}

void wp_delete_char(WordProcessorState* wp) {
    int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    if (abs >= wp->total_text_len) return;

    char removed = wp_char_at(wp, abs);

    StyledRun* r = &wp->runs[wp->cursor_run];
    if (wp->cursor_offset < r->len) {
        for (int i = wp->cursor_offset; i < r->len - 1; i++)
            r->text[i] = r->text[i + 1];
        r->len--;
    } else if (wp->cursor_run + 1 < wp->run_count) {
        StyledRun* next = &wp->runs[wp->cursor_run + 1];
        if (next->len > 0) {
            for (int i = 0; i < next->len - 1; i++)
                next->text[i] = next->text[i + 1];
            next->len--;
        }
    }

    wp->total_text_len--;
    wp->modified = true;
    wp->wrap_dirty = true;
    if (removed == '\n') wp_on_delete_newline(wp, abs);
    wp_merge_adjacent(wp);
    wp_sync_paragraph_count(wp);
}

void wp_cursor_left(WordProcessorState* wp) {
    if (wp->cursor_offset > 0) {
        wp->cursor_offset--;
    } else if (wp->cursor_run > 0) {
        wp->cursor_run--;
        wp->cursor_offset = wp->runs[wp->cursor_run].len;
    }
}

void wp_cursor_right(WordProcessorState* wp) {
    if (wp->cursor_offset < wp->runs[wp->cursor_run].len) {
        wp->cursor_offset++;
    } else if (wp->cursor_run + 1 < wp->run_count) {
        wp->cursor_run++;
        wp->cursor_offset = 0;
    }
}

void wp_clear_selection(WordProcessorState* wp) {
    wp->has_selection = false;
    wp->sel_anchor = 0;
    wp->sel_end = 0;
}

void wp_sel_range(WordProcessorState* wp, int* out_start, int* out_end) {
    if (wp->sel_anchor < wp->sel_end) {
        *out_start = wp->sel_anchor;
        *out_end = wp->sel_end;
    } else {
        *out_start = wp->sel_end;
        *out_end = wp->sel_anchor;
    }
}

void wp_start_selection(WordProcessorState* wp) {
    if (!wp->has_selection) {
        wp->sel_anchor = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
        wp->sel_end = wp->sel_anchor;
        wp->has_selection = true;
    }
}

void wp_update_selection_to_cursor(WordProcessorState* wp) {
    wp->sel_end = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    if (wp->sel_anchor == wp->sel_end) wp->has_selection = false;
}

void wp_apply_style_to_selection(WordProcessorState* wp, int mode, int value) {
    if (!wp->has_selection) return;

    int sel_s, sel_e;
    wp_sel_range(wp, &sel_s, &sel_e);
    if (sel_s >= sel_e) return;

    int end_ri = wp_split_at(wp, sel_e);
    int start_ri = wp_split_at(wp, sel_s);

    {
        int pos = 0;
        end_ri = wp->run_count;
        for (int i = 0; i < wp->run_count; i++) {
            if (pos >= sel_e) {
                end_ri = i;
                break;
            }
            pos += wp->runs[i].len;
        }
    }

    for (int i = start_ri; i < end_ri && i < wp->run_count; i++) {
        StyledRun* r = &wp->runs[i];
        switch (mode) {
        case 0: r->font_id = (uint8_t)value; break;
        case 1: r->size = (uint8_t)value; break;
        case 2: r->flags ^= STYLE_BOLD; break;
        case 3: r->flags ^= STYLE_ITALIC; break;
        }
    }

    wp->wrap_dirty = true;
    wp->modified = true;
    wp_merge_adjacent(wp);
}

void wp_apply_alignment(WordProcessorState* wp, uint8_t align) {
    if (align > PARA_ALIGN_RIGHT) align = PARA_ALIGN_LEFT;

    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        if (wp->paragraphs[i].align != align) {
            wp->paragraphs[i].align = align;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_adjust_paragraph_indent(WordProcessorState* wp, int delta) {
    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        int v = wp->paragraphs[i].left_indent + delta;
        int16_t nv = (int16_t)gui_clamp(v, 0, 240);
        if (wp->paragraphs[i].left_indent != nv) {
            wp->paragraphs[i].left_indent = nv;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_adjust_paragraph_first_line_indent(WordProcessorState* wp, int delta) {
    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        int v = wp->paragraphs[i].first_line_indent + delta;
        int16_t nv = (int16_t)gui_clamp(v, -120, 120);
        if (wp->paragraphs[i].first_line_indent != nv) {
            wp->paragraphs[i].first_line_indent = nv;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_adjust_paragraph_spacing_before(WordProcessorState* wp, int delta) {
    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        int v = wp->paragraphs[i].space_before + delta;
        int16_t nv = (int16_t)gui_clamp(v, 0, 120);
        if (wp->paragraphs[i].space_before != nv) {
            wp->paragraphs[i].space_before = nv;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_adjust_paragraph_spacing_after(WordProcessorState* wp, int delta) {
    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        int v = wp->paragraphs[i].space_after + delta;
        int16_t nv = (int16_t)gui_clamp(v, 0, 120);
        if (wp->paragraphs[i].space_after != nv) {
            wp->paragraphs[i].space_after = nv;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_set_line_spacing(WordProcessorState* wp, int value) {
    int selected = WP_LINE_SPACING_OPTIONS[0];
    for (int i = 0; i < WP_LINE_SPACING_OPTION_COUNT; i++) {
        if (WP_LINE_SPACING_OPTIONS[i] == value) {
            selected = value;
            break;
        }
    }

    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        if (wp->paragraphs[i].line_spacing != selected) {
            wp->paragraphs[i].line_spacing = (uint8_t)selected;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_cycle_line_spacing(WordProcessorState* wp) {
    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);
    int current = wp->paragraphs[para_s].line_spacing;
    int next = WP_LINE_SPACING_OPTIONS[0];

    for (int i = 0; i < WP_LINE_SPACING_OPTION_COUNT; i++) {
        if (WP_LINE_SPACING_OPTIONS[i] == current) {
            next = WP_LINE_SPACING_OPTIONS[(i + 1) % WP_LINE_SPACING_OPTION_COUNT];
            break;
        }
    }

    wp_set_line_spacing(wp, next);
}

void wp_toggle_list(WordProcessorState* wp, uint8_t list_type) {
    if (list_type != PARA_LIST_BULLET && list_type != PARA_LIST_NUMBER)
        return;

    int para_s, para_e;
    wp_selected_paragraph_range(wp, &para_s, &para_e);

    bool turn_off = true;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        if (wp->paragraphs[i].list_type != list_type) {
            turn_off = false;
            break;
        }
    }

    bool changed = false;
    for (int i = para_s; i <= para_e && i < wp->paragraph_count; i++) {
        ParagraphStyle* para = &wp->paragraphs[i];
        if (turn_off) {
            if (para->list_type != PARA_LIST_NONE) {
                para->list_type = PARA_LIST_NONE;
                changed = true;
            }
            if (para->left_indent == WP_LIST_LEFT && para->first_line_indent == WP_LIST_HANGING) {
                para->left_indent = 0;
                para->first_line_indent = 0;
                changed = true;
            }
            continue;
        }

        if (para->list_type == PARA_LIST_NONE &&
            para->left_indent == 0 && para->first_line_indent == 0) {
            para->left_indent = WP_LIST_LEFT;
            para->first_line_indent = WP_LIST_HANGING;
            changed = true;
        }
        if (para->list_type != list_type) {
            para->list_type = list_type;
            changed = true;
        }
    }

    if (changed) {
        wp->modified = true;
        wp->wrap_dirty = true;
    }
}

void wp_delete_selection(WordProcessorState* wp) {
    if (!wp->has_selection) return;

    int sel_s, sel_e;
    wp_sel_range(wp, &sel_s, &sel_e);
    int count = sel_e - sel_s;
    if (count <= 0) {
        wp_clear_selection(wp);
        return;
    }

    wp_pos_to_run(wp, sel_s, &wp->cursor_run, &wp->cursor_offset);
    for (int i = 0; i < count; i++)
        wp_delete_char(wp);

    wp_clear_selection(wp);
}

void wp_recompute_wrap(WordProcessorState* wp, int content_w) {
    int wrap_width = content_w - WP_MARGIN * 2 - WP_SCROLLBAR_W;
    if (wrap_width < 50) wrap_width = 50;

    if (wrap_width != wp->last_wrap_width) {
        wp->wrap_dirty = true;
        wp->last_wrap_width = wrap_width;
    }

    if (!wp->wrap_dirty && wp->wrap_lines) return;

    wp->wrap_line_count = 0;
    if (!wp_ensure_wrap_capacity(wp, 1)) {
        wp->content_height = WP_MARGIN * 2;
        return;
    }
    int y = WP_MARGIN;
    int total = wp->total_text_len;
    int pos = 0;
    int para_idx = 0;
    int list_number = 1;

    wp_sync_paragraph_count(wp);

    while (para_idx < wp->paragraph_count && wp->wrap_line_count < WP_MAX_WRAP_LINES) {
        ParagraphStyle* para = &wp->paragraphs[para_idx];
        int para_start = pos;
        int para_content_end = pos;
        bool has_newline = false;

        while (para_content_end < total) {
            char ch = wp_char_at(wp, para_content_end);
            if (ch == '\n') {
                has_newline = true;
                break;
            }
            para_content_end++;
        }

        y += para->space_before;

        bool first_line = true;
        int line_start = para_start;
        while (line_start < para_content_end) {
            if (wp->wrap_line_count >= WP_MAX_WRAP_LINES) break;
            if (!wp_ensure_wrap_capacity(wp, wp->wrap_line_count + 1)) break;

            int text_indent = wp_effective_text_indent(para, first_line);
            int avail_width = wrap_width - text_indent;
            if (avail_width < 40) avail_width = 40;

            int scan = line_start;
            int width = 0;
            int last_space = -1;
            int width_at_last_space = 0;

            while (scan < para_content_end) {
                char ch = wp_char_at(wp, scan);
                int cw = wp_char_advance_at(wp, scan, ch);
                if (width + cw > avail_width && scan > line_start) {
                    if (last_space >= line_start) {
                        scan = last_space + 1;
                        width = width_at_last_space;
                    }
                    break;
                }

                width += cw;
                if (ch == ' ') {
                    last_space = scan;
                    width_at_last_space = width;
                }
                scan++;
            }

            if (scan == line_start && scan < para_content_end) {
                width += wp_char_advance_at(wp, scan, wp_char_at(wp, scan));
                scan++;
            }

            int char_count = scan - line_start;
            bool last_visual_line = (scan >= para_content_end);
            if (last_visual_line && has_newline) char_count++;

            int line_width = 0;
            int line_height = 0;
            int line_ascent = 0;
            wp_measure_line_range(wp, line_start, char_count, &line_width, &line_height, &line_ascent);

            WrapLine* line = &wp->wrap_lines[wp->wrap_line_count];
            wp_pos_to_run(wp, line_start, &line->run_idx, &line->run_offset);
            line->char_count = char_count;
            line->y = y;
            line->height = line_height;
            line->baseline = line_ascent;
            line->width = line_width;
            line->paragraph_idx = para_idx;
            line->list_number = (first_line && para->list_type == PARA_LIST_NUMBER) ? list_number : 0;
            line->first_in_paragraph = first_line;

            int extra = wrap_width - text_indent - line_width;
            if (extra < 0) extra = 0;
            int x = WP_MARGIN + text_indent;
            if (para->align == PARA_ALIGN_CENTER) x += extra / 2;
            else if (para->align == PARA_ALIGN_RIGHT) x += extra;
            line->x = x;

            wp->wrap_line_count++;

            line_start = scan;
            first_line = false;
            if (!last_visual_line)
                y += wp_line_spacing_advance(line_height, para->line_spacing);
            else
                y += line_height;
        }

        if (para_start == para_content_end && wp->wrap_line_count < WP_MAX_WRAP_LINES) {
            if (!wp_ensure_wrap_capacity(wp, wp->wrap_line_count + 1)) break;
            WrapLine* line = &wp->wrap_lines[wp->wrap_line_count];
            wp_pos_to_run(wp, para_start, &line->run_idx, &line->run_offset);
            line->char_count = has_newline ? 1 : 0;
            line->y = y;
            wp_default_line_metrics(wp, para_start, &line->height, &line->baseline);
            line->width = 0;
            line->paragraph_idx = para_idx;
            line->list_number = (para->list_type == PARA_LIST_NUMBER) ? list_number : 0;
            line->first_in_paragraph = true;

            int text_indent = wp_effective_text_indent(para, true);
            int extra = wrap_width - text_indent;
            if (extra < 0) extra = 0;
            int x = WP_MARGIN + text_indent;
            if (para->align == PARA_ALIGN_CENTER) x += extra / 2;
            else if (para->align == PARA_ALIGN_RIGHT) x += extra;
            line->x = x;

            wp->wrap_line_count++;
            y = line->y + line->height;
        }

        y += para->space_after;

        pos = para_content_end;
        if (has_newline) pos++;
        if (para->list_type == PARA_LIST_NUMBER) list_number++;
        else list_number = 1;
        para_idx++;
    }

    if (wp->wrap_line_count == 0) {
        if (!wp_ensure_wrap_capacity(wp, 1)) {
            wp->content_height = WP_MARGIN * 2;
            return;
        }
        WrapLine* line = &wp->wrap_lines[0];
        line->run_idx = 0;
        line->run_offset = 0;
        line->char_count = 0;
        line->y = WP_MARGIN;
        wp_default_line_metrics(wp, 0, &line->height, &line->baseline);
        line->x = WP_MARGIN;
        line->width = 0;
        line->paragraph_idx = 0;
        line->list_number = 0;
        line->first_in_paragraph = true;

        wp->wrap_line_count = 1;
        y = WP_MARGIN + line->height;
    }

    wp->content_height = y + WP_MARGIN;
    wp->wrap_dirty = false;
}

int wp_find_wrap_line(WordProcessorState* wp, int abs_pos) {
    int pos = 0;
    for (int i = 0; i < wp->wrap_line_count; i++) {
        int next_pos = pos + wp->wrap_lines[i].char_count;
        if (abs_pos < next_pos || i == wp->wrap_line_count - 1)
            return i;
        pos = next_pos;
    }
    return 0;
}

int wp_wrap_line_start(WordProcessorState* wp, int line_idx) {
    int pos = 0;
    for (int i = 0; i < line_idx && i < wp->wrap_line_count; i++)
        pos += wp->wrap_lines[i].char_count;
    return pos;
}

void wp_cursor_up(WordProcessorState* wp) {
    int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    int line = wp_find_wrap_line(wp, abs);
    if (line <= 0) return;

    int col = abs - wp_wrap_line_start(wp, line);
    int prev_start = wp_wrap_line_start(wp, line - 1);
    int prev_len = wp->wrap_lines[line - 1].char_count;
    int new_col = col < prev_len ? col : prev_len;
    if (new_col > 0 && new_col == prev_len) {
        char ch = wp_char_at(wp, prev_start + prev_len - 1);
        if (ch == '\n') new_col = prev_len - 1;
    }
    wp_pos_to_run(wp, prev_start + new_col, &wp->cursor_run, &wp->cursor_offset);
}

void wp_cursor_down(WordProcessorState* wp) {
    int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    int line = wp_find_wrap_line(wp, abs);
    if (line >= wp->wrap_line_count - 1) return;

    int col = abs - wp_wrap_line_start(wp, line);
    int next_start = wp_wrap_line_start(wp, line + 1);
    int next_len = wp->wrap_lines[line + 1].char_count;
    int new_col = col < next_len ? col : next_len;
    if (new_col > 0 && new_col == next_len && line + 1 < wp->wrap_line_count - 1) {
        char ch = wp_char_at(wp, next_start + next_len - 1);
        if (ch == '\n') new_col = next_len - 1;
    }
    wp_pos_to_run(wp, next_start + new_col, &wp->cursor_run, &wp->cursor_offset);
}

void wp_ensure_cursor_visible(WordProcessorState* wp, int view_h) {
    int abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    int line = wp_find_wrap_line(wp, abs);
    if (line < 0 || line >= wp->wrap_line_count) return;

    int cy = wp->wrap_lines[line].y;
    int ch = wp->wrap_lines[line].height;

    if (cy < wp->scrollbar.scroll_offset) {
        wp->scrollbar.scroll_offset = cy - WP_MARGIN;
        if (wp->scrollbar.scroll_offset < 0) wp->scrollbar.scroll_offset = 0;
    }
    if (cy + ch > wp->scrollbar.scroll_offset + view_h) {
        wp->scrollbar.scroll_offset = cy + ch - view_h + WP_MARGIN;
    }

    int ms = wp->scrollbar.max_scroll();
    if (wp->scrollbar.scroll_offset > ms) wp->scrollbar.scroll_offset = ms;
    if (wp->scrollbar.scroll_offset < 0) wp->scrollbar.scroll_offset = 0;
}

void wp_set_filepath(WordProcessorState* wp, const char* path) {
    montauk::strncpy(wp->filepath, path, 255);
    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;
    if (last_slash >= 0)
        montauk::strncpy(wp->filename, path + last_slash + 1, 63);
    else
        montauk::strncpy(wp->filename, path, 63);
}

void wp_start_pathbar(WordProcessorState* wp, bool save_mode, const char* initial_text) {
    if (!initial_text) initial_text = "";
    wp->show_pathbar = true;
    wp->pathbar_save_mode = save_mode;
    montauk::strncpy(wp->pathbar_text, initial_text, 255);
    wp->pathbar_text[255] = '\0';
    wp->pathbar_len = montauk::slen(wp->pathbar_text);
    wp->pathbar_cursor = wp->pathbar_len;
}

void wp_open_save_pathbar(WordProcessorState* wp) {
    char path[256] = {};
    const char* initial_path = wp->filepath[0] ? wp->filepath : "";
    const char* suggested_name = wp->filename[0] ? wp->filename : "document.mwp";
    char msg[160] = {};
    if (dialogs::save_file("Save Document", initial_path, suggested_name, path, sizeof(path), msg, sizeof(msg))) {
        wp_set_filepath(wp, path);
        wp_save_file(wp);
    } else if (msg[0]) {
        wp_start_pathbar(wp, true, initial_path[0] ? initial_path : suggested_name);
    }
}

static int wp_serialized_size(WordProcessorState* wp) {
    int size = 4 + 2 + 1 + 1 + 1 + 2 + 2;
    for (int i = 0; i < wp->run_count; i++)
        size += 2 + 1 + 1 + 1 + 1 + wp->runs[i].len;
    size += wp->paragraph_count * 12;
    return size;
}

static void wp_write_u16(uint8_t* buf, int* off, int value) {
    buf[(*off)++] = (uint8_t)(value & 0xFF);
    buf[(*off)++] = (uint8_t)((value >> 8) & 0xFF);
}

static int wp_read_u16(const uint8_t* buf, int* off) {
    int value = buf[*off] | (buf[*off + 1] << 8);
    *off += 2;
    return value;
}

static bool wp_serialize_document(WordProcessorState* wp, uint8_t** out_buf, int* out_size) {
    int size = wp_serialized_size(wp);
    uint8_t* buf = (uint8_t*)montauk::malloc(size);
    if (!buf) return false;

    int off = 0;
    buf[off++] = 'M';
    buf[off++] = 'W';
    buf[off++] = 'P';
    buf[off++] = '1';
    buf[off++] = 2;
    buf[off++] = 0;
    buf[off++] = wp->cur_font_id;
    buf[off++] = wp->cur_size;
    buf[off++] = wp->cur_flags;
    wp_write_u16(buf, &off, wp->run_count);
    wp_write_u16(buf, &off, wp->paragraph_count);

    for (int i = 0; i < wp->run_count; i++) {
        StyledRun* r = &wp->runs[i];
        wp_write_u16(buf, &off, r->len);
        buf[off++] = r->font_id;
        buf[off++] = r->size;
        buf[off++] = r->flags;
        buf[off++] = 0;
        montauk::memcpy(buf + off, r->text, r->len);
        off += r->len;
    }

    for (int i = 0; i < wp->paragraph_count; i++) {
        ParagraphStyle* para = &wp->paragraphs[i];
        buf[off++] = para->align;
        buf[off++] = para->list_type;
        buf[off++] = para->line_spacing;
        buf[off++] = 0;
        wp_write_u16(buf, &off, (uint16_t)para->left_indent);
        wp_write_u16(buf, &off, (uint16_t)para->first_line_indent);
        wp_write_u16(buf, &off, (uint16_t)para->space_before);
        wp_write_u16(buf, &off, (uint16_t)para->space_after);
    }

    *out_buf = buf;
    *out_size = off;
    return true;
}

static int wp_count_newlines_in_buf(const uint8_t* text, int len) {
    int count = 1;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') {
            count++;
            if (count > WP_MAX_PARAGRAPHS) return WP_MAX_PARAGRAPHS + 1;
        }
    }
    return count;
}

static bool wp_deserialize_document(WordProcessorState* wp, const uint8_t* buf, int size) {
    if (size < 10) return false;
    if (buf[0] != 'M' || buf[1] != 'W' || buf[2] != 'P' || buf[3] != '1')
        return false;

    uint8_t major = buf[4];
    if (major != 1 && major != 2) return false;

    if (major == 1) {
        if (size < 10) return false;
    } else {
        if (size < 13) return false;
    }

    int header_off = 6;
    uint8_t cur_font = buf[header_off++];
    uint8_t cur_size = buf[header_off++];
    uint8_t cur_flags = 0;
    if (major >= 2) cur_flags = buf[header_off++];

    if (header_off + 2 > size) return false;
    int run_count = buf[header_off] | (buf[header_off + 1] << 8);
    header_off += 2;
    int stored_para_count = 0;
    if (major >= 2) {
        if (header_off + 2 > size) return false;
        stored_para_count = buf[header_off] | (buf[header_off + 1] << 8);
        header_off += 2;
    }

    if (run_count < 0 || run_count > WP_MAX_RUNS) return false;

    int off = header_off;
    int actual_para_count = 1;
    for (int i = 0; i < run_count; i++) {
        if (off + 6 > size) return false;
        int text_len = wp_read_u16(buf, &off);
        off += 4;
        if (text_len < 0 || off + text_len > size) return false;
        int para_count = wp_count_newlines_in_buf(buf + off, text_len);
        if (para_count > WP_MAX_PARAGRAPHS) return false;
        actual_para_count += para_count - 1;
        if (actual_para_count > WP_MAX_PARAGRAPHS) return false;
        off += text_len;
    }

    if (major >= 2) {
        if (stored_para_count <= 0) stored_para_count = actual_para_count;
        if (stored_para_count > WP_MAX_PARAGRAPHS) return false;
        if (off + stored_para_count * 12 > size) return false;
    }

    wp_free_runs(wp);
    wp->paragraph_count = 1;
    wp_init_paragraph_style(&wp->paragraphs[0]);
    wp->total_text_len = 0;

    off = header_off;
    for (int i = 0; i < run_count; i++) {
        int text_len = wp_read_u16(buf, &off);
        uint8_t font_id = buf[off++];
        uint8_t run_size = buf[off++];
        uint8_t flags = buf[off++];
        off++;

        StyledRun* r = &wp->runs[wp->run_count];
        wp_init_run(r, font_id, run_size, flags);
        wp_ensure_run_cap(r, text_len);
        montauk::memcpy(r->text, buf + off, text_len);
        r->len = text_len;
        wp->run_count++;
        wp->total_text_len += text_len;
        off += text_len;
    }

    if (wp->run_count == 0) {
        wp_init_run(&wp->runs[0], cur_font, cur_size, cur_flags);
        wp->run_count = 1;
    }

    wp->cur_font_id = cur_font;
    wp->cur_size = cur_size;
    wp->cur_flags = cur_flags;

    wp->paragraph_count = actual_para_count > 0 ? actual_para_count : 1;
    for (int i = 0; i < wp->paragraph_count; i++)
        wp_init_paragraph_style(&wp->paragraphs[i]);

    if (major >= 2) {
        int limit = wp_min_int(stored_para_count, wp->paragraph_count);
        for (int i = 0; i < limit; i++) {
            ParagraphStyle* para = &wp->paragraphs[i];
            para->align = buf[off++];
            para->list_type = buf[off++];
            para->line_spacing = buf[off++];
            off++;
            para->left_indent = (int16_t)wp_read_u16(buf, &off);
            para->first_line_indent = (int16_t)wp_read_u16(buf, &off);
            para->space_before = (int16_t)wp_read_u16(buf, &off);
            para->space_after = (int16_t)wp_read_u16(buf, &off);

            if (para->align > PARA_ALIGN_RIGHT) para->align = PARA_ALIGN_LEFT;
            if (para->list_type > PARA_LIST_NUMBER) para->list_type = PARA_LIST_NONE;

            bool known_spacing = false;
            for (int j = 0; j < WP_LINE_SPACING_OPTION_COUNT; j++) {
                if (para->line_spacing == WP_LINE_SPACING_OPTIONS[j]) {
                    known_spacing = true;
                    break;
                }
            }
            if (!known_spacing) para->line_spacing = 100;
        }

        for (int i = limit; i < stored_para_count; i++)
            off += 12;
    }

    wp_sync_paragraph_count(wp);
    wp_merge_adjacent(wp);
    wp->cursor_run = 0;
    wp->cursor_offset = 0;
    wp->scrollbar.scroll_offset = 0;
    wp->modified = false;
    wp->wrap_dirty = true;
    wp->sel_anchor = 0;
    wp->sel_end = 0;
    wp->has_selection = false;
    wp->mouse_selecting = false;
    wp->font_dropdown_open = false;
    wp->size_dropdown_open = false;
    wp->line_spacing_dropdown_open = false;
    wp->show_pathbar = false;
    return true;
}

static bool wp_capture_snapshot(WordProcessorState* wp, UndoSnapshot* snap) {
    montauk::memset(snap, 0, sizeof(*snap));
    if (!wp_serialize_document(wp, &snap->data, &snap->size))
        return false;

    snap->cursor_abs = wp_abs_pos(wp, wp->cursor_run, wp->cursor_offset);
    snap->sel_anchor = wp->sel_anchor;
    snap->sel_end = wp->sel_end;
    snap->has_selection = wp->has_selection;
    snap->modified = wp->modified;
    return true;
}

static bool wp_snapshots_match(const UndoSnapshot* a, const UndoSnapshot* b) {
    if (a->size != b->size) return false;
    if (a->cursor_abs != b->cursor_abs) return false;
    if (a->sel_anchor != b->sel_anchor || a->sel_end != b->sel_end) return false;
    if (a->has_selection != b->has_selection) return false;
    if (a->modified != b->modified) return false;
    if (a->size == 0) return true;
    return memcmp(a->data, b->data, a->size) == 0;
}

static bool wp_restore_snapshot(WordProcessorState* wp, const UndoSnapshot* snap) {
    if (!wp_deserialize_document(wp, snap->data, snap->size))
        return false;

    int cursor_abs = gui_clamp(snap->cursor_abs, 0, wp->total_text_len);
    wp_pos_to_run(wp, cursor_abs, &wp->cursor_run, &wp->cursor_offset);
    wp->sel_anchor = gui_clamp(snap->sel_anchor, 0, wp->total_text_len);
    wp->sel_end = gui_clamp(snap->sel_end, 0, wp->total_text_len);
    wp->has_selection = snap->has_selection && (wp->sel_anchor != wp->sel_end);
    wp->modified = snap->modified;
    wp->scrollbar.scroll_offset = 0;
    return true;
}

void wp_save_file(WordProcessorState* wp) {
    if (wp->filepath[0] == '\0') {
        wp_open_save_pathbar(wp);
        return;
    }

    uint8_t* buf = nullptr;
    int off = 0;
    if (!wp_serialize_document(wp, &buf, &off))
        return;

    int fd = montauk::fcreate(wp->filepath);
    if (fd >= 0) {
        montauk::fwrite(fd, buf, 0, off);
        montauk::close(fd);
        wp_history_mark_saved(wp);
    }

    montauk::mfree(buf);
}

void wp_load_file(WordProcessorState* wp, const char* path) {
    int fd = montauk::open(path);
    if (fd < 0) return;

    uint64_t fsize = montauk::getsize(fd);
    if (fsize < 10 || fsize > WP_MAX_TEXT * 2) {
        montauk::close(fd);
        return;
    }

    uint8_t* buf = (uint8_t*)montauk::malloc((int)fsize);
    montauk::read(fd, buf, 0, fsize);
    montauk::close(fd);

    if (buf[0] != 'M' || buf[1] != 'W' || buf[2] != 'P' || buf[3] != '1') {
        montauk::mfree(buf);
        return;
    }

    bool ok = wp_deserialize_document(wp, buf, (int)fsize);
    montauk::mfree(buf);
    if (!ok) return;

    wp_set_filepath(wp, path);
    wp_history_reset(wp);
}

void wp_history_reset(WordProcessorState* wp) {
    wp_clear_history(wp);

    UndoSnapshot snap = {};
    if (!wp_capture_snapshot(wp, &snap))
        return;

    wp->undo[0] = snap;
    wp->undo_count = 1;
    wp->undo_pos = 0;
}

void wp_history_checkpoint(WordProcessorState* wp) {
    UndoSnapshot snap = {};
    if (!wp_capture_snapshot(wp, &snap))
        return;

    if (wp->undo_count > 0 && wp_snapshots_match(&snap, &wp->undo[wp->undo_pos])) {
        wp_free_snapshot(&snap);
        return;
    }

    for (int i = wp->undo_pos + 1; i < wp->undo_count; i++)
        wp_free_snapshot(&wp->undo[i]);
    wp->undo_count = wp->undo_pos + 1;

    if (wp->undo_count >= WP_UNDO_MAX) {
        wp_free_snapshot(&wp->undo[0]);
        for (int i = 1; i < wp->undo_count; i++)
            wp->undo[i - 1] = wp->undo[i];
        montauk::memset(&wp->undo[wp->undo_count - 1], 0, sizeof(UndoSnapshot));
        wp->undo_count--;
        if (wp->undo_pos > 0) wp->undo_pos--;
    }

    wp->undo[wp->undo_count++] = snap;
    wp->undo_pos = wp->undo_count - 1;
}

void wp_history_mark_saved(WordProcessorState* wp) {
    wp->modified = false;
    if (wp->undo_count <= 0) {
        wp_history_reset(wp);
        return;
    }
    wp->undo[wp->undo_pos].modified = false;
}

bool wp_undo(WordProcessorState* wp) {
    if (wp->undo_count <= 0 || wp->undo_pos <= 0)
        return false;

    wp->undo_pos--;
    return wp_restore_snapshot(wp, &wp->undo[wp->undo_pos]);
}

bool wp_redo(WordProcessorState* wp) {
    if (wp->undo_count <= 0 || wp->undo_pos >= wp->undo_count - 1)
        return false;

    wp->undo_pos++;
    return wp_restore_snapshot(wp, &wp->undo[wp->undo_pos]);
}
