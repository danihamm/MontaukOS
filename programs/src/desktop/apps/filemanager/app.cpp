/*
    * app.cpp
    * File manager launcher and desktop integration entry points
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"

namespace filemanager {

static void open_filemanager_internal(DesktopState* ds, const char* initial_path) {
    int idx = desktop_create_window(ds, "Files", 150, 120, 560, 420);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];
    FileManagerState* fm = (FileManagerState*)montauk::malloc(sizeof(FileManagerState));
    montauk::memset(fm, 0, sizeof(FileManagerState));
    fm->selected = -1;
    fm->last_click_item = -1;
    fm->history_pos = -1;
    fm->history_count = 0;
    fm->desktop = ds;
    fm->grid_view = true;

    fm->scrollbar.init(0, 0, FM_SCROLLBAR_W, 100);

    ensure_filemanager_icons_loaded(ds);

    if (initial_path && initial_path[0] != '\0') {
        int probe_fd = montauk::open(initial_path);
        if (probe_fd >= 0) {
            montauk::close(probe_fd);
            montauk::strncpy(fm->current_path, initial_path, sizeof(fm->current_path));
            filemanager_read_dir(fm);
        } else {
            filemanager_read_drives(fm);
        }
    } else {
        filemanager_read_drives(fm);
    }
    filemanager_push_history(fm);

    win->app_data = fm;
    win->on_draw = filemanager_on_draw;
    win->on_mouse = filemanager_on_mouse;
    win->on_key = filemanager_on_key;
    win->on_close = filemanager_on_close;
}

} // namespace filemanager

void ensure_filemanager_icons_loaded(DesktopState* ds) {
    if (!ds || ds->icon_drive.pixels) return;

    Color defColor = colors::ICON_COLOR;
    ds->icon_drive          = svg_load("0:/icons/drive-harddisk.svg", 16, 16, defColor);
    ds->icon_drive_lg       = svg_load("0:/icons/drive-harddisk.svg", 48, 48, defColor);
    ds->icon_home_folder    = svg_load("0:/icons/folder-blue-home.svg", 16, 16, defColor);
    ds->icon_home_folder_lg = svg_load("0:/icons/folder-blue-home.svg", 48, 48, defColor);
    ds->icon_apps           = svg_load("0:/icons/folder-blue-development.svg", 16, 16, defColor);
    ds->icon_apps_lg        = svg_load("0:/icons/folder-blue-development.svg", 48, 48, defColor);

    for (int sf = 0; sf < filemanager::SF_COUNT; sf++) {
        char icon_path[128];
        snprintf(icon_path, 128, "0:/icons/%s", filemanager::sf_icons[sf]);
        ds->icon_special_folder[sf]    = svg_load(icon_path, 16, 16, defColor);
        ds->icon_special_folder_lg[sf] = svg_load(icon_path, 48, 48, defColor);
    }
    ds->icon_delete     = svg_load("0:/icons/trash-empty.svg", 16, 16, defColor);
    ds->icon_copy       = svg_load("0:/icons/edit-copy.svg", 16, 16, defColor);
    ds->icon_cut        = svg_load("0:/icons/edit-cut.svg", 16, 16, defColor);
    ds->icon_paste      = svg_load("0:/icons/edit-paste.svg", 16, 16, defColor);
    ds->icon_rename     = svg_load("0:/icons/edit-rename.svg", 16, 16, defColor);
    ds->icon_folder_new = svg_load("0:/icons/folder-new.svg", 16, 16, defColor);
}

void open_filemanager(DesktopState* ds) {
    filemanager::open_filemanager_internal(ds, nullptr);
}

void open_filemanager_path(DesktopState* ds, const char* path) {
    filemanager::open_filemanager_internal(ds, path);
}
