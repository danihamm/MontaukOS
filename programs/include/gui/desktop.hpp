/*
    * desktop.hpp
    * ZenithOS desktop state and compositor declarations
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include "gui/framebuffer.hpp"
#include "gui/svg.hpp"
#include "gui/window.hpp"
#include "gui/widgets.hpp"
#include "gui/terminal.hpp"
#include <Api/Syscall.hpp>

namespace gui {

static constexpr int MAX_WINDOWS = 8;
static constexpr int PANEL_HEIGHT = 32;

struct DesktopSettings {
    // Background
    bool bg_gradient;       // true = gradient, false = solid
    Color bg_solid;         // solid background color
    Color bg_grad_top;      // gradient top color
    Color bg_grad_bottom;   // gradient bottom color

    // Panel
    Color panel_color;      // panel background color

    // Accent
    Color accent_color;     // buttons, highlights, active indicators

    // Display
    bool show_shadows;      // window shadows on/off
    bool clock_24h;         // 24-hour clock format
    int ui_scale;           // 0=Small, 1=Default, 2=Large
};

struct DesktopState {
    Framebuffer fb;
    Window windows[MAX_WINDOWS];
    int window_count;
    int focused_window;

    Zenith::MouseState mouse;
    uint8_t prev_buttons;

    bool app_menu_open;

    SvgIcon icon_terminal;
    SvgIcon icon_filemanager;
    SvgIcon icon_sysinfo;
    SvgIcon icon_appmenu;
    SvgIcon icon_folder;
    SvgIcon icon_file;
    SvgIcon icon_computer;
    SvgIcon icon_network;
    SvgIcon icon_calculator;
    SvgIcon icon_texteditor;
    SvgIcon icon_go_up;
    SvgIcon icon_go_back;
    SvgIcon icon_go_forward;
    SvgIcon icon_save;
    SvgIcon icon_home;
    SvgIcon icon_exec;
    SvgIcon icon_wikipedia;

    SvgIcon icon_folder_lg;
    SvgIcon icon_file_lg;
    SvgIcon icon_exec_lg;

    SvgIcon icon_settings;
    SvgIcon icon_reboot;

    SvgIcon icon_weather;

    SvgIcon icon_doom;
    SvgIcon icon_procmgr;
    SvgIcon icon_mandelbrot;
    SvgIcon icon_devexplorer;

    bool ctx_menu_open;
    int ctx_menu_x, ctx_menu_y;

    bool net_popup_open;
    Zenith::NetCfg cached_net_cfg;
    uint64_t net_cfg_last_poll;
    Rect net_icon_rect;

    int screen_w, screen_h;

    DesktopSettings settings;
};

// Forward declarations - implemented in main.cpp
void desktop_init(DesktopState* ds);
void desktop_run(DesktopState* ds);
void desktop_compose(DesktopState* ds);
int  desktop_create_window(DesktopState* ds, const char* title, int x, int y, int w, int h);
void desktop_close_window(DesktopState* ds, int idx);
void desktop_raise_window(DesktopState* ds, int idx);
void desktop_draw_panel(DesktopState* ds);
void desktop_draw_window(DesktopState* ds, int idx);
void desktop_handle_mouse(DesktopState* ds);
void desktop_handle_keyboard(DesktopState* ds, const Zenith::KeyEvent& key);

} // namespace gui
