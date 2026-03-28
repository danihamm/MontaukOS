/*
 * main.cpp
 * MontaukOS Calculator - standalone Window Server app
 * Redesigned to match MontaukOS application conventions
 * Copyright (c) 2026 Daniel Hammer
 */

#include <cstdint>
#include <montauk/config.h>
#include <montauk/syscall.h>
#include <montauk/string.h>
#include <gui/gui.hpp>
#include <gui/canvas.hpp>
#include <gui/font.hpp>
#include <gui/standalone.hpp>
#include <gui/truetype.hpp>

extern "C" {
#include <stdio.h>
}

using namespace gui;

struct CalcState {
    int64_t display_val;    // current display value * 100
    int64_t accumulator;    // stored accumulator * 100
    char pending_op;        // '+', '-', '*', '/', or 0
    bool start_new;         // next digit starts a new number
    bool has_decimal;       // decimal point pressed
    int decimal_digits;     // number of decimal digits entered
    char display_str[32];   // formatted display string
};

enum CalcButtonKind : uint8_t {
    BTN_DIGIT = 0,
    BTN_UTILITY = 1,
    BTN_OPERATOR = 2,
    BTN_EQUALS = 3,
};

enum CalcAction : uint8_t {
    ACT_CLEAR = 0,
    ACT_NEGATE,
    ACT_PERCENT,
    ACT_DIVIDE,
    ACT_7,
    ACT_8,
    ACT_9,
    ACT_MULTIPLY,
    ACT_4,
    ACT_5,
    ACT_6,
    ACT_SUBTRACT,
    ACT_1,
    ACT_2,
    ACT_3,
    ACT_ADD,
    ACT_0,
    ACT_DECIMAL,
    ACT_EQUALS,
};

struct CalcButtonDef {
    CalcAction action;
    const char* label;
    int row;
    int col;
    int col_span;
    CalcButtonKind kind;
    char op;
};

struct CalcButtonLayout {
    Rect rect;
    const CalcButtonDef* def;
};

struct CalcLayout {
    Rect display_card;
    CalcButtonLayout buttons[19];
    int button_count;
};

static constexpr int INIT_W = 260;
static constexpr int INIT_H = 298;
static constexpr int OUTER_PAD = 8;
static constexpr int GRID_GAP = 4;
static constexpr int DISPLAY_H = 78;
static constexpr int CARD_RADIUS = 8;
static constexpr int CALC_BTN_RADIUS = 6;
static constexpr int LABEL_SIZE = 16;
static constexpr int VALUE_SIZE = 28;

static constexpr Color APP_BG        = Color::from_rgb(0xFA, 0xFA, 0xFA);
static constexpr Color GRID_COLOR    = Color::from_rgb(0xD0, 0xD0, 0xD0);
static constexpr Color HEADER_TEXT   = Color::from_rgb(0x55, 0x55, 0x55);
static constexpr Color CELL_TEXT     = Color::from_rgb(0x22, 0x22, 0x22);
static constexpr Color SURFACE_BG    = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color SELECT_FILL   = Color::from_rgb(0xD8, 0xE8, 0xFD);
static constexpr Color TB_BTN_BG     = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color TB_BTN_ACTIVE = Color::from_rgb(0xC0, 0xD0, 0xE8);

static CalcState g_calc = {};
static int g_hovered_button = -1;
static int g_pressed_button = -1;
static Color g_accent = colors::ACCENT;
static Color g_accent_soft = SELECT_FILL;
static Color g_accent_mid = TB_BTN_ACTIVE;
static Color g_accent_dark = colors::ACCENT;

