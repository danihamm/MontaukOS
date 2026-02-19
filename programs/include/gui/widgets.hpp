/*
    * widgets.hpp
    * ZenithOS GUI widget toolkit (Label, Button, TextBox, Scrollbar)
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"
#include "gui/font.hpp"
#include "gui/draw.hpp"

namespace gui {

// ---- Mouse event ----

struct MouseEvent {
    int x, y;
    uint8_t buttons;
    uint8_t prev_buttons;
    int32_t scroll;

    bool left_held() const { return buttons & 0x01; }
    bool right_held() const { return buttons & 0x02; }
    bool middle_held() const { return buttons & 0x04; }

    bool left_pressed() const { return (buttons & 0x01) && !(prev_buttons & 0x01); }
    bool left_released() const { return !(buttons & 0x01) && (prev_buttons & 0x01); }
    bool right_pressed() const { return (buttons & 0x02) && !(prev_buttons & 0x02); }
    bool right_released() const { return !(buttons & 0x02) && (prev_buttons & 0x02); }
};

// ---- Callback types ----

using ClickCallback = void (*)(void* userdata);

// ---- Label ----

struct Label {
    int x, y;
    const char* text;
    Color color;

    void draw(Framebuffer& fb) const {
        if (text) {
            draw_text(fb, x, y, text, color);
        }
    }
};

// ---- Button ----

struct Button {
    Rect bounds;
    const char* text;
    Color bg;
    Color fg;
    Color hover_bg;
    bool hovered;
    bool pressed;
    ClickCallback on_click;
    void* userdata;

    void init(int x, int y, int w, int h, const char* label) {
        bounds = {x, y, w, h};
        text = label;
        bg = colors::ACCENT;
        fg = colors::WHITE;
        hover_bg = Color::from_rgb(0x2B, 0x6B, 0xE0);
        hovered = false;
        pressed = false;
        on_click = nullptr;
        userdata = nullptr;
    }

    void draw(Framebuffer& fb) const {
        Color bg_color = hovered ? hover_bg : bg;
        fill_rounded_rect(fb, bounds.x, bounds.y, bounds.w, bounds.h, 4, bg_color);
        // Center text
        int tw = text_width(text);
        int tx = bounds.x + (bounds.w - tw) / 2;
        int ty = bounds.y + (bounds.h - FONT_HEIGHT) / 2;
        draw_text(fb, tx, ty, text, fg);
    }

    bool handle_mouse(const MouseEvent& ev) {
        hovered = bounds.contains(ev.x, ev.y);
        if (hovered && ev.left_pressed()) {
            pressed = true;
        }
        if (pressed && ev.left_released()) {
            pressed = false;
            if (hovered && on_click) {
                on_click(userdata);
                return true;
            }
        }
        if (!ev.left_held()) {
            pressed = false;
        }
        return false;
    }
};

// ---- IconButton (for panel/menu items with SVG icon + optional text) ----

struct IconButton {
    Rect bounds;
    const char* text;        // optional label (can be nullptr)
    uint32_t* icon_pixels;   // icon pixel data (ARGB)
    int icon_w, icon_h;
    Color bg;
    Color hover_bg;
    Color text_color;
    bool hovered;
    bool pressed;
    ClickCallback on_click;
    void* userdata;

    void init(int x, int y, int w, int h) {
        bounds = {x, y, w, h};
        text = nullptr;
        icon_pixels = nullptr;
        icon_w = 0;
        icon_h = 0;
        bg = {0, 0, 0, 0}; // transparent by default
        hover_bg = colors::MENU_HOVER;
        text_color = colors::TEXT_COLOR;
        hovered = false;
        pressed = false;
        on_click = nullptr;
        userdata = nullptr;
    }

    void draw(Framebuffer& fb) const {
        if (hovered) {
            fill_rounded_rect(fb, bounds.x, bounds.y, bounds.w, bounds.h, 3, hover_bg);
        } else if (bg.a > 0) {
            fill_rounded_rect(fb, bounds.x, bounds.y, bounds.w, bounds.h, 3, bg);
        }

        int content_x = bounds.x + 6;

        // Draw icon
        if (icon_pixels && icon_w > 0 && icon_h > 0) {
            int iy = bounds.y + (bounds.h - icon_h) / 2;
            fb.blit_alpha(content_x, iy, icon_w, icon_h, icon_pixels);
            content_x += icon_w + 6;
        }

        // Draw text
        if (text) {
            int ty = bounds.y + (bounds.h - FONT_HEIGHT) / 2;
            draw_text(fb, content_x, ty, text, text_color);
        }
    }

    bool handle_mouse(const MouseEvent& ev) {
        hovered = bounds.contains(ev.x, ev.y);
        if (hovered && ev.left_pressed()) {
            pressed = true;
        }
        if (pressed && ev.left_released()) {
            pressed = false;
            if (hovered && on_click) {
                on_click(userdata);
                return true;
            }
        }
        if (!ev.left_held()) {
            pressed = false;
        }
        return false;
    }
};

// ---- TextBox ----

struct TextBox {
    Rect bounds;
    char text[256];
    int cursor;
    int text_len;
    bool focused;
    Color bg;
    Color fg;
    Color border_color;
    Color cursor_color;

    void init(int x, int y, int w, int h) {
        bounds = {x, y, w, h};
        text[0] = '\0';
        cursor = 0;
        text_len = 0;
        focused = false;
        bg = colors::WHITE;
        fg = colors::TEXT_COLOR;
        border_color = colors::BORDER;
        cursor_color = colors::ACCENT;
    }

    void draw(Framebuffer& fb) const {
        fb.fill_rect(bounds.x, bounds.y, bounds.w, bounds.h, bg);
        draw_rect(fb, bounds.x, bounds.y, bounds.w, bounds.h,
                  focused ? cursor_color : border_color);

        // Draw text with 4px padding
        int tx = bounds.x + 4;
        int ty = bounds.y + (bounds.h - FONT_HEIGHT) / 2;
        draw_text(fb, tx, ty, text, fg);

        // Draw cursor if focused
        if (focused) {
            int cx = tx + cursor * FONT_WIDTH;
            draw_vline(fb, cx, ty, FONT_HEIGHT, cursor_color);
        }
    }

    void handle_mouse(const MouseEvent& ev) {
        if (ev.left_pressed()) {
            focused = bounds.contains(ev.x, ev.y);
        }
    }

    void handle_key(const Zenith::KeyEvent& key) {
        if (!focused || !key.pressed) return;

        if (key.ascii == '\b' || key.scancode == 0x0E) {
            // Backspace
            if (cursor > 0) {
                for (int i = cursor - 1; i < text_len - 1; i++) {
                    text[i] = text[i + 1];
                }
                text_len--;
                cursor--;
                text[text_len] = '\0';
            }
        } else if (key.ascii >= 32 && key.ascii < 127) {
            // Printable character
            if (text_len < 254) {
                for (int i = text_len; i > cursor; i--) {
                    text[i] = text[i - 1];
                }
                text[cursor] = key.ascii;
                cursor++;
                text_len++;
                text[text_len] = '\0';
            }
        } else if (key.scancode == 0x4B) {
            // Left arrow
            if (cursor > 0) cursor--;
        } else if (key.scancode == 0x4D) {
            // Right arrow
            if (cursor < text_len) cursor++;
        }
    }
};

// ---- Scrollbar ----

struct Scrollbar {
    Rect bounds;
    int content_height;
    int view_height;
    int scroll_offset;
    bool dragging;
    int drag_start_y;
    int drag_start_offset;
    Color bg;
    Color fg;
    Color hover_fg;
    bool hovered;

    void init(int x, int y, int w, int h) {
        bounds = {x, y, w, h};
        content_height = 0;
        view_height = h;
        scroll_offset = 0;
        dragging = false;
        drag_start_y = 0;
        drag_start_offset = 0;
        bg = colors::SCROLLBAR_BG;
        fg = colors::SCROLLBAR_FG;
        hover_fg = Color::from_rgb(0xA0, 0xA0, 0xA0);
        hovered = false;
    }

    int thumb_height() const {
        if (content_height <= view_height) return bounds.h;
        int th = (view_height * bounds.h) / content_height;
        return th < 20 ? 20 : th;
    }

    int thumb_y() const {
        if (content_height <= view_height) return bounds.y;
        int range = bounds.h - thumb_height();
        int max_scroll = content_height - view_height;
        if (max_scroll <= 0) return bounds.y;
        return bounds.y + (scroll_offset * range) / max_scroll;
    }

    int max_scroll() const {
        int ms = content_height - view_height;
        return ms > 0 ? ms : 0;
    }

    void draw(Framebuffer& fb) const {
        if (content_height <= view_height) return; // no scrollbar needed

        fb.fill_rect(bounds.x, bounds.y, bounds.w, bounds.h, bg);
        int th = thumb_height();
        int ty = thumb_y();
        Color thumb_color = (hovered || dragging) ? hover_fg : fg;
        fill_rounded_rect(fb, bounds.x + 1, ty, bounds.w - 2, th, 3, thumb_color);
    }

    void handle_mouse(const MouseEvent& ev) {
        if (content_height <= view_height) return;

        Rect thumb_rect = {bounds.x, thumb_y(), bounds.w, thumb_height()};
        hovered = thumb_rect.contains(ev.x, ev.y);

        if (hovered && ev.left_pressed()) {
            dragging = true;
            drag_start_y = ev.y;
            drag_start_offset = scroll_offset;
        }

        if (dragging && ev.left_held()) {
            int dy = ev.y - drag_start_y;
            int range = bounds.h - thumb_height();
            if (range > 0) {
                int ms = max_scroll();
                scroll_offset = drag_start_offset + (dy * ms) / range;
                if (scroll_offset < 0) scroll_offset = 0;
                if (scroll_offset > ms) scroll_offset = ms;
            }
        }

        if (!ev.left_held()) {
            dragging = false;
        }

        // Handle scroll wheel
        if (bounds.contains(ev.x, ev.y) && ev.scroll != 0) {
            scroll_offset += ev.scroll * 20;
            int ms = max_scroll();
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > ms) scroll_offset = ms;
        }
    }
};

} // namespace gui
