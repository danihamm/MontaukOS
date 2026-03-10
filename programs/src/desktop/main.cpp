/*
    * main.cpp
    * MontaukOS Desktop Environment - initialization, external window polling, run loop
    * Copyright (c) 2026 Daniel Hammer
*/

#include "desktop_internal.hpp"

// ============================================================================
// App Manifest Scanning
// ============================================================================

// Extract the basename from a readdir entry.
// readdir returns full paths from drive root (e.g. "apps/doom/") —
// strip the directory prefix and any trailing slash to get just "doom".
static void extract_basename(char* out, int outSz, const char* entry) {
    // Strip trailing slash
    int len = montauk::slen(entry);
    while (len > 0 && entry[len - 1] == '/') len--;

    // Find last slash before that
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (entry[i] == '/') last_slash = i;
    }

    const char* base = (last_slash >= 0) ? entry + last_slash + 1 : entry;
    int base_len = len - (last_slash >= 0 ? last_slash + 1 : 0);
    if (base_len >= outSz) base_len = outSz - 1;
    montauk::memcpy(out, base, base_len);
    out[base_len] = '\0';
}

void desktop_scan_apps(DesktopState* ds) {
    ds->external_app_count = 0;

    // Ensure the apps directory exists
    montauk::fmkdir("0:/apps");

    const char* entries[32];
    int count = montauk::readdir("0:/apps", entries, 32);

    for (int i = 0; i < count && ds->external_app_count < MAX_EXTERNAL_APPS; i++) {
        // readdir returns paths like "apps/doom/" — extract just "doom"
        char dirname[64];
        extract_basename(dirname, sizeof(dirname), entries[i]);
        if (dirname[0] == '\0') continue;

        // Try to open the manifest in this app directory
        char manifest_path[128];
        snprintf(manifest_path, sizeof(manifest_path), "0:/apps/%s/manifest.toml", dirname);

        int fh = montauk::open(manifest_path);
        if (fh < 0) continue;

        uint64_t sz = montauk::getsize(fh);
        if (sz == 0 || sz > 4096) { montauk::close(fh); continue; }

        char* text = (char*)montauk::malloc(sz + 1);
        montauk::read(fh, (uint8_t*)text, 0, sz);
        montauk::close(fh);
        text[sz] = '\0';

        auto doc = montauk::toml::parse(text);
        montauk::mfree(text);

        ExternalApp* app = &ds->external_apps[ds->external_app_count];

        // Read manifest fields
        const char* name = doc.get_string("app.name", "Unknown");
        const char* binary = doc.get_string("app.binary", "");
        const char* icon_file = doc.get_string("app.icon", "");
        const char* category = doc.get_string("menu.category", "Applications");
        bool visible = doc.get_bool("menu.visible", true);

        montauk::strncpy(app->name, name, sizeof(app->name));
        montauk::strncpy(app->category, category, sizeof(app->category));
        app->menu_visible = visible;

        // Build full binary path: 0:/apps/<dir>/<binary>
        snprintf(app->binary_path, sizeof(app->binary_path),
                 "0:/apps/%s/%s", dirname, binary);

        // Load icon from app directory
        app->icon = {};
        if (icon_file[0]) {
            char icon_path[128];
            snprintf(icon_path, sizeof(icon_path),
                     "0:/apps/%s/%s", dirname, icon_file);
            Color defColor = colors::ICON_COLOR;
            app->icon = svg_load(icon_path, 20, 20, defColor);
        }

        doc.destroy();
        ds->external_app_count++;
    }
}

// ============================================================================
// Menu Builder
// ============================================================================

// Category definitions and their ordering
static const char* CATEGORY_NAMES[] = { "Applications", "Internet", "System", "Games" };
static constexpr int NUM_CATEGORIES = 4;

// Embedded app definitions: { display_name, app_id, category_index }
struct EmbeddedAppDef {
    const char* name;
    int app_id;
    int category;   // index into CATEGORY_NAMES
};

static const EmbeddedAppDef embedded_apps[] = {
    { "Terminal",        0,  0 },
    { "Files",           1,  0 },
    { "Text Editor",     4,  0 },
    { "Word Processor", 15,  0 },
    { "Calculator",      3,  0 },
    { "System Info",     2,  2 },
    { "Kernel Log",      5,  2 },
    { "Processes",       6,  2 },
    { "Mandelbrot",      7,  3 },
};