static const CalcButtonDef g_button_defs[] = {
    { ACT_CLEAR,    "C",   0, 0, 1, BTN_UTILITY,  0   },
    { ACT_NEGATE,   "+/-", 0, 1, 1, BTN_UTILITY,  0   },
    { ACT_PERCENT,  "%",   0, 2, 1, BTN_UTILITY,  0   },
    { ACT_DIVIDE,   "/",   0, 3, 1, BTN_OPERATOR, '/' },
    { ACT_7,        "7",   1, 0, 1, BTN_DIGIT,    0   },
    { ACT_8,        "8",   1, 1, 1, BTN_DIGIT,    0   },
    { ACT_9,        "9",   1, 2, 1, BTN_DIGIT,    0   },
    { ACT_MULTIPLY, "*",   1, 3, 1, BTN_OPERATOR, '*' },
    { ACT_4,        "4",   2, 0, 1, BTN_DIGIT,    0   },
    { ACT_5,        "5",   2, 1, 1, BTN_DIGIT,    0   },
    { ACT_6,        "6",   2, 2, 1, BTN_DIGIT,    0   },
    { ACT_SUBTRACT, "-",   2, 3, 1, BTN_OPERATOR, '-' },
    { ACT_1,        "1",   3, 0, 1, BTN_DIGIT,    0   },
    { ACT_2,        "2",   3, 1, 1, BTN_DIGIT,    0   },
    { ACT_3,        "3",   3, 2, 1, BTN_DIGIT,    0   },
    { ACT_ADD,      "+",   3, 3, 1, BTN_OPERATOR, '+' },
    { ACT_0,        "0",   4, 0, 2, BTN_DIGIT,    0   },
    { ACT_DECIMAL,  ".",   4, 2, 1, BTN_DIGIT,    0   },
    { ACT_EQUALS,   "=",   4, 3, 1, BTN_EQUALS,   0   },
};

static constexpr int BUTTON_COUNT = sizeof(g_button_defs) / sizeof(g_button_defs[0]);

static Color shade(Color c, int delta) {
    return Color::from_rgb(
        (uint8_t)gui_clamp((int)c.r + delta, 0, 255),
        (uint8_t)gui_clamp((int)c.g + delta, 0, 255),
        (uint8_t)gui_clamp((int)c.b + delta, 0, 255));
}

static Color mix(Color a, Color b, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int inv = 255 - t;
    return Color::from_rgb(
        (uint8_t)((a.r * inv + b.r * t + 127) / 255),
        (uint8_t)((a.g * inv + b.g * t + 127) / 255),
        (uint8_t)((a.b * inv + b.b * t + 127) / 255));
}

static void calc_set_accent(Color accent) {
    g_accent = accent;
    g_accent_soft = mix(accent, colors::WHITE, 212);
    g_accent_mid = mix(accent, colors::WHITE, 170);
    g_accent_dark = shade(accent, -28);
}

static void calc_load_accent() {
    calc_set_accent(colors::ACCENT);

    char user[64];
    montauk::memset(user, 0, sizeof(user));
    if (montauk::getuser(user, sizeof(user)) > 0 && user[0]) {
        auto doc = montauk::config::load_user(user, "desktop");
        int64_t accent = doc.get_int("appearance.accent_color", -1);
        if (accent >= 0) {
            calc_set_accent(Color::from_rgb(
                (uint8_t)((accent >> 16) & 0xFF),
                (uint8_t)((accent >> 8) & 0xFF),
                (uint8_t)(accent & 0xFF)));
            doc.destroy();
            return;
        }
        doc.destroy();
    }

    auto doc = montauk::config::load("desktop");
    int64_t accent = doc.get_int("appearance.accent_color", -1);
    if (accent >= 0) {
        calc_set_accent(Color::from_rgb(
            (uint8_t)((accent >> 16) & 0xFF),
            (uint8_t)((accent >> 8) & 0xFF),
            (uint8_t)(accent & 0xFF)));
    }
    doc.destroy();
}

