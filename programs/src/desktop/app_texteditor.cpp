/*
    * app_texteditor.cpp
    * ZenithOS Desktop - Text Editor application
    * Single-buffer text editor with line numbers, cursor, scrolling, file I/O
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Text Editor state
// ============================================================================

static constexpr int TE_STATUS_H    = 24;
static constexpr int TE_LINE_NUM_W  = 48;
static constexpr int TE_INIT_CAP    = 4096;
static constexpr int TE_MAX_CAP     = 262144;  // 256KB
static constexpr int TE_MAX_LINES   = 16384;
static constexpr int TE_TAB_WIDTH   = 4;

struct TextEditorState {
    char* buffer;
    int buf_len;
    int buf_cap;
    int* line_offsets;
    int line_count;
    int cursor_pos;       // byte position in buffer
    int cursor_line;
    int cursor_col;
    int scroll_y;         // first visible line
    int scroll_x;         // horizontal scroll in pixels
    bool modified;
    char filepath[256];
    char filename[64];
    DesktopState* desktop;
};

// ============================================================================
// Line index management
// ============================================================================

static void te_recompute_lines(TextEditorState* te) {
    if (!te->line_offsets) {
        te->line_offsets = (int*)zenith::malloc(TE_MAX_LINES * sizeof(int));
    }

    te->line_count = 0;
    te->line_offsets[te->line_count++] = 0;

    for (int i = 0; i < te->buf_len; i++) {
        if (te->buffer[i] == '\n' && te->line_count < TE_MAX_LINES) {
            te->line_offsets[te->line_count++] = i + 1;
        }
    }
}

static void te_update_cursor_pos(TextEditorState* te) {
    // Find which line the cursor is on
    te->cursor_line = 0;
    for (int i = te->line_count - 1; i >= 0; i--) {
        if (te->cursor_pos >= te->line_offsets[i]) {
            te->cursor_line = i;
            break;
        }
    }
    te->cursor_col = te->cursor_pos - te->line_offsets[te->cursor_line];
}

static int te_line_length(TextEditorState* te, int line) {
    if (line < 0 || line >= te->line_count) return 0;
    int start = te->line_offsets[line];
    int end;
    if (line + 1 < te->line_count) {
        end = te->line_offsets[line + 1] - 1; // exclude newline
    } else {
        end = te->buf_len;
    }
    return end - start;
}

// ============================================================================
// Buffer operations
// ============================================================================

static void te_ensure_capacity(TextEditorState* te, int needed) {
    if (te->buf_len + needed <= te->buf_cap) return;
    int new_cap = te->buf_cap * 2;
    if (new_cap > TE_MAX_CAP) new_cap = TE_MAX_CAP;
    if (new_cap < te->buf_len + needed) new_cap = te->buf_len + needed;
    te->buffer = (char*)zenith::realloc(te->buffer, new_cap);
    te->buf_cap = new_cap;
}

static void te_insert_char(TextEditorState* te, char c) {
    if (te->buf_len >= TE_MAX_CAP - 1) return;
    te_ensure_capacity(te, 1);

    // Shift everything after cursor right
    for (int i = te->buf_len; i > te->cursor_pos; i--) {
        te->buffer[i] = te->buffer[i - 1];
    }
    te->buffer[te->cursor_pos] = c;
    te->buf_len++;
    te->cursor_pos++;
    te->modified = true;

    te_recompute_lines(te);
    te_update_cursor_pos(te);
}

static void te_insert_string(TextEditorState* te, const char* s, int len) {
    if (te->buf_len + len >= TE_MAX_CAP) return;
    te_ensure_capacity(te, len);

    for (int i = te->buf_len - 1; i >= te->cursor_pos; i--) {
        te->buffer[i + len] = te->buffer[i];
    }
    for (int i = 0; i < len; i++) {
        te->buffer[te->cursor_pos + i] = s[i];
    }
    te->buf_len += len;
    te->cursor_pos += len;
    te->modified = true;

    te_recompute_lines(te);
    te_update_cursor_pos(te);
}

static void te_backspace(TextEditorState* te) {
    if (te->cursor_pos <= 0) return;
    te->cursor_pos--;
    for (int i = te->cursor_pos; i < te->buf_len - 1; i++) {
        te->buffer[i] = te->buffer[i + 1];
    }
    te->buf_len--;
    te->modified = true;

    te_recompute_lines(te);
    te_update_cursor_pos(te);
}

static void te_delete_char(TextEditorState* te) {
    if (te->cursor_pos >= te->buf_len) return;
    for (int i = te->cursor_pos; i < te->buf_len - 1; i++) {
        te->buffer[i] = te->buffer[i + 1];
    }
    te->buf_len--;
    te->modified = true;

    te_recompute_lines(te);
    te_update_cursor_pos(te);
}

// ============================================================================
// Cursor movement
// ============================================================================

static void te_move_up(TextEditorState* te) {
    if (te->cursor_line <= 0) return;
    int target_col = te->cursor_col;
    int prev_line = te->cursor_line - 1;
    int prev_len = te_line_length(te, prev_line);
    if (target_col > prev_len) target_col = prev_len;
    te->cursor_pos = te->line_offsets[prev_line] + target_col;
    te_update_cursor_pos(te);
}

static void te_move_down(TextEditorState* te) {
    if (te->cursor_line >= te->line_count - 1) return;
    int target_col = te->cursor_col;
    int next_line = te->cursor_line + 1;
    int next_len = te_line_length(te, next_line);
    if (target_col > next_len) target_col = next_len;
    te->cursor_pos = te->line_offsets[next_line] + target_col;
    te_update_cursor_pos(te);
}

static void te_move_left(TextEditorState* te) {
    if (te->cursor_pos > 0) {
        te->cursor_pos--;
        te_update_cursor_pos(te);
    }
}

static void te_move_right(TextEditorState* te) {
    if (te->cursor_pos < te->buf_len) {
        te->cursor_pos++;
        te_update_cursor_pos(te);
    }
}

static void te_move_home(TextEditorState* te) {
    te->cursor_pos = te->line_offsets[te->cursor_line];
    te_update_cursor_pos(te);
}

static void te_move_end(TextEditorState* te) {
    te->cursor_pos = te->line_offsets[te->cursor_line] + te_line_length(te, te->cursor_line);
    te_update_cursor_pos(te);
}

// ============================================================================
// Scrolling
// ============================================================================

static void te_ensure_cursor_visible(TextEditorState* te, int visible_lines) {
    if (te->cursor_line < te->scroll_y) {
        te->scroll_y = te->cursor_line;
    }
    if (te->cursor_line >= te->scroll_y + visible_lines) {
        te->scroll_y = te->cursor_line - visible_lines + 1;
    }

    // Horizontal scroll
    int cursor_px = te->cursor_col * FONT_WIDTH;
    int view_w = 580 - TE_LINE_NUM_W; // approximate
    if (cursor_px - te->scroll_x > view_w - FONT_WIDTH * 2) {
        te->scroll_x = cursor_px - view_w + FONT_WIDTH * 4;
    }
    if (cursor_px < te->scroll_x) {
        te->scroll_x = cursor_px - FONT_WIDTH * 2;
        if (te->scroll_x < 0) te->scroll_x = 0;
    }
}

// ============================================================================
// File I/O
// ============================================================================

static void te_load_file(TextEditorState* te, const char* path) {
    int fd = zenith::open(path);
    if (fd < 0) return;

    uint64_t size = zenith::getsize(fd);
    if (size > TE_MAX_CAP) size = TE_MAX_CAP;

    if ((int)size >= te->buf_cap) {
        int new_cap = (int)size + 1024;
        if (new_cap > TE_MAX_CAP) new_cap = TE_MAX_CAP;
        te->buffer = (char*)zenith::realloc(te->buffer, new_cap);
        te->buf_cap = new_cap;
    }

    zenith::read(fd, (uint8_t*)te->buffer, 0, size);
    zenith::close(fd);

    te->buf_len = (int)size;
    te->cursor_pos = 0;
    te->scroll_y = 0;
    te->scroll_x = 0;
    te->modified = false;

    zenith::strncpy(te->filepath, path, 255);

    // Extract filename from path
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash >= 0) {
        zenith::strncpy(te->filename, path + last_slash + 1, 63);
    } else {
        zenith::strncpy(te->filename, path, 63);
    }

    te_recompute_lines(te);
    te_update_cursor_pos(te);
}

static void te_save_file(TextEditorState* te) {
    if (te->filepath[0] == '\0') return;

    int fd = zenith::fcreate(te->filepath);
    if (fd < 0) return;

    zenith::fwrite(fd, (const uint8_t*)te->buffer, 0, te->buf_len);
    zenith::close(fd);

    te->modified = false;
}

// ============================================================================
// Drawing
// ============================================================================

static void texteditor_on_draw(Window* win, Framebuffer& fb) {
    TextEditorState* te = (TextEditorState*)win->app_data;
    if (!te) return;

    Rect cr = win->content_rect();
    int cw = cr.w;
    int ch = cr.h;
    uint32_t* pixels = win->content;

    // Background
    uint32_t bg_px = colors::WINDOW_BG.to_pixel();
    for (int i = 0; i < cw * ch; i++) pixels[i] = bg_px;

    int text_area_h = ch - TE_STATUS_H;
    int visible_lines = text_area_h / FONT_HEIGHT;

    te_ensure_cursor_visible(te, visible_lines);

    // Line number gutter background
    uint32_t gutter_px = Color::from_rgb(0xF0, 0xF0, 0xF0).to_pixel();
    for (int y = 0; y < text_area_h && y < ch; y++)
        for (int x = 0; x < TE_LINE_NUM_W && x < cw; x++)
            pixels[y * cw + x] = gutter_px;

    // Gutter separator
    uint32_t sep_px = colors::BORDER.to_pixel();
    if (TE_LINE_NUM_W < cw) {
        for (int y = 0; y < text_area_h && y < ch; y++)
            pixels[y * cw + TE_LINE_NUM_W] = sep_px;
    }

    // Draw lines
    uint32_t text_px = colors::TEXT_COLOR.to_pixel();
    uint32_t linenum_px = Color::from_rgb(0x99, 0x99, 0x99).to_pixel();
    uint32_t cursor_line_px = Color::from_rgb(0xFF, 0xFD, 0xE8).to_pixel();
    uint32_t cursor_px = colors::ACCENT.to_pixel();

    int text_start_x = TE_LINE_NUM_W + 4;

    for (int vis = 0; vis < visible_lines + 1; vis++) {
        int line = te->scroll_y + vis;
        if (line >= te->line_count) break;

        int py = vis * FONT_HEIGHT;
        if (py >= text_area_h) break;

        // Cursor line highlighting
        if (line == te->cursor_line) {
            for (int y = py; y < py + FONT_HEIGHT && y < text_area_h; y++)
                for (int x = TE_LINE_NUM_W + 1; x < cw; x++)
                    pixels[y * cw + x] = cursor_line_px;
        }

        // Line number
        char num_str[8];
        snprintf(num_str, 8, "%4d", line + 1);
        draw_text_to_pixels(pixels, cw, ch, 4, py, num_str, linenum_px);

        // Line text
        int line_start = te->line_offsets[line];
        int line_len = te_line_length(te, line);

        for (int ci = 0; ci < line_len; ci++) {
            int px = text_start_x + ci * FONT_WIDTH - te->scroll_x;
            if (px + FONT_WIDTH <= TE_LINE_NUM_W + 1) continue;
            if (px >= cw) break;

            char c = te->buffer[line_start + ci];
            if (c >= 32 || c < 0) {
                const uint8_t* glyph = &font_data[(unsigned char)c * FONT_HEIGHT];
                for (int fy = 0; fy < FONT_HEIGHT && py + fy < text_area_h; fy++) {
                    uint8_t bits = glyph[fy];
                    for (int fx = 0; fx < FONT_WIDTH; fx++) {
                        if (bits & (0x80 >> fx)) {
                            int dx = px + fx;
                            int dy = py + fy;
                            if (dx > TE_LINE_NUM_W && dx < cw && dy >= 0 && dy < text_area_h)
                                pixels[dy * cw + dx] = text_px;
                        }
                    }
                }
            }
        }

        // Draw cursor
        if (line == te->cursor_line) {
            int cx = text_start_x + te->cursor_col * FONT_WIDTH - te->scroll_x;
            if (cx > TE_LINE_NUM_W && cx + 2 <= cw) {
                for (int y = py; y < py + FONT_HEIGHT && y < text_area_h; y++) {
                    pixels[y * cw + cx] = cursor_px;
                    if (cx + 1 < cw)
                        pixels[y * cw + cx + 1] = cursor_px;
                }
            }
        }
    }

    // ---- Status bar ----
    int status_y = ch - TE_STATUS_H;
    uint32_t status_bg = Color::from_rgb(0x2B, 0x3E, 0x50).to_pixel();
    uint32_t status_fg = colors::PANEL_TEXT.to_pixel();

    for (int y = status_y; y < ch; y++)
        for (int x = 0; x < cw; x++)
            pixels[y * cw + x] = status_bg;

    // Filename + modified flag
    char status_left[128];
    if (te->filename[0]) {
        snprintf(status_left, 128, " %s%s", te->filename, te->modified ? " [modified]" : "");
    } else {
        snprintf(status_left, 128, " Untitled%s", te->modified ? " [modified]" : "");
    }
    draw_text_to_pixels(pixels, cw, ch, 4, status_y + (TE_STATUS_H - FONT_HEIGHT) / 2,
                        status_left, status_fg);

    // Cursor position (right side)
    char status_right[32];
    snprintf(status_right, 32, "Ln %d, Col %d ", te->cursor_line + 1, te->cursor_col + 1);
    int sr_w = zenith::slen(status_right) * FONT_WIDTH;
    draw_text_to_pixels(pixels, cw, ch, cw - sr_w - 4,
                        status_y + (TE_STATUS_H - FONT_HEIGHT) / 2,
                        status_right, status_fg);
}

// ============================================================================
// Mouse handling
// ============================================================================

static void texteditor_on_mouse(Window* win, MouseEvent& ev) {
    TextEditorState* te = (TextEditorState*)win->app_data;
    if (!te) return;

    Rect cr = win->content_rect();
    int local_x = ev.x - cr.x;
    int local_y = ev.y - cr.y;
    int text_area_h = cr.h - TE_STATUS_H;

    if (ev.left_pressed() && local_y < text_area_h && local_x > TE_LINE_NUM_W) {
        // Click to position cursor
        int clicked_line = te->scroll_y + local_y / FONT_HEIGHT;
        if (clicked_line >= te->line_count) clicked_line = te->line_count - 1;
        if (clicked_line < 0) clicked_line = 0;

        int clicked_col = (local_x - TE_LINE_NUM_W - 4 + te->scroll_x + FONT_WIDTH / 2) / FONT_WIDTH;
        if (clicked_col < 0) clicked_col = 0;
        int line_len = te_line_length(te, clicked_line);
        if (clicked_col > line_len) clicked_col = line_len;

        te->cursor_pos = te->line_offsets[clicked_line] + clicked_col;
        te_update_cursor_pos(te);
    }

    // Scroll
    if (ev.scroll != 0 && local_y < text_area_h) {
        te->scroll_y -= ev.scroll * 3;
        if (te->scroll_y < 0) te->scroll_y = 0;
        int max_scroll = te->line_count - (text_area_h / FONT_HEIGHT) + 1;
        if (max_scroll < 0) max_scroll = 0;
        if (te->scroll_y > max_scroll) te->scroll_y = max_scroll;
    }
}

// ============================================================================
// Keyboard handling
// ============================================================================

static void texteditor_on_key(Window* win, const Zenith::KeyEvent& key) {
    TextEditorState* te = (TextEditorState*)win->app_data;
    if (!te || !key.pressed) return;

    // Ctrl+S: save
    if (key.ctrl && (key.ascii == 's' || key.ascii == 'S')) {
        te_save_file(te);
        return;
    }

    // Arrow keys
    if (key.scancode == 0x48) { te_move_up(te); return; }
    if (key.scancode == 0x50) { te_move_down(te); return; }
    if (key.scancode == 0x4B) { te_move_left(te); return; }
    if (key.scancode == 0x4D) { te_move_right(te); return; }

    // Home
    if (key.scancode == 0x47) { te_move_home(te); return; }
    // End
    if (key.scancode == 0x4F) { te_move_end(te); return; }
    // Delete
    if (key.scancode == 0x53) { te_delete_char(te); return; }

    // Backspace
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        te_backspace(te);
        return;
    }

    // Enter
    if (key.ascii == '\n' || key.ascii == '\r') {
        te_insert_char(te, '\n');
        return;
    }

    // Tab
    if (key.ascii == '\t') {
        for (int i = 0; i < TE_TAB_WIDTH; i++) {
            te_insert_char(te, ' ');
        }
        return;
    }

    // Printable characters
    if (key.ascii >= 32 && key.ascii < 127) {
        te_insert_char(te, key.ascii);
        return;
    }
}

static void texteditor_on_close(Window* win) {
    TextEditorState* te = (TextEditorState*)win->app_data;
    if (te) {
        if (te->buffer) zenith::mfree(te->buffer);
        if (te->line_offsets) zenith::mfree(te->line_offsets);
        zenith::mfree(te);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Text Editor launchers
// ============================================================================

void open_texteditor(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Text Editor", 180, 60, 600, 450);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    TextEditorState* te = (TextEditorState*)zenith::malloc(sizeof(TextEditorState));
    zenith::memset(te, 0, sizeof(TextEditorState));

    te->buffer = (char*)zenith::malloc(TE_INIT_CAP);
    te->buf_cap = TE_INIT_CAP;
    te->buf_len = 0;
    te->modified = false;
    te->desktop = ds;

    te_recompute_lines(te);
    te_update_cursor_pos(te);

    win->app_data = te;
    win->on_draw = texteditor_on_draw;
    win->on_mouse = texteditor_on_mouse;
    win->on_key = texteditor_on_key;
    win->on_close = texteditor_on_close;
}

void open_texteditor_with_file(DesktopState* ds, const char* path) {
    // Extract filename for window title
    const char* name = path;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') name = path + i + 1;
    }

    char title[64];
    snprintf(title, 64, "%s - Editor", name);

    int idx = desktop_create_window(ds, title, 180, 60, 600, 450);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    TextEditorState* te = (TextEditorState*)zenith::malloc(sizeof(TextEditorState));
    zenith::memset(te, 0, sizeof(TextEditorState));

    te->buffer = (char*)zenith::malloc(TE_INIT_CAP);
    te->buf_cap = TE_INIT_CAP;
    te->buf_len = 0;
    te->modified = false;
    te->desktop = ds;

    // Initialize line index for empty document first (ensures line_offsets
    // is non-null even if te_load_file fails to open the file)
    te_recompute_lines(te);
    te_update_cursor_pos(te);

    te_load_file(te, path);

    win->app_data = te;
    win->on_draw = texteditor_on_draw;
    win->on_mouse = texteditor_on_mouse;
    win->on_key = texteditor_on_key;
    win->on_close = texteditor_on_close;
}
