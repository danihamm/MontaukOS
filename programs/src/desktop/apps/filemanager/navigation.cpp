/*
    * navigation.cpp
    * Navigation and file opening logic for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"

namespace filemanager {

void filemanager_push_history(FileManagerState* fm) {
    // Don't push if same as current position
    if (fm->history_count > 0 && fm->history_pos >= 0) {
        if (montauk::streq(fm->history[fm->history_pos], fm->current_path)) return;
    }
    fm->history_pos++;
    if (fm->history_pos >= 16) fm->history_pos = 15;
    montauk::strcpy(fm->history[fm->history_pos], fm->current_path);
    fm->history_count = fm->history_pos + 1;
}

void filemanager_navigate(FileManagerState* fm, const char* name) {
    int path_len = montauk::slen(fm->current_path);
    if (path_len > 0 && fm->current_path[path_len - 1] != '/') {
        str_append(fm->current_path, "/", 256);
    }
    str_append(fm->current_path, name, 256);
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

void filemanager_go_up(FileManagerState* fm) {
    if (fm->at_drives_root) return;

    // From apps view, go back to Computer
    if (fm->at_apps_view) {
        fm->at_apps_view = false;
        filemanager_free_app_icons(fm);
        filemanager_read_drives(fm);
        filemanager_push_history(fm);
        return;
    }

    int len = montauk::slen(fm->current_path);

    // At drive root (e.g. "0:/" or "10:/") -- go to drives view
    bool is_drive_root = false;
    if (len >= 3 && fm->current_path[len - 1] == '/') {
        // Check if path is just "N:/" or "NN:/"
        int colon = -1;
        for (int i = 0; i < len; i++) {
            if (fm->current_path[i] == ':') { colon = i; break; }
        }
        if (colon >= 0 && colon + 2 == len) is_drive_root = true;
    }
    if (is_drive_root) {
        filemanager_read_drives(fm);
        filemanager_push_history(fm);
        return;
    }

    if (len > 0 && fm->current_path[len - 1] == '/') {
        fm->current_path[len - 1] = '\0';
        len--;
    }

    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (fm->current_path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash >= 0) {
        // Keep the slash only if it's the root slash (right after "N:")
        if (last_slash > 0 && fm->current_path[last_slash - 1] == ':') {
            fm->current_path[last_slash + 1] = '\0';
        } else {
            fm->current_path[last_slash] = '\0';
        }
    }
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

void filemanager_navigate_to_history(FileManagerState* fm) {
    montauk::strcpy(fm->current_path, fm->history[fm->history_pos]);
    if (fm->current_path[0] == '\0') {
        filemanager_read_drives(fm);
    } else {
        filemanager_read_dir(fm);
    }
}

void filemanager_go_back(FileManagerState* fm) {
    if (fm->history_pos <= 0) return;
    fm->history_pos--;
    filemanager_navigate_to_history(fm);
}

void filemanager_go_forward(FileManagerState* fm) {
    if (fm->history_pos >= fm->history_count - 1) return;
    fm->history_pos++;
    filemanager_navigate_to_history(fm);
}

void filemanager_go_home(FileManagerState* fm) {
    if (fm->at_apps_view) {
        fm->at_apps_view = false;
        filemanager_free_app_icons(fm);
    }
    filemanager_read_drives(fm);
    filemanager_push_history(fm);
}

void filemanager_start_pathbar(FileManagerState* fm) {
    fm->pathbar_editing = true;
    if (fm->at_drives_root) {
        fm->pathbar_buf[0] = '\0';
        fm->pathbar_len = 0;
    } else {
        montauk::strcpy(fm->pathbar_buf, fm->current_path);
        fm->pathbar_len = montauk::slen(fm->pathbar_buf);
    }
    fm->pathbar_cursor = fm->pathbar_len;
}

void filemanager_cancel_pathbar(FileManagerState* fm) {
    fm->pathbar_editing = false;
}

void filemanager_commit_pathbar(FileManagerState* fm) {
    fm->pathbar_editing = false;
    if (fm->pathbar_len == 0) {
        filemanager_read_drives(fm);
        filemanager_push_history(fm);
        return;
    }
    montauk::strcpy(fm->current_path, fm->pathbar_buf);
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

void filemanager_open_entry(FileManagerState* fm, int idx) {
    if (idx < 0 || idx >= fm->entry_count) return;

    // Special user folder in Computer view
    if (fm->at_drives_root && fm->entry_types[idx] == 7) {
        DesktopState* ds = fm->desktop;
        if (ds && ds->home_dir[0] != '\0') {
            montauk::strcpy(fm->current_path, ds->home_dir);
            int plen = montauk::slen(fm->current_path);
            if (plen > 0 && fm->current_path[plen - 1] != '/')
                str_append(fm->current_path, "/", 256);
            str_append(fm->current_path, fm->entry_names[idx], 256);
            filemanager_push_history(fm);
            filemanager_read_dir(fm);
        }
        return;
    }

    // Apps shortcut in Computer view
    if (fm->at_drives_root && fm->entry_types[idx] == 5) {
        filemanager_read_apps(fm);
        return;
    }

    // App entry in apps view - launch it
    if (fm->at_apps_view && fm->entry_types[idx] == 6) {
        DesktopState* ds = fm->desktop;
        if (ds && fm->app_map[idx] >= 0 && fm->app_map[idx] < ds->external_app_count) {
            const ExternalApp& app = ds->external_apps[fm->app_map[idx]];
            if (app.launch_with_home) {
                montauk::spawn(app.binary_path, ds->home_dir);
            } else {
                montauk::spawn(app.binary_path);
            }
        }
        return;
    }

    if (fm->at_drives_root && fm->entry_types[idx] == 4) {
        // Home folder
        DesktopState* ds = fm->desktop;
        if (ds && ds->home_dir[0] != '\0') {
            montauk::strcpy(fm->current_path, ds->home_dir);
            filemanager_push_history(fm);
            filemanager_read_dir(fm);
        }
        return;
    }

    if (fm->at_drives_root && fm->entry_types[idx] == 3) {
        int d = fm->drive_indices[idx];
        char dpath[8];
        if (d < 10) {
            dpath[0] = '0' + d; dpath[1] = ':'; dpath[2] = '/'; dpath[3] = '\0';
        } else {
            dpath[0] = '1'; dpath[1] = '0' + (d - 10); dpath[2] = ':'; dpath[3] = '/'; dpath[4] = '\0';
        }
        montauk::strcpy(fm->current_path, dpath);
        filemanager_push_history(fm);
        filemanager_read_dir(fm);
        return;
    }

    if (fm->is_dir[idx]) {
        filemanager_navigate(fm, fm->entry_names[idx]);
        return;
    }

    // Open file with appropriate app
    char fullpath[512];
    filemanager_build_fullpath(fullpath, 512, fm->current_path, fm->entry_names[idx]);
    if (is_image_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/imageviewer/imageviewer.elf", fullpath);
    } else if (is_font_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/fontpreview/fontpreview.elf", fullpath);
    } else if (is_pdf_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/pdfviewer/pdfviewer.elf", fullpath);
    } else if (is_spreadsheet_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/spreadsheet/spreadsheet.elf", fullpath);
    } else if (is_wordprocessor_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/wordprocessor/wordprocessor.elf", fullpath);
    } else if (is_audio_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/music/music.elf", fullpath);
    } else if (is_video_file(fm->entry_names[idx])) {
        montauk::spawn("0:/apps/video/video.elf", fullpath);
    } else if (str_ends_with(fm->entry_names[idx], ".elf")) {
        montauk::spawn(fullpath);
    } else {
        montauk::spawn("0:/apps/texteditor/texteditor.elf", fullpath);
    }
}

} // namespace filemanager