static void calc_format_display(CalcState* cs) {
    int64_t val = cs->display_val;
    bool neg = val < 0;
    if (neg) val = -val;

    int64_t integer = val / 100;
    int64_t frac = val % 100;

    if (cs->has_decimal || frac != 0) {
        char int_buf[20];
        int pos = 0;

        if (integer == 0) {
            int_buf[pos++] = '0';
        } else {
            char tmp[20];
            int ti = 0;
            int64_t v = integer;
            while (v > 0) {
                tmp[ti++] = '0' + (int)(v % 10);
                v /= 10;
            }
            while (ti > 0) int_buf[pos++] = tmp[--ti];
        }
        int_buf[pos] = '\0';

        if (neg) {
            snprintf(cs->display_str, sizeof(cs->display_str), "-%s.%02d", int_buf, (int)frac);
        } else {
            snprintf(cs->display_str, sizeof(cs->display_str), "%s.%02d", int_buf, (int)frac);
        }

        if (!cs->has_decimal) {
            int len = montauk::slen(cs->display_str);
            while (len > 1 && cs->display_str[len - 1] == '0') {
                cs->display_str[--len] = '\0';
            }
            if (len > 0 && cs->display_str[len - 1] == '.') {
                cs->display_str[--len] = '\0';
            }
        }
    } else {
        if (neg) {
            char int_buf[20];
            int pos = 0;
            int64_t v = integer;
            if (v == 0) {
                int_buf[pos++] = '0';
            } else {
                char tmp[20];
                int ti = 0;
                while (v > 0) {
                    tmp[ti++] = '0' + (int)(v % 10);
                    v /= 10;
                }
                while (ti > 0) int_buf[pos++] = tmp[--ti];
            }
            int_buf[pos] = '\0';
            snprintf(cs->display_str, sizeof(cs->display_str), "-%s", int_buf);
        } else {
            int pos = 0;
            int64_t v = integer;
            if (v == 0) {
                cs->display_str[pos++] = '0';
            } else {
                char tmp[20];
                int ti = 0;
                while (v > 0) {
                    tmp[ti++] = '0' + (int)(v % 10);
                    v /= 10;
                }
                while (ti > 0) cs->display_str[pos++] = tmp[--ti];
            }
            cs->display_str[pos] = '\0';
        }
    }
}

static void calc_format_value(char* out, int out_sz, int64_t value) {
    CalcState tmp = {};
    tmp.display_val = value;
    calc_format_display(&tmp);
    montauk::strncpy(out, tmp.display_str, out_sz);
}

