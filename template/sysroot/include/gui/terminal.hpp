/*
    * terminal.hpp
    * MontaukOS terminal emulator with ANSI escape sequence support
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/font.hpp"
#include <montauk/syscall.h>
#include <montauk/string.h>
#include <Api/Syscall.hpp>

namespace gui {

struct TermCell {
    char ch;
    Color fg;
    Color bg;
};

static constexpr int TERM_MAX_SCROLLBACK = 500;

struct TerminalState {
    TermCell* cells;
    TermCell* alt_cells;         // alternate screen buffer
    int cols, rows;
    int cursor_x, cursor_y;
    int saved_cursor_x, saved_cursor_y;  // saved cursor for alternate screen
    int scrollback_lines;        // lines of history above visible screen (0..max_scrollback)
    int max_scrollback;          // 0 for viewers (klog), TERM_MAX_SCROLLBACK for terminal
    int view_offset;             // how many rows scrolled back (0 = live view)
    int child_pid;
    void* desktop;               // DesktopState* for closing window on child exit
    Color current_fg;
    Color current_bg;
    bool cursor_visible;
    bool alt_screen_active;
    bool reverse_video;
    bool dirty;                  // true when content changed since last render

    enum { STATE_NORMAL, STATE_ESC, STATE_CSI } parse_state;
    bool csi_private;            // true if '?' was seen after CSI
    int csi_params[8];
    int csi_param_count;
    int csi_current_param;
};

// Standard ANSI color palette as ARGB pixels
static inline Color term_ansi_color(int idx) {
    switch (idx) {
    case 0:  return Color::from_hex(0x000000);
    case 1:  return Color::from_hex(0xCC0000);
    case 2:  return Color::from_hex(0x4E9A06);
    case 3:  return Color::from_hex(0xC4A000);
    case 4:  return Color::from_hex(0x3465A4);
    case 5:  return Color::from_hex(0x75507B);
    case 6:  return Color::from_hex(0x06989A);
    case 7:  return Color::from_hex(0xD3D7CF);
    case 8:  return Color::from_hex(0x555753);
    case 9:  return Color::from_hex(0xEF2929);
    case 10: return Color::from_hex(0x8AE234);
    case 11: return Color::from_hex(0xFCE94F);
    case 12: return Color::from_hex(0x729FCF);
    case 13: return Color::from_hex(0xAD7FA8);
    case 14: return Color::from_hex(0x34E2E2);
    case 15: return Color::from_hex(0xEEEEEC);
    default: return colors::TERM_FG;
    }
}

// Helper: pointer to start of screen-relative row r in the cells buffer
static inline TermCell* term_screen_row(TerminalState* t, int r) {
    return &t->cells[(t->scrollback_lines + r) * t->cols];
}

static inline void terminal_scroll_up(TerminalState* t) {
    if (!t->alt_screen_active && t->scrollback_lines < t->max_scrollback) {
        // Room for scrollback: top visible row becomes scrollback, no data movement
        t->scrollback_lines++;
    } else if (!t->alt_screen_active && t->max_scrollback > 0) {
        // Scrollback full: discard oldest line, shift entire buffer up by one row
        int total = (t->max_scrollback + t->rows - 1) * t->cols * sizeof(TermCell);
        montauk::memmove(t->cells, t->cells + t->cols, total);
    } else {
        // Alt screen or no scrollback (klog): shift visible area up
        TermCell* screen = term_screen_row(t, 0);
        montauk::memmove(screen, screen + t->cols, (t->rows - 1) * t->cols * sizeof(TermCell));
    }

    // Clear the new bottom visible row
    TermCell* bottom = term_screen_row(t, t->rows - 1);
    for (int c = 0; c < t->cols; c++) {
        bottom[c] = {' ', t->current_fg, colors::TERM_BG};
    }

    // Keep user's scrolled-back viewport stable
    if (t->view_offset > 0) {
        t->view_offset++;
        if (t->view_offset > t->scrollback_lines)
            t->view_offset = t->scrollback_lines;
    }
}

// Initialize only the cell grid (no child process). Used by viewers like klog.
// max_sb = 0 for viewers, TERM_MAX_SCROLLBACK for the real terminal.
static inline void terminal_init_cells(TerminalState* t, int cols, int rows, int max_sb = 0) {
    t->cols = cols;
    t->rows = rows;
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->saved_cursor_x = 0;
    t->saved_cursor_y = 0;
    t->scrollback_lines = 0;
    t->max_scrollback = max_sb;
    t->view_offset = 0;
    t->current_fg = colors::TERM_FG;
    t->current_bg = colors::TERM_BG;
    t->cursor_visible = false;
    t->alt_screen_active = false;
    t->reverse_video = false;
    t->dirty = true;
    t->parse_state = TerminalState::STATE_NORMAL;
    t->csi_private = false;
    t->csi_param_count = 0;
    t->csi_current_param = 0;
    t->child_pid = 0;

    int total_cells = (rows + max_sb) * cols;
    int screen_cells = rows * cols;
    t->cells = (TermCell*)montauk::alloc(total_cells * sizeof(TermCell));
    t->alt_cells = (TermCell*)montauk::alloc(screen_cells * sizeof(TermCell));
    if (!t->cells || !t->alt_cells) {
        // Allocation failed — leave terminal in a safe but unusable state
        if (t->cells) { montauk::free(t->cells); t->cells = nullptr; }
        if (t->alt_cells) { montauk::free(t->alt_cells); t->alt_cells = nullptr; }
        t->cols = 0; t->rows = 0;
        return;
    }
    for (int i = 0; i < total_cells; i++) {
        t->cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    }
    for (int i = 0; i < screen_cells; i++) {
        t->alt_cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    }
}

static inline void terminal_init(TerminalState* t, int cols, int rows) {
    terminal_init_cells(t, cols, rows, TERM_MAX_SCROLLBACK);
    t->cursor_visible = true;

    t->child_pid = montauk::spawn_redir("0:/os/shell.elf");
    if (t->child_pid > 0)
        montauk::childio_settermsz(t->child_pid, cols, rows);
}

static inline void terminal_put_char(TerminalState* t, char ch) {
    if (t->cursor_x >= t->cols) {
        t->cursor_x = 0;
        t->cursor_y++;
    }
    if (t->cursor_y >= t->rows) {
        terminal_scroll_up(t);
        t->cursor_y = t->rows - 1;
    }
    TermCell* row = term_screen_row(t, t->cursor_y);
    row[t->cursor_x].ch = ch;
    row[t->cursor_x].fg = t->current_fg;
    row[t->cursor_x].bg = t->current_bg;
    t->cursor_x++;
}

static inline void terminal_enter_alt_screen(TerminalState* t) {
    if (t->alt_screen_active) return;
    t->alt_screen_active = true;
    t->dirty = true;
    // Save cursor
    t->saved_cursor_x = t->cursor_x;
    t->saved_cursor_y = t->cursor_y;
    // Save visible screen to alt_cells, clear visible screen
    int total = t->cols * t->rows;
    TermCell* screen = term_screen_row(t, 0);
    for (int i = 0; i < total; i++) {
        t->alt_cells[i] = screen[i];
        screen[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    }
    t->view_offset = 0;
    t->cursor_x = 0;
    t->cursor_y = 0;
}

static inline void terminal_exit_alt_screen(TerminalState* t) {
    if (!t->alt_screen_active) return;
    t->alt_screen_active = false;
    t->dirty = true;
    // Restore visible screen from alt_cells
    int total = t->cols * t->rows;
    TermCell* screen = term_screen_row(t, 0);
    for (int i = 0; i < total; i++) {
        screen[i] = t->alt_cells[i];
    }
    // Restore cursor
    t->cursor_x = t->saved_cursor_x;
    t->cursor_y = t->saved_cursor_y;
}

static inline void terminal_process_private_mode(TerminalState* t, char cmd) {
    int p0 = t->csi_param_count > 0 ? t->csi_params[0] : 0;

    if (cmd == 'h') {
        // Set private mode
        if (p0 == 25) {
            t->cursor_visible = true;
        } else if (p0 == 1049) {
            terminal_enter_alt_screen(t);
        }
    } else if (cmd == 'l') {
        // Reset private mode
        if (p0 == 25) {
            t->cursor_visible = false;
        } else if (p0 == 1049) {
            terminal_exit_alt_screen(t);
        }
    }
}

static inline void terminal_process_csi(TerminalState* t, char cmd) {
    // Finalize current param
    if (t->csi_param_count < 8) {
        t->csi_params[t->csi_param_count] = t->csi_current_param;
        t->csi_param_count++;
    }

    // Handle private mode sequences (ESC[?...)
    if (t->csi_private) {
        terminal_process_private_mode(t, cmd);
        return;
    }

    int p0 = t->csi_param_count > 0 ? t->csi_params[0] : 0;
    int p1 = t->csi_param_count > 1 ? t->csi_params[1] : 0;

    switch (cmd) {
    case 'H': case 'f': {
        // Cursor position: ESC[row;colH (1-based)
        int row = (p0 > 0 ? p0 : 1) - 1;
        int col = (p1 > 0 ? p1 : 1) - 1;
        if (row < 0) row = 0;
        if (row >= t->rows) row = t->rows - 1;
        if (col < 0) col = 0;
        if (col >= t->cols) col = t->cols - 1;
        t->cursor_y = row;
        t->cursor_x = col;
        break;
    }
    case 'A': {
        // Cursor up
        int n = p0 > 0 ? p0 : 1;
        t->cursor_y -= n;
        if (t->cursor_y < 0) t->cursor_y = 0;
        break;
    }
    case 'B': {
        // Cursor down
        int n = p0 > 0 ? p0 : 1;
        t->cursor_y += n;
        if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
        break;
    }
    case 'C': {
        // Cursor forward
        int n = p0 > 0 ? p0 : 1;
        t->cursor_x += n;
        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
        break;
    }
    case 'D': {
        // Cursor backward
        int n = p0 > 0 ? p0 : 1;
        t->cursor_x -= n;
        if (t->cursor_x < 0) t->cursor_x = 0;
        break;
    }
    case 'J': {
        // Erase in display
        if (p0 == 0) {
            // Clear from cursor to end
            TermCell* row = term_screen_row(t, t->cursor_y);
            for (int x = t->cursor_x; x < t->cols; x++)
                row[x] = {' ', t->current_fg, colors::TERM_BG};
            for (int r = t->cursor_y + 1; r < t->rows; r++) {
                TermCell* rp = term_screen_row(t, r);
                for (int c = 0; c < t->cols; c++)
                    rp[c] = {' ', t->current_fg, colors::TERM_BG};
            }
        } else if (p0 == 1) {
            // Clear from start to cursor
            for (int r = 0; r < t->cursor_y; r++) {
                TermCell* rp = term_screen_row(t, r);
                for (int c = 0; c < t->cols; c++)
                    rp[c] = {' ', t->current_fg, colors::TERM_BG};
            }
            TermCell* row = term_screen_row(t, t->cursor_y);
            for (int x = 0; x <= t->cursor_x; x++)
                row[x] = {' ', t->current_fg, colors::TERM_BG};
        } else if (p0 == 2) {
            // Clear entire screen
            for (int r = 0; r < t->rows; r++) {
                TermCell* rp = term_screen_row(t, r);
                for (int c = 0; c < t->cols; c++)
                    rp[c] = {' ', t->current_fg, colors::TERM_BG};
            }
            t->cursor_x = 0;
            t->cursor_y = 0;
        }
        break;
    }
    case 'K': {
        // Erase in line
        int start = 0, end = t->cols;
        if (p0 == 0) { start = t->cursor_x; end = t->cols; }
        else if (p0 == 1) { start = 0; end = t->cursor_x + 1; }
        else if (p0 == 2) { start = 0; end = t->cols; }
        TermCell* row = term_screen_row(t, t->cursor_y);
        for (int x = start; x < end; x++)
            row[x] = {' ', t->current_fg, colors::TERM_BG};
        break;
    }
    case 'm': {
        // SGR - Set Graphics Rendition
        for (int i = 0; i < t->csi_param_count; i++) {
            int code = t->csi_params[i];
            if (code == 0) {
                t->current_fg = colors::TERM_FG;
                t->current_bg = colors::TERM_BG;
                t->reverse_video = false;
            } else if (code == 1) {
                // Bold: map to bright version of current color
                uint8_t r = t->current_fg.r;
                uint8_t g = t->current_fg.g;
                uint8_t b = t->current_fg.b;
                int add = 50;
                r = (r + add > 255) ? 255 : r + add;
                g = (g + add > 255) ? 255 : g + add;
                b = (b + add > 255) ? 255 : b + add;
                t->current_fg = Color::from_rgb(r, g, b);
            } else if (code == 2) {
                // Dim: darken current fg color
                t->current_fg.r = t->current_fg.r / 2;
                t->current_fg.g = t->current_fg.g / 2;
                t->current_fg.b = t->current_fg.b / 2;
            } else if (code == 7) {
                // Reverse video
                if (!t->reverse_video) {
                    t->reverse_video = true;
                    Color tmp = t->current_fg;
                    t->current_fg = t->current_bg;
                    t->current_bg = tmp;
                }
            } else if (code == 27) {
                // Reverse off
                if (t->reverse_video) {
                    t->reverse_video = false;
                    Color tmp = t->current_fg;
                    t->current_fg = t->current_bg;
                    t->current_bg = tmp;
                }
            } else if (code >= 30 && code <= 37) {
                t->current_fg = term_ansi_color(code - 30);
                if (t->reverse_video) {
                    // In reverse mode, fg is displayed as bg
                    Color tmp = t->current_fg;
                    t->current_fg = t->current_bg;
                    t->current_bg = tmp;
                }
            } else if (code >= 40 && code <= 47) {
                t->current_bg = term_ansi_color(code - 40);
            } else if (code >= 90 && code <= 97) {
                t->current_fg = term_ansi_color(code - 90 + 8);
            } else if (code >= 100 && code <= 107) {
                t->current_bg = term_ansi_color(code - 100 + 8);
            } else if (code == 39) {
                t->current_fg = colors::TERM_FG;
            } else if (code == 49) {
                t->current_bg = colors::TERM_BG;
            }
        }
        if (t->csi_param_count == 0) {
            // ESC[m with no params = reset
            t->current_fg = colors::TERM_FG;
            t->current_bg = colors::TERM_BG;
            t->reverse_video = false;
        }
        break;
    }
    default:
        break;
    }
}

static inline void terminal_feed(TerminalState* t, const char* data, int len) {
    if (len > 0) t->dirty = true;
    for (int i = 0; i < len; i++) {
        char ch = data[i];

        switch (t->parse_state) {
        case TerminalState::STATE_NORMAL:
            if (ch == '\033') {
                t->parse_state = TerminalState::STATE_ESC;
            } else if (ch == '\n') {
                t->cursor_x = 0;  // CR+LF: shell sends \n without \r
                t->cursor_y++;
                if (t->cursor_y >= t->rows) {
                    terminal_scroll_up(t);
                    t->cursor_y = t->rows - 1;
                }
            } else if (ch == '\r') {
                t->cursor_x = 0;
            } else if (ch == '\b') {
                if (t->cursor_x > 0) t->cursor_x--;
            } else if (ch == '\t') {
                int next = (t->cursor_x + 8) & ~7;
                if (next > t->cols) next = t->cols;
                while (t->cursor_x < next) {
                    terminal_put_char(t, ' ');
                }
            } else if (ch >= 32 || ch < 0) {
                // Printable character (also treat high-bit chars as printable)
                terminal_put_char(t, ch);
            }
            break;

        case TerminalState::STATE_ESC:
            if (ch == '[') {
                t->parse_state = TerminalState::STATE_CSI;
                t->csi_private = false;
                t->csi_param_count = 0;
                t->csi_current_param = 0;
                for (int j = 0; j < 8; j++) t->csi_params[j] = 0;
            } else if (ch == 'c') {
                // Reset terminal
                t->current_fg = colors::TERM_FG;
                t->current_bg = colors::TERM_BG;
                t->cursor_x = 0;
                t->cursor_y = 0;
                t->parse_state = TerminalState::STATE_NORMAL;
            } else {
                // Unknown ESC sequence, ignore
                t->parse_state = TerminalState::STATE_NORMAL;
            }
            break;

        case TerminalState::STATE_CSI:
            if (ch >= '0' && ch <= '9') {
                t->csi_current_param = t->csi_current_param * 10 + (ch - '0');
            } else if (ch == ';') {
                if (t->csi_param_count < 8) {
                    t->csi_params[t->csi_param_count] = t->csi_current_param;
                    t->csi_param_count++;
                }
                t->csi_current_param = 0;
            } else if (ch == '?') {
                t->csi_private = true;
            } else if (ch >= 0x40 && ch <= 0x7E) {
                // Final byte - execute command
                terminal_process_csi(t, ch);
                t->parse_state = TerminalState::STATE_NORMAL;
            } else {
                // Unknown, abort CSI
                t->parse_state = TerminalState::STATE_NORMAL;
            }
            break;
        }
    }
}

static inline void terminal_render(TerminalState* t, uint32_t* pixels, int pw, int ph) {
    if (!t->dirty || !t->cells) return;
    t->dirty = false;

    int cell_w = mono_cell_width();
    int cell_h = mono_cell_height();
    bool use_ttf = fonts::mono && fonts::mono->valid;
    GlyphCache* gc = use_ttf ? fonts::mono->get_cache(fonts::TERM_SIZE) : nullptr;

    // Fill background using row-copy: fill first row, then memcpy to the rest
    uint32_t bg_px = colors::TERM_BG.to_pixel();
    int row_bytes = pw * sizeof(uint32_t);
    for (int i = 0; i < pw; i++) pixels[i] = bg_px;
    for (int r = 1; r < ph; r++) {
        montauk::memcpy(&pixels[r * pw], pixels, row_bytes);
    }

    // Determine which rows of the buffer to display
    int base_row = t->scrollback_lines - t->view_offset;
    if (base_row < 0) base_row = 0;

    // Render each visible cell
    int visible_rows = ph / cell_h;
    int visible_cols = pw / cell_w;
    if (visible_rows > t->rows) visible_rows = t->rows;
    if (visible_cols > t->cols) visible_cols = t->cols;

    for (int r = 0; r < visible_rows; r++) {
        int py = r * cell_h;
        int src_row = base_row + r;
        for (int c = 0; c < visible_cols; c++) {
            int idx = src_row * t->cols + c;
            TermCell& cell = t->cells[idx];

            int px = c * cell_w;

            // Only draw cell background if it differs from terminal bg
            uint32_t cell_bg = cell.bg.to_pixel();
            if (cell_bg != bg_px) {
                for (int fy = 0; fy < cell_h && py + fy < ph; fy++) {
                    uint32_t* row = &pixels[(py + fy) * pw + px];
                    for (int fx = 0; fx < cell_w && px + fx < pw; fx++) {
                        row[fx] = cell_bg;
                    }
                }
            }

            // Draw character glyph
            if (cell.ch > 32 || cell.ch < 0) {
                if (use_ttf) {
                    int baseline = py + gc->ascent;
                    fonts::mono->draw_char_to_buffer(pixels, pw, ph,
                        px, baseline, (unsigned char)cell.ch, cell.fg, gc);
                } else {
                    uint32_t cell_fg = cell.fg.to_pixel();
                    const uint8_t* glyph = &font_data[(unsigned char)cell.ch * FONT_HEIGHT];
                    for (int fy = 0; fy < FONT_HEIGHT; fy++) {
                        int dy = py + fy;
                        if (dy >= ph) break;
                        uint8_t bits = glyph[fy];
                        for (int fx = 0; fx < FONT_WIDTH; fx++) {
                            if (bits & (0x80 >> fx)) {
                                int dx = px + fx;
                                if (dx >= pw) break;
                                pixels[dy * pw + dx] = cell_fg;
                            }
                        }
                    }
                }
            }
        }
    }

    // Draw cursor (only when viewing live position)
    if (t->view_offset == 0 && t->cursor_visible &&
        t->cursor_x < visible_cols && t->cursor_y < visible_rows) {
        int cx = t->cursor_x * cell_w;
        int cy = t->cursor_y * cell_h;
        uint32_t cursor_px = colors::WHITE.to_pixel();
        for (int fy = 0; fy < cell_h; fy++) {
            int dy = cy + fy;
            if (dy >= ph) break;
            for (int fx = 0; fx < cell_w; fx++) {
                int dx = cx + fx;
                if (dx >= pw) break;
                pixels[dy * pw + dx] = cursor_px;
            }
        }
        // Draw character on top of cursor in black
        if (t->cursor_y < t->rows && t->cursor_x < t->cols) {
            TermCell* row = term_screen_row(t, t->cursor_y);
            char ch = row[t->cursor_x].ch;
            if (ch > 32 || ch < 0) {
                if (use_ttf) {
                    int baseline = cy + gc->ascent;
                    fonts::mono->draw_char_to_buffer(pixels, pw, ph,
                        cx, baseline, (unsigned char)ch, colors::BLACK, gc);
                } else {
                    const uint8_t* glyph = &font_data[(unsigned char)ch * FONT_HEIGHT];
                    uint32_t black_px = colors::BLACK.to_pixel();
                    for (int fy = 0; fy < FONT_HEIGHT; fy++) {
                        int dy = cy + fy;
                        if (dy >= ph) break;
                        uint8_t bits = glyph[fy];
                        for (int fx = 0; fx < FONT_WIDTH; fx++) {
                            if (bits & (0x80 >> fx)) {
                                int dx = cx + fx;
                                if (dx >= pw) break;
                                pixels[dy * pw + dx] = black_px;
                            }
                        }
                    }
                }
            }
        }
    }
}

static inline void terminal_resize(TerminalState* t, int new_cols, int new_rows) {
    if (new_cols == t->cols && new_rows == t->rows) return;
    if (new_cols < 1 || new_rows < 1) return;
    t->dirty = true;

    int new_capacity = new_rows + t->max_scrollback;
    int new_total = new_capacity * new_cols;
    TermCell* new_cells = (TermCell*)montauk::alloc(new_total * sizeof(TermCell));
    TermCell* new_alt = (TermCell*)montauk::alloc(new_rows * new_cols * sizeof(TermCell));
    if (!new_cells || !new_alt) {
        if (new_cells) montauk::free(new_cells);
        if (new_alt) montauk::free(new_alt);
        return;  // keep existing buffers
    }

    // Clear new buffers
    for (int i = 0; i < new_total; i++)
        new_cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    for (int i = 0; i < new_rows * new_cols; i++)
        new_alt[i] = {' ', colors::TERM_FG, colors::TERM_BG};

    // Copy content: scrollback + visible screen
    int old_content = t->scrollback_lines + t->rows;
    int keep = old_content < new_capacity ? old_content : new_capacity;
    int discard = old_content - keep;
    int copy_cols = t->cols < new_cols ? t->cols : new_cols;

    for (int r = 0; r < keep; r++) {
        for (int c = 0; c < copy_cols; c++) {
            new_cells[r * new_cols + c] = t->cells[(discard + r) * t->cols + c];
        }
    }

    int new_scrollback = keep - new_rows;
    if (new_scrollback < 0) new_scrollback = 0;

    // Adjust cursor
    int abs_cursor_y = t->scrollback_lines + t->cursor_y - discard;
    int new_cursor_y = abs_cursor_y - new_scrollback;
    if (new_cursor_y < 0) new_cursor_y = 0;
    if (new_cursor_y >= new_rows) new_cursor_y = new_rows - 1;
    int new_cursor_x = t->cursor_x < new_cols ? t->cursor_x : new_cols - 1;

    if (t->cells) montauk::free(t->cells);
    if (t->alt_cells) montauk::free(t->alt_cells);

    t->cells = new_cells;
    t->alt_cells = new_alt;
    t->cols = new_cols;
    t->rows = new_rows;
    t->scrollback_lines = new_scrollback;
    t->cursor_x = new_cursor_x;
    t->cursor_y = new_cursor_y;

    // Clamp view offset
    if (t->view_offset > t->scrollback_lines)
        t->view_offset = t->scrollback_lines;

    // Notify child process of new terminal size
    if (t->child_pid > 0) {
        montauk::childio_settermsz(t->child_pid, new_cols, new_rows);
    }
}

static inline void terminal_handle_key(TerminalState* t, const Montauk::KeyEvent& key) {
    // Snap to live on any keyboard input
    if (t->view_offset > 0) {
        t->view_offset = 0;
        t->dirty = true;
    }
    if (t->child_pid > 0) {
        montauk::childio_writekey(t->child_pid, &key);
    }
}

// Returns false if the child process has exited
static inline bool terminal_poll(TerminalState* t) {
    if (t->child_pid <= 0) return false;
    char buf[4096];
    // Drain all available data so large output renders in one frame
    for (;;) {
        int n = montauk::childio_read(t->child_pid, buf, sizeof(buf));
        if (n > 0) {
            terminal_feed(t, buf, n);
        } else {
            // n == -1 means child process is gone; n == 0 means no data yet
            if (n < 0) { t->child_pid = 0; return false; }
            break;
        }
    }
    return true;
}

} // namespace gui
