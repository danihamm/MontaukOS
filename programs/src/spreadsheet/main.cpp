/*
 * main.cpp
 * Spreadsheet app — global state, hit testing, entry point
 * Copyright (c) 2026 Daniel Hammer
 */

#include "spreadsheet.h"

// ============================================================================
// Global state definitions
// ============================================================================

int g_win_w = INIT_W;
int g_win_h = INIT_H;

Cell g_cells[MAX_ROWS][MAX_COLS];

int g_sel_col = 0;
int g_sel_row = 0;

int g_scroll_x = 0;
int g_scroll_y = 0;

int g_col_widths[MAX_COLS];

bool g_editing = false;
char g_edit_buf[CELL_TEXT_MAX];
int  g_edit_len = 0;
int  g_edit_cursor = 0;

bool g_has_selection = false;
int  g_anchor_col = 0;
int  g_anchor_row = 0;

ClipCell g_clipboard[CLIP_MAX_CELLS];
int  g_clip_count = 0;
int  g_clip_cols = 0;
int  g_clip_rows = 0;

char g_filepath[256] = {};
bool g_modified = false;

bool g_pathbar_open = false;
bool g_pathbar_save = false;
char g_pathbar_text[256] = {};
int  g_pathbar_len = 0;
int  g_pathbar_cursor = 0;

TrueTypeFont* g_font = nullptr;
TrueTypeFont* g_font_bold = nullptr;

bool g_fmt_dropdown_open = false;

bool g_col_resizing = false;
int  g_col_resize_idx = -1;
int  g_col_resize_start_x = 0;
int  g_col_resize_start_w = 0;

UndoEntry* g_undo[UNDO_MAX + 1];
int g_undo_count = 0;
int g_undo_pos = 0;

// ============================================================================
// Hit testing
// ============================================================================

bool hit_cell(int mx, int my, int* out_col, int* out_row) {
    int pbh = g_pathbar_open ? PATHBAR_H : 0;
    int grid_y = TOOLBAR_H + pbh + FORMULA_BAR_H + COL_HEADER_H;
    if (mx < ROW_HEADER_W || my < grid_y) return false;

    int content_x = mx + g_scroll_x;
    int content_y = my - grid_y + g_scroll_y;

    *out_row = content_y / ROW_H;
    if (*out_row < 0) *out_row = 0;
    if (*out_row >= MAX_ROWS) *out_row = MAX_ROWS - 1;

    int x = ROW_HEADER_W;
    for (int c = 0; c < MAX_COLS; c++) {
        if (content_x >= x - ROW_HEADER_W && content_x < x - ROW_HEADER_W + g_col_widths[c]) {
            *out_col = c;
            return true;
        }
        x += g_col_widths[c];
    }

    *out_col = MAX_COLS - 1;
    return true;
}

bool handle_toolbar_click(int mx, int my) {
    if (my >= TOOLBAR_H || my < TB_BTN_Y || my >= TB_BTN_Y + TB_BTN_SIZE) return false;

    if (g_fmt_dropdown_open && !(mx >= TB_FMT_X0 && mx < TB_FMT_X1)) {
        g_fmt_dropdown_open = false;
    }

    if (mx >= TB_OPEN_X0 && mx < TB_OPEN_X1) {
        if (g_editing) commit_edit();
        g_pathbar_open = true;
        g_pathbar_save = false;
        g_pathbar_text[0] = '\0';
        g_pathbar_len = 0;
        g_pathbar_cursor = 0;
    } else if (mx >= TB_SAVE_X0 && mx < TB_SAVE_X1) {
        if (g_editing) commit_edit();
        if (g_filepath[0]) {
            save_file();
        } else {
            g_pathbar_open = true;
            g_pathbar_save = true;
            str_cpy(g_pathbar_text, "0:/", 256);
            g_pathbar_len = str_len(g_pathbar_text);
            g_pathbar_cursor = g_pathbar_len;
        }
    } else if (mx >= TB_CUT_X0 && mx < TB_CUT_X1) {
        if (!g_editing) cut_selection();
    } else if (mx >= TB_COPY_X0 && mx < TB_COPY_X1) {
        if (!g_editing) copy_selection();
    } else if (mx >= TB_PASTE_X0 && mx < TB_PASTE_X1) {
        if (!g_editing) paste_at_cursor();
    } else if (mx >= TB_BOLD_X0 && mx < TB_BOLD_X1) {
        if (!g_editing) apply_bold_toggle();
    } else if (mx >= TB_AL_X0 && mx < TB_AL_X1) {
        if (!g_editing) apply_align(ALIGN_LEFT);
    } else if (mx >= TB_AC_X0 && mx < TB_AC_X1) {
        if (!g_editing) apply_align(ALIGN_CENTER);
    } else if (mx >= TB_AR_X0 && mx < TB_AR_X1) {
        if (!g_editing) apply_align(ALIGN_RIGHT);
    } else if (mx >= TB_FMT_X0 && mx < TB_FMT_X1) {
        g_fmt_dropdown_open = !g_fmt_dropdown_open;
    } else if (mx >= TB_UNDO_X0 && mx < TB_UNDO_X1) {
        undo_do();
    } else if (mx >= TB_REDO_X0 && mx < TB_REDO_X1) {
        redo_do();
    } else {
        return false;
    }
    return true;
}

