/*
    * main.cpp
    * MontaukOS Desktop Environment - initialization, external window polling, run loop
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

// ============================================================================
// Initialization
// ============================================================================

void gui::desktop_init(DesktopState* ds) {
    ds->screen_w = ds->fb.width();
    ds->screen_h = ds->fb.height();

    // Immediately clear the screen to hide flanterm boot text
    ds->fb.clear(colors::DESKTOP_BG);
    ds->fb.flip();

    // Load TrueType fonts
    fonts::init();

    ds->window_count = 0;
    ds->focused_window = -1;
    ds->prev_buttons = 0;
    ds->app_menu_open = false;

    montauk::memset(&ds->mouse, 0, sizeof(Montauk::MouseState));
    montauk::set_mouse_bounds(ds->screen_w - 1, ds->screen_h - 1);

    // Load SVG icons — scalable (colorful) for app menu, symbolic for toolbar/panel
    Color defColor = colors::ICON_COLOR;
    ds->icon_terminal    = svg_load("0:/icons/utilities-terminal.svg",        20, 20, defColor);
    ds->icon_filemanager = svg_load("0:/icons/system-file-manager.svg",      20, 20, defColor);
    ds->icon_sysinfo     = svg_load("0:/icons/preferences-desktop-apps.svg", 20, 20, defColor);
    ds->icon_appmenu     = svg_load("0:/icons/view-app-grid-symbolic.svg",   20, 20, colors::PANEL_TEXT);
    ds->icon_folder      = svg_load("0:/icons/folder.svg",                   16, 16, defColor);
    ds->icon_file        = svg_load("0:/icons/text-x-generic.svg",           16, 16, defColor);
    ds->icon_computer    = svg_load("0:/icons/computer.svg",                 20, 20, defColor);
    ds->icon_network     = svg_load("0:/icons/network-wired-symbolic.svg",   16, 16, colors::PANEL_TEXT);
    ds->icon_calculator  = svg_load("0:/icons/accessories-calculator.svg",   20, 20, defColor);
    ds->icon_texteditor  = svg_load("0:/icons/accessories-text-editor.svg",  20, 20, defColor);
    ds->icon_go_up       = svg_load("0:/icons/go-up-symbolic.svg",           16, 16, defColor);
    ds->icon_go_back     = svg_load("0:/icons/go-previous-symbolic.svg",     16, 16, defColor);
    ds->icon_go_forward  = svg_load("0:/icons/go-next-symbolic.svg",         16, 16, defColor);
    ds->icon_save        = svg_load("0:/icons/document-save-symbolic.svg",   16, 16, defColor);
    ds->icon_home        = svg_load("0:/icons/user-home.svg",                16, 16, defColor);
    ds->icon_exec        = svg_load("0:/icons/utilities-terminal.svg",        16, 16, defColor);
    ds->icon_wikipedia   = svg_load("0:/icons/web-browser.svg",              20, 20, defColor);

    ds->icon_folder_lg = svg_load("0:/icons/folder.svg",                   48, 48, defColor);
    ds->icon_file_lg   = svg_load("0:/icons/text-x-generic.svg",           48, 48, defColor);
    ds->icon_exec_lg   = svg_load("0:/icons/utilities-terminal.svg",        48, 48, defColor);

    ds->icon_settings = svg_load("0:/icons/help-about.svg",     20, 20, defColor);
    ds->icon_reboot   = svg_load("0:/icons/system-reboot.svg", 20, 20, defColor);
    ds->icon_shutdown = svg_load("0:/icons/system-shutdown.svg", 20, 20, defColor);

    ds->icon_weather = svg_load("0:/icons/weather-widget.svg", 20, 20, defColor);

    ds->icon_doom     = svg_load("0:/icons/doom.svg", 20, 20, defColor);
    ds->icon_procmgr  = svg_load("0:/icons/system-monitor.svg", 20, 20, defColor);
    ds->icon_mandelbrot = svg_load("0:/icons/applications-science.svg", 20, 20, defColor);
    ds->icon_devexplorer = svg_load("0:/icons/hardware.svg", 20, 20, defColor);
    ds->icon_spreadsheet = svg_load("0:/icons/spreadsheet.svg", 20, 20, defColor);

    // Settings defaults
    ds->settings.bg_gradient = true;
    ds->settings.bg_image = false;
    ds->settings.bg_grad_top = Color::from_rgb(0xD0, 0xD8, 0xE8);
    ds->settings.bg_grad_bottom = Color::from_rgb(0xA0, 0xA8, 0xB8);
    ds->settings.bg_solid = Color::from_rgb(0xD0, 0xD8, 0xE8);
    ds->settings.bg_image_path[0] = '\0';
    ds->settings.bg_wallpaper = nullptr;
    ds->settings.bg_wallpaper_w = 0;
    ds->settings.bg_wallpaper_h = 0;
    ds->settings.panel_color = Color::from_rgb(0x10, 0x10, 0x10);
    ds->settings.accent_color = colors::ACCENT;
    ds->settings.show_shadows = true;
    ds->settings.clock_24h = true;
    ds->settings.ui_scale = 1;

    // Try to load default wallpaper
    wallpaper_load(&ds->settings, "0:/home/stephen-walker-DaC8D3USffk-unsplash.jpg",
                   ds->screen_w, ds->screen_h);
    montauk::win_setscale(1);

    ds->ctx_menu_open = false;
    ds->ctx_menu_x = 0;
    ds->ctx_menu_y = 0;

    ds->net_popup_open = false;
    montauk::get_netcfg(&ds->cached_net_cfg);
    ds->net_cfg_last_poll = montauk::get_milliseconds();
    ds->net_icon_rect = {0, 0, 0, 0};

}

// ============================================================================
// External Window Polling
// ============================================================================

void desktop_poll_external_windows(DesktopState* ds) {
    Montauk::WinInfo extWins[8];
    int extCount = montauk::win_enumerate(extWins, 8);

    // Check for new external windows and map them
    for (int e = 0; e < extCount; e++) {
        int extId = extWins[e].id;

        // Check if we already have this window
        bool found = false;
        for (int i = 0; i < ds->window_count; i++) {
            if (ds->windows[i].external && ds->windows[i].ext_win_id == extId) {
                found = true;
                // Update dirty flag and cursor
                if (extWins[e].dirty) {
                    ds->windows[i].dirty = true;
                }
                ds->windows[i].ext_cursor = extWins[e].cursor;
                // Re-map if external app resized its buffer
                if (extWins[e].width != ds->windows[i].content_w ||
                    extWins[e].height != ds->windows[i].content_h) {
                    uint64_t va = montauk::win_map(extId);
                    if (va != 0) {
                        ds->windows[i].content = (uint32_t*)va;
                        ds->windows[i].content_w = extWins[e].width;
                        ds->windows[i].content_h = extWins[e].height;
                        ds->windows[i].dirty = true;
                    }
                }
                break;
            }
        }

        if (!found && ds->window_count < MAX_WINDOWS) {
            // Map the pixel buffer into our address space
            uint64_t va = montauk::win_map(extId);
            if (va == 0) continue;

            int idx = ds->window_count;
            Window* win = &ds->windows[idx];
            montauk::memset(win, 0, sizeof(Window));

            montauk::strncpy(win->title, extWins[e].title, MAX_TITLE_LEN);
            int w = extWins[e].width;
            int h = extWins[e].height;
            // Position the window centered-ish
            int wx = (ds->screen_w - w - 2 * BORDER_WIDTH) / 2 + idx * 30;
            int wy = PANEL_HEIGHT + 20 + idx * 30;
            win->frame = {wx, wy, w + 2 * BORDER_WIDTH, h + TITLEBAR_HEIGHT + BORDER_WIDTH};
            win->state = WIN_NORMAL;
            win->z_order = idx;
            win->focused = true;
            win->dirty = true;
            win->dragging = false;
            win->resizing = false;
            win->saved_frame = win->frame;

            // Point content to the shared pixel buffer
            win->content = (uint32_t*)va;
            win->content_w = w;
            win->content_h = h;

            win->on_draw = nullptr;
            win->on_mouse = nullptr;
            win->on_key = nullptr;
            win->on_close = nullptr;
            win->on_poll = nullptr;
            win->app_data = nullptr;
            win->external = true;
            win->ext_win_id = extId;

            // Unfocus previous window
            if (ds->focused_window >= 0 && ds->focused_window < ds->window_count) {
                ds->windows[ds->focused_window].focused = false;
            }
            ds->focused_window = idx;
            ds->window_count++;
        }
    }

    // Check for removed external windows (process exited)
    for (int i = ds->window_count - 1; i >= 0; i--) {
        if (!ds->windows[i].external) continue;

        int extId = ds->windows[i].ext_win_id;
        bool stillExists = false;
        for (int e = 0; e < extCount; e++) {
            if (extWins[e].id == extId) {
                stillExists = true;
                break;
            }
        }

        if (!stillExists) {
            // Window gone — remove without freeing content (shared memory)
            ds->windows[i].content = nullptr; // prevent free
            gui::desktop_close_window(ds, i);
        }
    }
}

// ============================================================================
// Run Loop
// ============================================================================

void gui::desktop_run(DesktopState* ds) {
    for (;;) {
        // Poll mouse state
        ds->prev_buttons = ds->mouse.buttons;
        montauk::mouse_state(&ds->mouse);

        // Poll keyboard events
        while (montauk::is_key_available()) {
            Montauk::KeyEvent key;
            montauk::getkey(&key);
            desktop_handle_keyboard(ds, key);
        }

        // Poll external windows (discover new, remove dead, update dirty)
        desktop_poll_external_windows(ds);

        // Poll windows that have a poll callback
        for (int i = 0; i < ds->window_count; i++) {
            Window* win = &ds->windows[i];
            if (win->state == WIN_CLOSED) continue;
            if (win->on_poll) {
                win->on_poll(win);
            }
        }

        // Handle mouse events
        desktop_handle_mouse(ds);

        // Compose and present
        desktop_compose(ds);
        ds->fb.flip();

        // Target ~60fps
        montauk::sleep_ms(16);
    }
}

// ============================================================================
// Entry Point
// ============================================================================

static DesktopState* g_desktop;

extern "C" void _start() {
    DesktopState* ds = (DesktopState*)montauk::malloc(sizeof(DesktopState));
    montauk::memset(ds, 0, sizeof(DesktopState));

    // Placement-new the Framebuffer since it has a constructor
    new (&ds->fb) Framebuffer();

    g_desktop = ds;

    desktop_init(ds);
    desktop_run(ds);

    montauk::exit(0);
}
