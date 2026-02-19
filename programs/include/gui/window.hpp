/*
    * window.hpp
    * ZenithOS window management types
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"
#include "gui/widgets.hpp"
#include <Api/Syscall.hpp>

namespace gui {

enum WindowState { WIN_NORMAL, WIN_MINIMIZED, WIN_MAXIMIZED, WIN_CLOSED };

static constexpr int TITLEBAR_HEIGHT = 30;
static constexpr int BORDER_WIDTH = 1;
static constexpr int SHADOW_SIZE = 3;
static constexpr int BTN_RADIUS = 6;
static constexpr int MAX_TITLE_LEN = 64;

struct Window;
using WindowDrawCallback  = void (*)(Window* win, Framebuffer& fb);
using WindowMouseCallback = void (*)(Window* win, MouseEvent& ev);
using WindowKeyCallback   = void (*)(Window* win, const Zenith::KeyEvent& key);
using WindowCloseCallback = void (*)(Window* win);

struct Window {
    char title[MAX_TITLE_LEN];
    Rect frame;
    WindowState state;
    int z_order;
    bool focused;
    bool dirty;

    uint32_t* content;
    int content_w, content_h;

    bool dragging;
    int drag_offset_x, drag_offset_y;

    bool resizing;

    Rect saved_frame;

    WindowDrawCallback  on_draw;
    WindowMouseCallback on_mouse;
    WindowKeyCallback   on_key;
    WindowCloseCallback on_close;
    void* app_data;

    Rect titlebar_rect() const {
        return {frame.x, frame.y, frame.w, TITLEBAR_HEIGHT};
    }

    Rect content_rect() const {
        return {frame.x + BORDER_WIDTH, frame.y + TITLEBAR_HEIGHT,
                frame.w - 2 * BORDER_WIDTH, frame.h - TITLEBAR_HEIGHT - BORDER_WIDTH};
    }

    Rect close_btn_rect() const {
        int by = frame.y + (TITLEBAR_HEIGHT - BTN_RADIUS * 2) / 2;
        return {frame.x + 12, by, BTN_RADIUS * 2, BTN_RADIUS * 2};
    }

    Rect min_btn_rect() const {
        int by = frame.y + (TITLEBAR_HEIGHT - BTN_RADIUS * 2) / 2;
        return {frame.x + 12 + 22, by, BTN_RADIUS * 2, BTN_RADIUS * 2};
    }

    Rect max_btn_rect() const {
        int by = frame.y + (TITLEBAR_HEIGHT - BTN_RADIUS * 2) / 2;
        return {frame.x + 12 + 44, by, BTN_RADIUS * 2, BTN_RADIUS * 2};
    }
};

} // namespace gui