static void calc_apply_op(CalcState* cs) {
    if (cs->pending_op == 0) {
        cs->accumulator = cs->display_val;
        return;
    }

    switch (cs->pending_op) {
    case '+':
        cs->accumulator += cs->display_val;
        break;
    case '-':
        cs->accumulator -= cs->display_val;
        break;
    case '*':
        cs->accumulator = (cs->accumulator * cs->display_val) / 100;
        break;
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
                abs_val += digit;
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

static void calc_expression_text(char* out, int out_sz) {
    if (g_calc.pending_op) {
        char acc_buf[32];
        calc_format_value(acc_buf, sizeof(acc_buf), g_calc.accumulator);
        snprintf(out, out_sz, "%s %c", acc_buf, g_calc.pending_op);
    } else if (g_calc.start_new) {
        snprintf(out, out_sz, "Result");
    } else if (g_calc.has_decimal) {
        snprintf(out, out_sz, "Entering decimal");
    } else {
        out[0] = '\0';
    }
}

static void calc_build_layout(int w, int h, CalcLayout* layout) {
    if (!layout) return;

    layout->display_card = {OUTER_PAD, OUTER_PAD, w - OUTER_PAD * 2, DISPLAY_H};
    layout->button_count = BUTTON_COUNT;

    int grid_y = layout->display_card.y + layout->display_card.h + OUTER_PAD;
    int grid_h = h - grid_y - OUTER_PAD;
    if (grid_h < 5) grid_h = 5;

    int grid_w = w - OUTER_PAD * 2;
    int btn_w = (grid_w - GRID_GAP * 3) / 4;
    int btn_h = (grid_h - GRID_GAP * 4) / 5;
    if (btn_w < 8) btn_w = 8;
    if (btn_h < 8) btn_h = 8;

    for (int i = 0; i < BUTTON_COUNT; i++) {
        const CalcButtonDef* def = &g_button_defs[i];
        int x = OUTER_PAD + def->col * (btn_w + GRID_GAP);
        int y = grid_y + def->row * (btn_h + GRID_GAP);
        int bw = btn_w * def->col_span + GRID_GAP * (def->col_span - 1);
        layout->buttons[i].rect = {x, y, bw, btn_h};
        layout->buttons[i].def = def;
    }
}

static int calc_hit_button(const CalcLayout& layout, int x, int y) {
    for (int i = 0; i < layout.button_count; i++) {
        if (layout.buttons[i].rect.contains(x, y))
            return i;
    }
    return -1;
}

static void draw_surface(Canvas& c, const Rect& r, int radius, Color border, Color fill) {
    c.fill_rounded_rect(r.x, r.y, r.w, r.h, radius, border);
    if (r.w > 2 && r.h > 2) {
        c.fill_rounded_rect(
            r.x + 1, r.y + 1, r.w - 2, r.h - 2,
            radius > 0 ? radius - 1 : 0, fill);
    }
}

static bool calc_button_active(const CalcButtonDef& def) {
    return def.kind == BTN_OPERATOR && def.op != 0 && g_calc.pending_op == def.op;
}

static void calc_dispatch(CalcAction action) {
    switch (action) {
    case ACT_CLEAR:    calc_press_clear(&g_calc); break;
    case ACT_NEGATE:   calc_press_negate(&g_calc); break;
    case ACT_PERCENT:  calc_press_percent(&g_calc); break;
    case ACT_DIVIDE:   calc_press_operator(&g_calc, '/'); break;
    case ACT_7:        calc_input_digit(&g_calc, 7); break;
    case ACT_8:        calc_input_digit(&g_calc, 8); break;
    case ACT_9:        calc_input_digit(&g_calc, 9); break;
    case ACT_MULTIPLY: calc_press_operator(&g_calc, '*'); break;
    case ACT_4:        calc_input_digit(&g_calc, 4); break;
    case ACT_5:        calc_input_digit(&g_calc, 5); break;
    case ACT_6:        calc_input_digit(&g_calc, 6); break;
    case ACT_SUBTRACT: calc_press_operator(&g_calc, '-'); break;
    case ACT_1:        calc_input_digit(&g_calc, 1); break;
    case ACT_2:        calc_input_digit(&g_calc, 2); break;
    case ACT_3:        calc_input_digit(&g_calc, 3); break;
    case ACT_ADD:      calc_press_operator(&g_calc, '+'); break;
    case ACT_0:        calc_input_digit(&g_calc, 0); break;
    case ACT_DECIMAL:  calc_press_decimal(&g_calc); break;
    case ACT_EQUALS:   calc_press_equals(&g_calc); break;
    }
}

static void render(Canvas& c) {
    CalcLayout layout;
    calc_build_layout(c.w, c.h, &layout);

    c.fill(APP_BG);

    char chip_text[24];
    if (g_calc.pending_op) {
        snprintf(chip_text, sizeof(chip_text), "Op: %c", g_calc.pending_op);
    } else {
        snprintf(chip_text, sizeof(chip_text), "Ready");
    }
    int chip_w = text_width(fonts::system_font, chip_text, LABEL_SIZE) + 18;
    Rect chip = {
        layout.display_card.x + layout.display_card.w - chip_w - 10,
        layout.display_card.y + 10,
        chip_w,
        24
    };
    c.fill_rounded_rect(chip.x, chip.y, chip.w, chip.h, 4,
                        g_calc.pending_op ? g_accent_mid : g_accent_soft);
    draw_text(c, fonts::system_font, chip.x + 9, chip.y + 4, chip_text, HEADER_TEXT, LABEL_SIZE);

    draw_surface(c, layout.display_card, CARD_RADIUS, GRID_COLOR, SURFACE_BG);

    char expr[48];
    calc_expression_text(expr, sizeof(expr));
    draw_text(c, fonts::system_font,
              layout.display_card.x + 12, layout.display_card.y + 10,
              expr, HEADER_TEXT, LABEL_SIZE);

    int value_w;
    int value_h;
    if (fonts::system_font && fonts::system_font->valid) {
        value_w = fonts::system_font->measure_text(g_calc.display_str, VALUE_SIZE);
        value_h = fonts::system_font->get_line_height(VALUE_SIZE);
    } else {
        int text_len = montauk::slen(g_calc.display_str);
        value_w = text_len * FONT_WIDTH * 2;
        value_h = FONT_HEIGHT * 2;
    }

    int value_x = layout.display_card.x + layout.display_card.w - value_w - 12;
    int value_y = layout.display_card.y + 26 + (layout.display_card.h - 26 - value_h) / 2;
    if (value_x < layout.display_card.x + 10) value_x = layout.display_card.x + 10;
    draw_text(c, fonts::system_font, value_x, value_y, g_calc.display_str, CELL_TEXT, VALUE_SIZE);

    for (int i = 0; i < layout.button_count; i++) {
        const CalcButtonDef& def = *layout.buttons[i].def;
        const Rect& rect = layout.buttons[i].rect;

        bool active = calc_button_active(def);
        bool hovered = i == g_hovered_button;
        bool pressed = i == g_pressed_button;

        Color fill = g_accent_soft;
        Color text = CELL_TEXT;

        if (def.kind == BTN_UTILITY) {
            fill = g_accent_mid;
        } else if (def.kind == BTN_OPERATOR) {
            fill = active ? g_accent_dark : g_accent_mid;
            text = active ? colors::WHITE : CELL_TEXT;
        } else if (def.kind == BTN_EQUALS) {
            fill = g_accent;
            text = colors::WHITE;
        }

        if (hovered && !pressed) {
            fill = shade(fill, 8);
        }
        if (pressed) {
            fill = shade(fill, -12);
        }

        c.fill_rounded_rect(rect.x, rect.y, rect.w, rect.h, CALC_BTN_RADIUS, fill);

        TrueTypeFont* button_font =
            ((def.kind == BTN_OPERATOR || def.kind == BTN_EQUALS || def.kind == BTN_UTILITY) &&
             fonts::system_bold && fonts::system_bold->valid)
            ? fonts::system_bold : fonts::system_font;

        int tw = text_width(button_font, def.label, LABEL_SIZE);
        int th = text_height(button_font, LABEL_SIZE);
        int lx = rect.x + (rect.w - tw) / 2;
        int ly = rect.y + (rect.h - th) / 2;
        draw_text(c, button_font, lx, ly, def.label, text, LABEL_SIZE);
    }
}

static bool handle_mouse(const Montauk::WinEvent& ev, int win_w, int win_h) {
    CalcLayout layout;
    calc_build_layout(win_w, win_h, &layout);

    bool redraw = false;
    int hovered = calc_hit_button(layout, ev.mouse.x, ev.mouse.y);
    if (hovered != g_hovered_button) {
        g_hovered_button = hovered;
        redraw = true;
    }

    bool left_pressed = (ev.mouse.buttons & 1) && !(ev.mouse.prev_buttons & 1);
    bool left_released = !(ev.mouse.buttons & 1) && (ev.mouse.prev_buttons & 1);

    if (left_pressed && hovered >= 0) {
        g_pressed_button = hovered;
        calc_dispatch(layout.buttons[hovered].def->action);
        redraw = true;
    }

    if (left_released && g_pressed_button != -1) {
        g_pressed_button = -1;
        redraw = true;
    }

    return redraw;
}

static bool handle_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return false;

    if (key.ascii >= '0' && key.ascii <= '9') {
        calc_input_digit(&g_calc, key.ascii - '0');
    } else if (key.ascii == '+') {
        calc_press_operator(&g_calc, '+');
    } else if (key.ascii == '-') {
        calc_press_operator(&g_calc, '-');
    } else if (key.ascii == '*') {
        calc_press_operator(&g_calc, '*');
    } else if (key.ascii == '/') {
        calc_press_operator(&g_calc, '/');
    } else if (key.ascii == '=' || key.ascii == '\n' || key.ascii == '\r') {
        calc_press_equals(&g_calc);
    } else if (key.ascii == '.') {
        calc_press_decimal(&g_calc);
    } else if (key.ascii == 'c' || key.ascii == 'C' || key.scancode == 0x0E) {
        calc_press_clear(&g_calc);
    } else if (key.ascii == '%') {
        calc_press_percent(&g_calc);
    } else {
        return false;
    }

    return true;
}

extern "C" void _start() {
    fonts::init();
    calc_load_accent();
    calc_press_clear(&g_calc);

    WsWindow win;
    if (!win.create("Calculator", INIT_W, INIT_H))
        montauk::exit(1);

    {
        Canvas canvas = win.canvas();
        render(canvas);
    }
    win.present();

    while (win.id >= 0 && !win.closed) {
        Montauk::WinEvent ev;
        int r = win.poll(&ev);

        if (r < 0) break;
        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        bool redraw = false;

        if (ev.type == 3) break;
        if (ev.type == 2 || ev.type == 4) {
            g_hovered_button = -1;
            g_pressed_button = -1;
            redraw = true;
        } else if (ev.type == 1) {
            redraw = handle_mouse(ev, win.width, win.height);
        } else if (ev.type == 0) {
            redraw = handle_key(ev.key);
        }

        if (redraw) {
            Canvas canvas = win.canvas();
            render(canvas);
            win.present();
        }
    }

    win.destroy();
    montauk::exit(0);
}
