/*
    * actions.cpp
    * Clipboard, rename, delete, and context-menu actions for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"

namespace filemanager {

void filemanager_do_copy(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    filemanager_build_fullpath(fm->clipboard_path, 256,
                               fm->current_path, fm->entry_names[fm->selected]);
    fm->clipboard_has_data = true;
    fm->clipboard_is_cut = false;
}

void filemanager_do_cut(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    filemanager_build_fullpath(fm->clipboard_path, 256,
                               fm->current_path, fm->entry_names[fm->selected]);
    fm->clipboard_has_data = true;
    fm->clipboard_is_cut = true;
}

void filemanager_do_paste(FileManagerState* fm) {
    if (!fm->clipboard_has_data || fm->at_drives_root) return;

    const char* basename = path_basename(fm->clipboard_path);
    char dst[512];
    filemanager_build_fullpath(dst, 512, fm->current_path, basename);

    // Check if source is a directory by trying to readdir it
    const char* probe[1];
    int probe_count = montauk::readdir(fm->clipboard_path, probe, 1);
    bool src_is_dir = (probe_count >= 0);

    bool ok;
    if (src_is_dir)
        ok = filemanager_copy_dir_recursive(fm->clipboard_path, dst);
    else
        ok = filemanager_copy_file(fm->clipboard_path, dst);

    if (ok && fm->clipboard_is_cut) {
        if (src_is_dir)
            filemanager_delete_recursive(fm->clipboard_path);
        else
            montauk::fdelete(fm->clipboard_path);
        fm->clipboard_has_data = false;
    }

    filemanager_read_dir(fm);
}

void filemanager_start_rename(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    fm->rename_active = true;
    fm->rename_idx = fm->selected;
    montauk::strcpy(fm->rename_buf, fm->entry_names[fm->selected]);
    fm->rename_len = montauk::slen(fm->rename_buf);
    fm->rename_cursor = fm->rename_len;
}

void filemanager_finish_rename(FileManagerState* fm) {
    if (!fm->rename_active) return;
    fm->rename_active = false;

    // If name unchanged, skip
    if (montauk::streq(fm->rename_buf, fm->entry_names[fm->rename_idx])) return;
    if (fm->rename_len == 0) return;

    char old_path[512], new_path[512];
    filemanager_build_fullpath(old_path, 512,
                               fm->current_path, fm->entry_names[fm->rename_idx]);
    filemanager_build_fullpath(new_path, 512,
                               fm->current_path, fm->rename_buf);

    if (fm->is_dir[fm->rename_idx]) {
        // Directory rename: copy recursively then delete old
        if (filemanager_copy_dir_recursive(old_path, new_path))
            filemanager_delete_recursive(old_path);
    } else {
        // File rename: copy then delete old
        if (filemanager_copy_file(old_path, new_path))
            montauk::fdelete(old_path);
    }

    filemanager_read_dir(fm);
}

void filemanager_cancel_rename(FileManagerState* fm) {
    fm->rename_active = false;
}

void filemanager_new_folder(FileManagerState* fm) {
    if (fm->at_drives_root) return;

    // Find a unique name
    char name[64] = "New Folder";
    char path[512];
    filemanager_build_fullpath(path, 512, fm->current_path, name);
    int attempt = 1;
    // Try creating; if it fails, append a number
    if (montauk::fmkdir(path) != 0) {
        for (attempt = 2; attempt <= 99; attempt++) {
            snprintf(name, 64, "New Folder %d", attempt);
            filemanager_build_fullpath(path, 512, fm->current_path, name);
            if (montauk::fmkdir(path) == 0) break;
        }
    }

    filemanager_read_dir(fm);

    // Select the new folder and start rename
    for (int i = 0; i < fm->entry_count; i++) {
        if (montauk::streq(fm->entry_names[i], name)) {
            fm->selected = i;
            filemanager_start_rename(fm);
            break;
        }
    }
}

void filemanager_delete_selected(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    char fullpath[512];
    filemanager_build_fullpath(fullpath, 512,
                               fm->current_path, fm->entry_names[fm->selected]);
    if (fm->is_dir[fm->selected])
        filemanager_delete_recursive(fullpath);
    else
        montauk::fdelete(fullpath);
    filemanager_read_dir(fm);
}

void filemanager_open_ctx_menu(FileManagerState* fm, int local_x, int local_y, int target_idx) {
    fm->ctx_open = true;
    fm->ctx_x = local_x;
    fm->ctx_y = local_y;
    fm->ctx_target_idx = target_idx;
    fm->ctx_hover = -1;
    fm->ctx_item_count = 0;

    if (target_idx >= 0 && target_idx < fm->entry_count) {
        int tt = fm->entry_types[target_idx];
        if ((fm->at_drives_root && (tt == 3 || tt == 4 || tt == 5 || tt == 7)) ||
            (fm->at_apps_view && tt == 6)) {
            // Drive, Home, Apps shortcut, or app entry: only Open
            fm->ctx_items[fm->ctx_item_count++] = CTX_OPEN;
        } else {
            fm->ctx_items[fm->ctx_item_count++] = CTX_OPEN;
            fm->ctx_items[fm->ctx_item_count++] = CTX_COPY;
            fm->ctx_items[fm->ctx_item_count++] = CTX_CUT;
            fm->ctx_items[fm->ctx_item_count++] = CTX_RENAME;
            fm->ctx_items[fm->ctx_item_count++] = CTX_DELETE;
        }
    } else {
        // Background click
        if (fm->clipboard_has_data)
            fm->ctx_items[fm->ctx_item_count++] = CTX_PASTE;
        fm->ctx_items[fm->ctx_item_count++] = CTX_NEW_FOLDER;
    }
}

void filemanager_close_ctx_menu(FileManagerState* fm) {
    fm->ctx_open = false;
}

} // namespace filemanager