static constexpr int NUM_EMBEDDED = sizeof(embedded_apps) / sizeof(embedded_apps[0]);

// Resolve embedded app_id to an icon pointer in DesktopState
static SvgIcon* icon_for_embedded(DesktopState* ds, int app_id) {
    switch (app_id) {
    case 0:  return &ds->icon_terminal;
    case 1:  return &ds->icon_filemanager;
    case 2:  return &ds->icon_sysinfo;
    case 3:  return &ds->icon_calculator;
    case 4:  return &ds->icon_texteditor;
    case 5:  return &ds->icon_terminal;    // Kernel Log uses terminal icon
    case 6:  return &ds->icon_procmgr;
    case 7:  return &ds->icon_mandelbrot;
    case 15: return &ds->icon_texteditor;  // Word Processor uses text editor icon
    default: return nullptr;
    }
}

void desktop_build_menu(DesktopState* ds) {
    menu_row_count = 0;

    // Build each category
    for (int cat = 0; cat < NUM_CATEGORIES; cat++) {
        menu_add_category(CATEGORY_NAMES[cat]);

        // Add embedded apps in this category
        for (int e = 0; e < NUM_EMBEDDED; e++) {
            if (embedded_apps[e].category == cat) {
                menu_add_embedded(embedded_apps[e].name, embedded_apps[e].app_id,
                                  icon_for_embedded(ds, embedded_apps[e].app_id));
            }
        }

        // Add external apps in this category
        for (int x = 0; x < ds->external_app_count; x++) {
            ExternalApp* app = &ds->external_apps[x];
            if (!app->menu_visible) continue;
            if (montauk::streq(app->category, CATEGORY_NAMES[cat])) {
                menu_add_external(app->name, app->binary_path, &app->icon);
            }
        }
    }

    // Divider + always-visible entries
    menu_add_category("");  // divider (cat 4, always expanded)
    menu_add_embedded("Settings", 11, &ds->icon_settings);
    menu_add_embedded("Reboot",   12, &ds->icon_reboot);
    menu_add_embedded("Shutdown", 14, &ds->icon_shutdown);
}

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
    // drive icons loaded lazily by file manager to reduce startup heap pressure

    ds->icon_settings = svg_load("0:/icons/help-about.svg",     20, 20, defColor);
    ds->icon_reboot   = svg_load("0:/icons/system-reboot.svg", 20, 20, defColor);
    ds->icon_shutdown = svg_load("0:/icons/system-shutdown.svg", 20, 20, defColor);

    ds->icon_procmgr    = svg_load("0:/icons/system-monitor.svg",        20, 20, defColor);
    ds->icon_mandelbrot = svg_load("0:/icons/applications-science.svg",  20, 20, defColor);

    // Scan 0:/apps/ for external app manifests and build the menu
    desktop_scan_apps(ds);
    desktop_build_menu(ds);

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
    wallpaper_load(&ds->settings, "0:/home/lucas-alexander-2dJn8XoIKCg-unsplash.jpg",
                   ds->screen_w, ds->screen_h);
    montauk::win_setscale(1);

    ds->ctx_menu_open = false;
    ds->ctx_menu_x = 0;
    ds->ctx_menu_y = 0;

    ds->net_popup_open = false;
    montauk::get_netcfg(&ds->cached_net_cfg);
    ds->net_cfg_last_poll = montauk::get_milliseconds();
    ds->net_icon_rect = {0, 0, 0, 0};

    ds->closing_ext_count = 0;

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
            // Skip windows we've already sent a close event to — the owning
            // process hasn't destroyed them yet, so they still appear in
            // win_enumerate.  Re-creating them would place them at the
            // default position instead of where the user dragged them.
            bool closing = false;
            for (int c = 0; c < ds->closing_ext_count; c++) {
                if (ds->closing_ext_ids[c] == extId) { closing = true; break; }
            }
            if (closing) continue;

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

    // Purge closing IDs that the kernel no longer reports (fully destroyed)
    for (int c = ds->closing_ext_count - 1; c >= 0; c--) {
        bool alive = false;
        for (int e = 0; e < extCount; e++) {
            if (extWins[e].id == ds->closing_ext_ids[c]) { alive = true; break; }
        }
        if (!alive) {
            ds->closing_ext_ids[c] = ds->closing_ext_ids[--ds->closing_ext_count];
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

        // Re-poll external windows so that any killed during mouse/key
        // handling are removed before we touch their pixel buffers.
        desktop_poll_external_windows(ds);

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
