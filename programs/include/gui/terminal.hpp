/*
    * terminal.hpp
    * ZenithOS terminal emulator with ANSI escape sequence support
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/font.hpp"
#include <zenith/syscall.h>
#include <zenith/string.h>
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
    int scroll_top;
    int total_rows;
    int child_pid;
    Color current_fg;
    Color current_bg;
    bool cursor_visible;
    bool alt_screen_active;
    bool reverse_video;

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

static inline void terminal_scroll_up(TerminalState* t) {
    // Move all rows up by one
    for (int r = 0; r < t->rows - 1; r++) {
        for (int c = 0; c < t->cols; c++) {
            t->cells[r * t->cols + c] = t->cells[(r + 1) * t->cols + c];
        }
    }
    // Clear last row
    int last = t->rows - 1;
    for (int c = 0; c < t->cols; c++) {
        t->cells[last * t->cols + c] = {' ', t->current_fg, colors::TERM_BG};
    }
}

static inline void terminal_init(TerminalState* t, int cols, int rows) {
    t->cols = cols;
    t->rows = rows;
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->saved_cursor_x = 0;
    t->saved_cursor_y = 0;
    t->scroll_top = 0;
    t->total_rows = rows;
    t->current_fg = colors::TERM_FG;
    t->current_bg = colors::TERM_BG;
    t->cursor_visible = true;
    t->alt_screen_active = false;
    t->reverse_video = false;
    t->parse_state = TerminalState::STATE_NORMAL;
    t->csi_private = false;
    t->csi_param_count = 0;
    t->csi_current_param = 0;

    int total_cells = cols * rows;
    t->cells = (TermCell*)zenith::alloc(total_cells * sizeof(TermCell));
    t->alt_cells = (TermCell*)zenith::alloc(total_cells * sizeof(TermCell));
    for (int i = 0; i < total_cells; i++) {
        t->cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
        t->alt_cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    }

    t->child_pid = zenith::spawn_redir("0:/os/shell.elf");
    zenith::childio_settermsz(t->child_pid, cols, rows);
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
    int idx = t->cursor_y * t->cols + t->cursor_x;
    t->cells[idx].ch = ch;
    t->cells[idx].fg = t->current_fg;
    t->cells[idx].bg = t->current_bg;
    t->cursor_x++;
}

static inline void terminal_enter_alt_screen(TerminalState* t) {
    if (t->alt_screen_active) return;
    t->alt_screen_active = true;
    // Save cursor
    t->saved_cursor_x = t->cursor_x;
    t->saved_cursor_y = t->cursor_y;
    // Swap buffers: save main screen to alt_cells, clear main
    int total = t->cols * t->rows;
    for (int i = 0; i < total; i++) {
        t->alt_cells[i] = t->cells[i];
        t->cells[i] = {' ', colors::TERM_FG, colors::TERM_BG};
    }
    t->cursor_x = 0;
    t->cursor_y = 0;
}

static inline void terminal_exit_alt_screen(TerminalState* t) {
    if (!t->alt_screen_active) return;
    t->alt_screen_active = false;
    // Restore main screen from alt_cells
    int total = t->cols * t->rows;
    for (int i = 0; i < total; i++) {
        t->cells[i] = t->alt_cells[i];
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
            for (int x = t->cursor_x; x < t->cols; x++)
                t->cells[t->cursor_y * t->cols + x] = {' ', t->current_fg, colors::TERM_BG};
            for (int r = t->cursor_y + 1; r < t->rows; r++)
                for (int c = 0; c < t->cols; c++)
                    t->cells[r * t->cols + c] = {' ', t->current_fg, colors::TERM_BG};
        } else if (p0 == 1) {
            // Clear from start to cursor
            for (int r = 0; r < t->cursor_y; r++)
                for (int c = 0; c < t->cols; c++)
                    t->cells[r * t->cols + c] = {' ', t->current_fg, colors::TERM_BG};
            for (int x = 0; x <= t->cursor_x; x++)
                t->cells[t->cursor_y * t->cols + x] = {' ', t->current_fg, colors::TERM_BG};
        } else if (p0 == 2) {
            // Clear entire screen
            for (int i = 0; i < t->rows * t->cols; i++)
                t->cells[i] = {' ', t->current_fg, colors::TERM_BG};
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
        for (int x = start; x < end; x++)
            t->cells[t->cursor_y * t->cols + x] = {' ', t->current_fg, colors::TERM_BG};
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
    // Fill background
    uint32_t bg_px = colors::TERM_BG.to_pixel();
    int total = pw * ph;
    for (int i = 0; i < total; i++) {
        pixels[i] = bg_px;
    }

    // Render each visible cell
    int visible_rows = ph / FONT_HEIGHT;
    int visible_cols = pw / FONT_WIDTH;
    if (visible_rows > t->rows) visible_rows = t->rows;
    if (visible_cols > t->cols) visible_cols = t->cols;

    for (int r = 0; r < visible_rows; r++) {
        for (int c = 0; c < visible_cols; c++) {
            int idx = r * t->cols + c;
            TermCell& cell = t->cells[idx];

            int px = c * FONT_WIDTH;
            int py = r * FONT_HEIGHT;

            uint32_t cell_bg = cell.bg.to_pixel();
            uint32_t cell_fg = cell.fg.to_pixel();

            // Draw cell background
            for (int fy = 0; fy < FONT_HEIGHT; fy++) {
                int dy = py + fy;
                if (dy >= ph) break;
                for (int fx = 0; fx < FONT_WIDTH; fx++) {
                    int dx = px + fx;
                    if (dx >= pw) break;
                    pixels[dy * pw + dx] = cell_bg;
                }
            }

            // Draw character glyph
            if (cell.ch > 32 || cell.ch < 0) {
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

    // Draw cursor
    if (t->cursor_visible && t->cursor_x < visible_cols && t->cursor_y < visible_rows) {
        int cx = t->cursor_x * FONT_WIDTH;
        int cy = t->cursor_y * FONT_HEIGHT;
        uint32_t cursor_px = colors::WHITE.to_pixel();
        for (int fy = 0; fy < FONT_HEIGHT; fy++) {
            int dy = cy + fy;
            if (dy >= ph) break;
            for (int fx = 0; fx < FONT_WIDTH; fx++) {
                int dx = cx + fx;
                if (dx >= pw) break;
                pixels[dy * pw + dx] = cursor_px;
            }
        }
        // Draw character on top of cursor in black
        if (t->cursor_y < t->rows && t->cursor_x < t->cols) {
            int idx = t->cursor_y * t->cols + t->cursor_x;
            char ch = t->cells[idx].ch;
            if (ch > 32 || ch < 0) {
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

static inline void terminal_handle_key(TerminalState* t, const Zenith::KeyEvent& key) {
    if (t->child_pid > 0) {
        zenith::childio_writekey(t->child_pid, &key);
    }
}

static inline void terminal_poll(TerminalState* t) {
    if (t->child_pid <= 0) return;
    char buf[512];
    int n = zenith::childio_read(t->child_pid, buf, sizeof(buf));
    if (n > 0) {
        terminal_feed(t, buf, n);
    }
}

} // namespace gui