bool handle_fmt_dropdown_click(int mx, int my) {
    if (!g_fmt_dropdown_open) return false;
    int dx = TB_FMT_X0;
    int dy = TOOLBAR_H;
    int dw = 80;
    int item_h = 26;
    int item_count = 4;
    int dh = item_count * item_h + 4;

    if (mx >= dx && mx < dx + dw && my >= dy && my < dy + dh) {
        int idx = (my - dy - 2) / item_h;
        if (idx >= 0 && idx < item_count) {
            apply_format((NumFormat)idx);
        }
        g_fmt_dropdown_open = false;
        return true;
    }
    g_fmt_dropdown_open = false;
    return true;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Initialize column widths
    for (int i = 0; i < MAX_COLS; i++)
        g_col_widths[i] = DEF_COL_W;

    // Initialize cells
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            g_cells[r][c].input[0] = '\0';
            g_cells[r][c].display[0] = '\0';
            g_cells[r][c].value = 0;
            g_cells[r][c].type = CT_EMPTY;
            g_cells[r][c].align = ALIGN_AUTO;
            g_cells[r][c].fmt = FMT_AUTO;
            g_cells[r][c].bold = false;
        }

    // Initialize undo pointers
    for (int i = 0; i <= UNDO_MAX; i++) g_undo[i] = nullptr;
    g_undo_count = 0;
    g_undo_pos = 0;

    // Load font
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_font = load_font("0:/fonts/Roboto-Medium.ttf");
    g_font_bold = load_font("0:/fonts/Roboto-Bold.ttf");

    // Check for file argument
    char args[512] = {};
    int arglen = montauk::getargs(args, sizeof(args));
    if (arglen > 0 && args[0]) {
        str_cpy(g_filepath, args, 256);
        load_file(g_filepath);
    }

    // Build window title
    char title[64] = "Spreadsheet";
    if (g_filepath[0]) {
        const char* fname = g_filepath;
        for (int i = 0; g_filepath[i]; i++)
            if (g_filepath[i] == '/') fname = g_filepath + i + 1;
        snprintf(title, 64, "%s - Spreadsheet", fname);
    }

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create(title, INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    render(pixels);
    montauk::win_present(win_id);

    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;

        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        // Close
        if (ev.type == 3) break;

        // Resize
        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            clamp_scroll();
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        bool redraw = false;

        // Keyboard
        if (ev.type == 0 && ev.key.pressed) {
            auto& key = ev.key;

            // Path bar input (intercepts all keys when open)
            if (g_pathbar_open) {
                if (key.ascii == '\n' || key.ascii == '\r') {
                    if (g_pathbar_text[0]) {
                        if (g_pathbar_save) {
                            str_cpy(g_filepath, g_pathbar_text, 256);
                            save_file();
                        } else {
                            load_file(g_pathbar_text);
                        }
                    }
                    g_pathbar_open = false;
                } else if (key.scancode == 0x01) {
                    g_pathbar_open = false;
                } else if (key.ascii == '\b' || key.scancode == 0x0E) {
                    if (g_pathbar_cursor > 0) {
                        for (int i = g_pathbar_cursor - 1; i < g_pathbar_len - 1; i++)
                            g_pathbar_text[i] = g_pathbar_text[i + 1];
                        g_pathbar_len--;
                        g_pathbar_cursor--;
                        g_pathbar_text[g_pathbar_len] = '\0';
                    }
                } else if (key.scancode == 0x4B) {
                    if (g_pathbar_cursor > 0) g_pathbar_cursor--;
                } else if (key.scancode == 0x4D) {
                    if (g_pathbar_cursor < g_pathbar_len) g_pathbar_cursor++;
                } else if (key.ascii >= 32 && key.ascii < 127 && g_pathbar_len < 254) {
                    for (int i = g_pathbar_len; i > g_pathbar_cursor; i--)
                        g_pathbar_text[i] = g_pathbar_text[i - 1];
                    g_pathbar_text[g_pathbar_cursor] = key.ascii;
                    g_pathbar_cursor++;
                    g_pathbar_len++;
                    g_pathbar_text[g_pathbar_len] = '\0';
                }
                redraw = true;
                goto done_keys;
            }

            // Escape: cancel edit or quit
            if (key.scancode == 0x01) {
                if (g_editing) { cancel_edit(); redraw = true; }
                else break;
            }
            // Enter: commit edit and move down
            else if (key.ascii == '\n' || key.ascii == '\r') {
                if (g_editing) commit_edit();
                if (g_sel_row < MAX_ROWS - 1) g_sel_row++;
                ensure_sel_visible();
                redraw = true;
            }
            // Tab: commit and move right
            else if (key.ascii == '\t') {
                if (g_editing) commit_edit();
                if (key.shift) {
                    if (g_sel_col > 0) g_sel_col--;
                } else {
                    if (g_sel_col < MAX_COLS - 1) g_sel_col++;
                }
                ensure_sel_visible();
                redraw = true;
            }
            // Arrow keys (with Shift = extend selection)
            else if (key.scancode == 0x48 && !g_editing) { // Up
                if (key.shift) {
                    if (!g_has_selection) { g_anchor_col = g_sel_col; g_anchor_row = g_sel_row; g_has_selection = true; }
                    if (g_sel_row > 0) g_sel_row--;
                } else {
                    if (g_sel_row > 0) g_sel_row--;
                    clear_selection();
                }
                ensure_sel_visible();
                redraw = true;
            }
            else if (key.scancode == 0x50 && !g_editing) { // Down
                if (key.shift) {
                    if (!g_has_selection) { g_anchor_col = g_sel_col; g_anchor_row = g_sel_row; g_has_selection = true; }
                    if (g_sel_row < MAX_ROWS - 1) g_sel_row++;
                } else {
                    if (g_sel_row < MAX_ROWS - 1) g_sel_row++;
                    clear_selection();
                }
                ensure_sel_visible();
                redraw = true;
            }
            else if (key.scancode == 0x4B && !g_editing) { // Left
                if (key.shift) {
                    if (!g_has_selection) { g_anchor_col = g_sel_col; g_anchor_row = g_sel_row; g_has_selection = true; }
                    if (g_sel_col > 0) g_sel_col--;
                } else {
                    if (g_sel_col > 0) g_sel_col--;
                    clear_selection();
                }
                ensure_sel_visible();
                redraw = true;
            }
            else if (key.scancode == 0x4D && !g_editing) { // Right
                if (key.shift) {
                    if (!g_has_selection) { g_anchor_col = g_sel_col; g_anchor_row = g_sel_row; g_has_selection = true; }
                    if (g_sel_col < MAX_COLS - 1) g_sel_col++;
                } else {
                    if (g_sel_col < MAX_COLS - 1) g_sel_col++;
                    clear_selection();
                }
                ensure_sel_visible();
                redraw = true;
            }
            // Backspace
            else if (key.ascii == '\b' || key.scancode == 0x0E) {
                if (g_editing) {
                    if (g_edit_cursor > 0) {
                        for (int i = g_edit_cursor - 1; i < g_edit_len - 1; i++)
                            g_edit_buf[i] = g_edit_buf[i + 1];
                        g_edit_len--;
                        g_edit_cursor--;
                        g_edit_buf[g_edit_len] = '\0';
                    }
                    redraw = true;
                } else {
                    undo_push();
                    int c0, r0, c1, r1;
                    sel_range(&c0, &r0, &c1, &r1);
                    for (int r = r0; r <= r1; r++)
                        for (int c = c0; c <= c1; c++)
                            g_cells[r][c].input[0] = '\0';
                    eval_all_cells();
                    g_modified = true;
                    redraw = true;
                }
            }
            // Delete key
            else if (key.scancode == 0x53) {
                if (g_editing) {
                    if (g_edit_cursor < g_edit_len) {
                        for (int i = g_edit_cursor; i < g_edit_len - 1; i++)
                            g_edit_buf[i] = g_edit_buf[i + 1];
                        g_edit_len--;
                        g_edit_buf[g_edit_len] = '\0';
                    }
                    redraw = true;
                } else {
                    undo_push();
                    int c0, r0, c1, r1;
                    sel_range(&c0, &r0, &c1, &r1);
                    for (int r = r0; r <= r1; r++)
                        for (int c = c0; c <= c1; c++)
                            g_cells[r][c].input[0] = '\0';
                    eval_all_cells();
                    g_modified = true;
                    redraw = true;
                }
            }
            // Edit mode arrow keys (cursor movement within formula bar)
            else if (key.scancode == 0x4B && g_editing) {
                if (g_edit_cursor > 0) g_edit_cursor--;
                redraw = true;
            }
            else if (key.scancode == 0x4D && g_editing) {
                if (g_edit_cursor < g_edit_len) g_edit_cursor++;
                redraw = true;
            }
            // Ctrl+B: bold toggle
            else if (key.ctrl && (key.ascii == 'b' || key.ascii == 'B' || key.ascii == 2) && !g_editing) {
                apply_bold_toggle();
                redraw = true;
            }
            // Ctrl+Z: undo
            else if (key.ctrl && (key.ascii == 'z' || key.ascii == 'Z' || key.ascii == 26) && !g_editing) {
                undo_do();
                redraw = true;
            }
            // Ctrl+Y: redo
            else if (key.ctrl && (key.ascii == 'y' || key.ascii == 'Y' || key.ascii == 25) && !g_editing) {
                redo_do();
                redraw = true;
            }
            // Ctrl+C: copy
            else if (key.ctrl && (key.ascii == 'c' || key.ascii == 'C' || key.ascii == 3) && !g_editing) {
                copy_selection();
                redraw = true;
            }
            // Ctrl+X: cut
            else if (key.ctrl && (key.ascii == 'x' || key.ascii == 'X' || key.ascii == 24) && !g_editing) {
                cut_selection();
                redraw = true;
            }
            // Ctrl+V: paste
            else if (key.ctrl && (key.ascii == 'v' || key.ascii == 'V' || key.ascii == 22) && !g_editing) {
                paste_at_cursor();
                redraw = true;
            }
            // Ctrl+S: save (or Save As if no path)
            else if (key.ctrl && (key.ascii == 's' || key.ascii == 'S' || key.ascii == 19)) {
                if (g_editing) commit_edit();
                if (g_filepath[0]) {
                    save_file();
                } else {
                    g_pathbar_open = true;
                    g_pathbar_save = true;
                    str_cpy(g_pathbar_text, "0:/", 256);
                    g_pathbar_len = str_len(g_pathbar_text);
                    g_pathbar_cursor = g_pathbar_len;
                }
                redraw = true;
            }
            // Ctrl+O: open
            else if (key.ctrl && (key.ascii == 'o' || key.ascii == 'O' || key.ascii == 15) && !g_editing) {
                g_pathbar_open = true;
                g_pathbar_save = false;
                g_pathbar_text[0] = '\0';
                g_pathbar_len = 0;
                g_pathbar_cursor = 0;
                redraw = true;
            }
            // Printable character: start editing or insert
            else if (key.ascii >= 32 && key.ascii < 127 && !key.ctrl) {
                if (!g_editing) {
                    clear_selection();
                    g_editing = true;
                    g_edit_buf[0] = key.ascii;
                    g_edit_buf[1] = '\0';
                    g_edit_len = 1;
                    g_edit_cursor = 1;
                } else if (g_edit_len < CELL_TEXT_MAX - 1) {
                    for (int i = g_edit_len; i > g_edit_cursor; i--)
                        g_edit_buf[i] = g_edit_buf[i - 1];
                    g_edit_buf[g_edit_cursor] = key.ascii;
                    g_edit_len++;
                    g_edit_cursor++;
                    g_edit_buf[g_edit_len] = '\0';
                }
                redraw = true;
            }
            // F2: edit current cell content (append mode)
            else if (key.scancode == 0x3C) {
                if (!g_editing) start_editing();
                redraw = true;
            }
        }
        done_keys:

        // Mouse
        if (ev.type == 1) {
            int mx = ev.mouse.x;
            int my = ev.mouse.y;
            uint8_t btns = ev.mouse.buttons;
            uint8_t prev = ev.mouse.prev_buttons;
            bool clicked = (btns & 1) && !(prev & 1);
            bool left_held = (btns & 1);
            bool left_released = !(btns & 1) && (prev & 1);

            int pbh = g_pathbar_open ? PATHBAR_H : 0;
            int header_y = TOOLBAR_H + pbh + FORMULA_BAR_H;

            // Update cursor style: resize_h when hovering near column border in header
            {
                int cursor = 0; // arrow
                if (g_col_resizing) {
                    cursor = 1; // resize_h while dragging
                } else if (my >= header_y && my < header_y + COL_HEADER_H) {
                    for (int c = 0; c < MAX_COLS; c++) {
                        int cx = col_x(c) - g_scroll_x + g_col_widths[c] - 1;
                        if (mx >= cx - COL_RESIZE_GRAB && mx <= cx + COL_RESIZE_GRAB) {
                            cursor = 1; // resize_h
                            break;
                        }
                    }
                }
                montauk::win_setcursor(win_id, cursor);
            }

            // Column resize: drag in progress
            if (g_col_resizing) {
                if (left_held) {
                    int delta = mx - g_col_resize_start_x;
                    int new_w = g_col_resize_start_w + delta;
                    if (new_w < MIN_COL_W) new_w = MIN_COL_W;
                    g_col_widths[g_col_resize_idx] = new_w;
                    clamp_scroll();
                    redraw = true;
                }
                if (left_released) {
                    g_col_resizing = false;
                }
                goto done_mouse;
            }

            // Column resize: start drag (click near right edge of column header)
            if (clicked && my >= header_y && my < header_y + COL_HEADER_H) {
                for (int c = 0; c < MAX_COLS; c++) {
                    int cx = col_x(c) - g_scroll_x + g_col_widths[c] - 1;
                    if (mx >= cx - COL_RESIZE_GRAB && mx <= cx + COL_RESIZE_GRAB) {
                        g_col_resizing = true;
                        g_col_resize_idx = c;
                        g_col_resize_start_x = mx;
                        g_col_resize_start_w = g_col_widths[c];
                        goto done_mouse;
                    }
                }
            }

            if (clicked) {
                if (handle_fmt_dropdown_click(mx, my)) {
                    redraw = true;
                }
                else if (handle_toolbar_click(mx, my)) {
                    redraw = true;
                }
                else {
                    int col, row;
                    if (hit_cell(mx, my, &col, &row)) {
                        if (g_editing) commit_edit();
                        g_sel_col = col;
                        g_sel_row = row;
                        clear_selection();
                        g_fmt_dropdown_open = false;
                        redraw = true;
                    }
                }
            }

            if (ev.mouse.scroll != 0) {
                g_scroll_y -= ev.mouse.scroll * SCROLL_STEP;
                clamp_scroll();
                redraw = true;
            }
        }
        done_mouse:

        if (redraw) {
            render(pixels);
            montauk::win_present(win_id);
        }
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
