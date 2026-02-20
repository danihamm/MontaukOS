/*
    * app_calculator.cpp
    * ZenithOS Desktop - Calculator application
    * Integer-only 4-function calculator (values scaled by 100 for 2 decimal places)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Calculator state
// ============================================================================

struct CalcState {
    int64_t display_val;    // current display value * 100
    int64_t accumulator;    // stored accumulator * 100
    char pending_op;        // '+', '-', '*', '/', or 0
    bool start_new;         // next digit starts a new number
    bool has_decimal;       // decimal point pressed
    int decimal_digits;     // number of decimal digits entered
    char display_str[32];   // formatted display string
};

static constexpr int CALC_DISPLAY_H = 56;
static constexpr int CALC_BTN_W = 52;
static constexpr int CALC_BTN_H = 40;
static constexpr int CALC_BTN_PAD = 4;

// ============================================================================
// Display formatting
// ============================================================================

static void calc_format_display(CalcState* cs) {
    int64_t val = cs->display_val;
    bool neg = val < 0;
    if (neg) val = -val;

    int64_t integer = val / 100;
    int64_t frac = val % 100;

    if (cs->has_decimal || frac != 0) {
        // Show decimal
        char int_buf[20];
        int pos = 0;

        if (integer == 0) {
            int_buf[pos++] = '0';
        } else {
            char tmp[20]; int ti = 0;
            int64_t v = integer;
            while (v > 0) { tmp[ti++] = '0' + (int)(v % 10); v /= 10; }
            while (ti > 0) int_buf[pos++] = tmp[--ti];
        }
        int_buf[pos] = '\0';

        if (neg) {
            snprintf(cs->display_str, 32, "-%s.%02d", int_buf, (int)frac);
        } else {
            snprintf(cs->display_str, 32, "%s.%02d", int_buf, (int)frac);
        }

        // Trim trailing zeros after decimal point (unless user is entering decimals)
        if (!cs->has_decimal) {
            int len = zenith::slen(cs->display_str);
            while (len > 1 && cs->display_str[len - 1] == '0') {
                cs->display_str[--len] = '\0';
            }
            if (len > 0 && cs->display_str[len - 1] == '.') {
                cs->display_str[--len] = '\0';
            }
        }
    } else {
        // Integer display
        if (neg) {
            char int_buf[20];
            int pos = 0;
            int64_t v = integer;
            if (v == 0) {
                int_buf[pos++] = '0';
            } else {
                char tmp[20]; int ti = 0;
                while (v > 0) { tmp[ti++] = '0' + (int)(v % 10); v /= 10; }
                while (ti > 0) int_buf[pos++] = tmp[--ti];
            }
            int_buf[pos] = '\0';
            snprintf(cs->display_str, 32, "-%s", int_buf);
        } else {
            int pos = 0;
            int64_t v = integer;
            if (v == 0) {
                cs->display_str[pos++] = '0';
            } else {
                char tmp[20]; int ti = 0;
                while (v > 0) { tmp[ti++] = '0' + (int)(v % 10); v /= 10; }
                while (ti > 0) cs->display_str[pos++] = tmp[--ti];
            }
            cs->display_str[pos] = '\0';
        }
    }
}

// ============================================================================
// Calculator operations
// ============================================================================

static void calc_apply_op(CalcState* cs) {
    if (cs->pending_op == 0) {
        cs->accumulator = cs->display_val;
        return;
    }
    switch (cs->pending_op) {
    case '+': cs->accumulator += cs->display_val; break;
    case '-': cs->accumulator -= cs->display_val; break;
    case '*': cs->accumulator = (cs->accumulator * cs->display_val) / 100; break;
    case '/':
        if (cs->display_val != 0)
            cs->accumulator = (cs->accumulator * 100) / cs->display_val;
        break;
    }
}

static void calc_input_digit(CalcState* cs, int digit) {
    if (cs->start_new) {
        cs->display_val = 0;
        cs->start_new = false;
        cs->has_decimal = false;
        cs->decimal_digits = 0;
    }

    if (cs->has_decimal) {
        if (cs->decimal_digits < 2) {
            cs->decimal_digits++;
            bool neg = cs->display_val < 0;
            int64_t abs_val = neg ? -cs->display_val : cs->display_val;
            if (cs->decimal_digits == 1) {
                abs_val = (abs_val / 100) * 100 + digit * 10;
            } else {
                abs_val = abs_val + digit;
            }
            cs->display_val = neg ? -abs_val : abs_val;
        }
    } else {
        bool neg = cs->display_val < 0;
        int64_t abs_val = neg ? -cs->display_val : cs->display_val;
        int64_t integer = abs_val / 100;
        if (integer < 999999999) {
            integer = integer * 10 + digit;
            cs->display_val = integer * 100;
            if (neg) cs->display_val = -cs->display_val;
        }
    }
    calc_format_display(cs);
}

static void calc_press_operator(CalcState* cs, char op) {
    if (!cs->start_new) {
        calc_apply_op(cs);
        cs->display_val = cs->accumulator;
    }
    cs->pending_op = op;
    cs->start_new = true;
    cs->has_decimal = false;
    cs->decimal_digits = 0;
    calc_format_display(cs);
}

static void calc_press_equals(CalcState* cs) {
    calc_apply_op(cs);
    cs->display_val = cs->accumulator;
    cs->pending_op = 0;
    cs->start_new = true;
    cs->has_decimal = false;
    cs->decimal_digits = 0;
    calc_format_display(cs);
}

static void calc_press_clear(CalcState* cs) {
    cs->display_val = 0;
    cs->accumulator = 0;
    cs->pending_op = 0;
    cs->start_new = false;
    cs->has_decimal = false;
    cs->decimal_digits = 0;
    calc_format_display(cs);
}

static void calc_press_negate(CalcState* cs) {
    cs->display_val = -cs->display_val;
    calc_format_display(cs);
}

static void calc_press_percent(CalcState* cs) {
    // Divide by 100: display_val is already scaled by 100, so dividing by 100 means /100
    cs->display_val = cs->display_val / 100;
    calc_format_display(cs);
}

static void calc_press_decimal(CalcState* cs) {
    if (cs->start_new) {
        cs->display_val = 0;
        cs->start_new = false;
        cs->decimal_digits = 0;
    }
    cs->has_decimal = true;
    calc_format_display(cs);
}

// ============================================================================
// Drawing
// ============================================================================

// Button layout: labels[row][col]
static const char* calc_labels[5][4] = {
    { "C", "+/-", "%",  "/" },
    { "7", "8",   "9",  "*" },
    { "4", "5",   "6",  "-" },
    { "1", "2",   "3",  "+" },
    { "0", "0",   ".",  "=" },
};

static void calculator_on_draw(Window* win, Framebuffer& fb) {
    CalcState* cs = (CalcState*)win->app_data;
    if (!cs) return;

    Rect cr = win->content_rect();
    int cw = cr.w;
    int ch = cr.h;
    uint32_t* pixels = win->content;

    // Background
    uint32_t bg_px = Color::from_rgb(0xF0, 0xF0, 0xF0).to_pixel();
    for (int i = 0; i < cw * ch; i++) pixels[i] = bg_px;

    // Display area
    uint32_t disp_px = Color::from_rgb(0x2D, 0x2D, 0x2D).to_pixel();
    for (int y = 0; y < CALC_DISPLAY_H && y < ch; y++)
        for (int x = 0; x < cw; x++)
            pixels[y * cw + x] = disp_px;

    // Display text (right-aligned, 2x scale)
    uint32_t disp_text = colors::WHITE.to_pixel();
    int text_len = zenith::slen(cs->display_str);
    int text_w = text_len * FONT_WIDTH * 2;
    int tx = cw - text_w - 12;
    int ty = (CALC_DISPLAY_H - FONT_HEIGHT * 2) / 2;
    if (tx < 4) tx = 4;
    draw_text_to_pixels_2x(pixels, cw, ch, tx, ty, cs->display_str, disp_text);

    // Button grid
    int grid_y = CALC_DISPLAY_H + CALC_BTN_PAD;

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            // Skip second column of "0" button (it spans 2 cols)
            if (row == 4 && col == 1) continue;

            int bx = CALC_BTN_PAD + col * (CALC_BTN_W + CALC_BTN_PAD);
            int by = grid_y + row * (CALC_BTN_H + CALC_BTN_PAD);
            int bw = CALC_BTN_W;

            // "0" button spans 2 columns
            if (row == 4 && col == 0) {
                bw = CALC_BTN_W * 2 + CALC_BTN_PAD;
            }

            // Button color
            uint32_t btn_px;
            if (col == 3) {
                // Operator column: accent blue
                btn_px = colors::ACCENT.to_pixel();
            } else if (row == 0) {
                // Function row: gray
                btn_px = Color::from_rgb(0xD0, 0xD0, 0xD0).to_pixel();
            } else {
                // Digit buttons: light gray
                btn_px = Color::from_rgb(0xE8, 0xE8, 0xE8).to_pixel();
            }

            // Draw button
            for (int dy = 0; dy < CALC_BTN_H && by + dy < ch; dy++)
                for (int dx = 0; dx < bw && bx + dx < cw; dx++)
                    pixels[(by + dy) * cw + (bx + dx)] = btn_px;

            // Button text
            const char* label = calc_labels[row][col];
            uint32_t label_px = (col == 3) ? colors::WHITE.to_pixel() : colors::TEXT_COLOR.to_pixel();
            int label_w = zenith::slen(label) * FONT_WIDTH;
            int lx = bx + (bw - label_w) / 2;
            int ly = by + (CALC_BTN_H - FONT_HEIGHT) / 2;
            draw_text_to_pixels(pixels, cw, ch, lx, ly, label, label_px);
        }
    }
}

// ============================================================================
// Mouse handling
// ============================================================================

static void calculator_on_mouse(Window* win, MouseEvent& ev) {
    CalcState* cs = (CalcState*)win->app_data;
    if (!cs) return;

    if (!ev.left_pressed()) return;

    Rect cr = win->content_rect();
    int local_x = ev.x - cr.x;
    int local_y = ev.y - cr.y;

    // Check if click is in button grid
    int grid_y = CALC_DISPLAY_H + CALC_BTN_PAD;
    if (local_y < grid_y) return;

    int row = (local_y - grid_y) / (CALC_BTN_H + CALC_BTN_PAD);
    int col = (local_x - CALC_BTN_PAD) / (CALC_BTN_W + CALC_BTN_PAD);

    if (row < 0 || row > 4 || col < 0 || col > 3) return;

    // Handle "0" button spanning 2 columns
    if (row == 4 && col <= 1) col = 0;

    // Dispatch button press
    if (row == 0) {
        switch (col) {
        case 0: calc_press_clear(cs); break;
        case 1: calc_press_negate(cs); break;
        case 2: calc_press_percent(cs); break;
        case 3: calc_press_operator(cs, '/'); break;
        }
    } else if (row >= 1 && row <= 3) {
        if (col == 3) {
            char ops[] = {'*', '-', '+'};
            calc_press_operator(cs, ops[row - 1]);
        } else {
            int digits[3][3] = {{7,8,9},{4,5,6},{1,2,3}};
            calc_input_digit(cs, digits[row - 1][col]);
        }
    } else if (row == 4) {
        switch (col) {
        case 0: calc_input_digit(cs, 0); break;
        case 2: calc_press_decimal(cs); break;
        case 3: calc_press_equals(cs); break;
        }
    }
}

// ============================================================================
// Keyboard handling
// ============================================================================

static void calculator_on_key(Window* win, const Zenith::KeyEvent& key) {
    CalcState* cs = (CalcState*)win->app_data;
    if (!cs || !key.pressed) return;

    if (key.ascii >= '0' && key.ascii <= '9') {
        calc_input_digit(cs, key.ascii - '0');
    } else if (key.ascii == '+') {
        calc_press_operator(cs, '+');
    } else if (key.ascii == '-') {
        calc_press_operator(cs, '-');
    } else if (key.ascii == '*') {
        calc_press_operator(cs, '*');
    } else if (key.ascii == '/') {
        calc_press_operator(cs, '/');
    } else if (key.ascii == '=' || key.ascii == '\n' || key.ascii == '\r') {
        calc_press_equals(cs);
    } else if (key.ascii == '.') {
        calc_press_decimal(cs);
    } else if (key.ascii == 'c' || key.ascii == 'C' || key.scancode == 0x0E) {
        calc_press_clear(cs);
    } else if (key.ascii == '%') {
        calc_press_percent(cs);
    }
}

static void calculator_on_close(Window* win) {
    if (win->app_data) {
        zenith::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Calculator launcher
// ============================================================================

void open_calculator(DesktopState* ds) {
    int calc_w = CALC_BTN_PAD + 4 * (CALC_BTN_W + CALC_BTN_PAD);
    int calc_h = CALC_DISPLAY_H + CALC_BTN_PAD + 5 * (CALC_BTN_H + CALC_BTN_PAD);

    int idx = desktop_create_window(ds, "Calculator", 350, 150, calc_w, calc_h + TITLEBAR_HEIGHT + BORDER_WIDTH);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    CalcState* cs = (CalcState*)zenith::malloc(sizeof(CalcState));
    zenith::memset(cs, 0, sizeof(CalcState));
    cs->display_val = 0;
    cs->accumulator = 0;
    cs->pending_op = 0;
    cs->start_new = false;
    cs->has_decimal = false;
    cs->decimal_digits = 0;
    calc_format_display(cs);

    win->app_data = cs;
    win->on_draw = calculator_on_draw;
    win->on_mouse = calculator_on_mouse;
    win->on_key = calculator_on_key;
    win->on_close = calculator_on_close;
}
