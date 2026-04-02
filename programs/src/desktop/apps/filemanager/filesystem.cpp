/*
    * filesystem.cpp
    * Filesystem enumeration and copy/delete helpers for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"
#include <montauk/toml.h>

namespace filemanager {

void filemanager_read_drives(FileManagerState* fm) {
    fm->entry_count = 0;
    fm->at_drives_root = true;
    fm->current_path[0] = '\0';

    // Home folder entry (if we have a valid home directory)
    DesktopState* ds = fm->desktop;
    if (ds && ds->home_dir[0] != '\0') {
        int i = fm->entry_count;
        montauk::strncpy(fm->entry_names[i], "Home", 63);
        fm->entry_types[i] = 4; // home
        fm->entry_sizes[i] = 0;
        fm->is_dir[i] = true;
        fm->drive_indices[i] = -1;
        fm->entry_count++;
    }

    // Apps entry
    {
        int i = fm->entry_count;
        montauk::strncpy(fm->entry_names[i], "Apps", 63);
        fm->entry_types[i] = 5; // apps
        fm->entry_sizes[i] = 0;
        fm->is_dir[i] = true;
        fm->drive_indices[i] = -1;
        fm->entry_count++;
    }

    // Special user folders (Documents, Desktop, Music, etc.) - only if they exist
    if (ds && ds->home_dir[0] != '\0') {
        for (int sf = 0; sf < SF_COUNT && fm->entry_count < 64; sf++) {
            char probe_path[256];
            montauk::strcpy(probe_path, ds->home_dir);
            int plen = montauk::slen(probe_path);
            if (plen > 0 && probe_path[plen - 1] != '/')
                str_append(probe_path, "/", 256);
            str_append(probe_path, sf_names[sf], 256);

            // Probe: try open to check if the directory exists
            int probe_fd = montauk::open(probe_path);
            if (probe_fd >= 0) {
                montauk::close(probe_fd);
                int i = fm->entry_count;
                montauk::strncpy(fm->entry_names[i], sf_names[sf], 63);
                fm->entry_types[i] = 7; // special_dir
                fm->entry_sizes[i] = 0;
                fm->is_dir[i] = true;
                fm->drive_indices[i] = sf; // special folder index
                fm->entry_count++;
            }
        }
    }

    // Drive entries
    int drives[FM_MAX_DRIVES];
    int driveCount = montauk::drivelist(drives, FM_MAX_DRIVES);

    for (int di = 0; di < driveCount; di++) {
        int d = drives[di];
        int i = fm->entry_count;
        char probe[8];
        if (d < 10) {
            probe[0] = '0' + d;
            probe[1] = ':';
            probe[2] = '/';
            probe[3] = '\0';
        } else {
            probe[0] = '1';
            probe[1] = '0' + (d - 10);
            probe[2] = ':';
            probe[3] = '/';
            probe[4] = '\0';
        }
        char label[64];
        montauk::strcpy(label, "Drive ");
        str_append(label, probe, 64);
        montauk::strncpy(fm->entry_names[i], label, 63);
        fm->entry_types[i] = 3; // drive
        fm->entry_sizes[i] = 0;
        fm->is_dir[i] = true;
        fm->drive_indices[i] = d;
        fm->entry_count++;
        if (fm->entry_count >= 64) break;
    }

    fm->selected = -1;
    fm->scroll_offset = 0;
    fm->scrollbar.scroll_offset = 0;
    fm->last_click_item = -1;
    fm->last_click_time = 0;
}

void filemanager_read_dir(FileManagerState* fm) {
    fm->at_drives_root = false;
    if (fm->at_apps_view) {
        fm->at_apps_view = false;
        filemanager_free_app_icons(fm);
    }
    const char* names[64];
    fm->entry_count = montauk::readdir(fm->current_path, names, 64);
    if (fm->entry_count < 0) fm->entry_count = 0;

    // readdir returns full paths from the VFS (e.g. "man/fetch.1" instead
    // of just "fetch.1").  Compute the prefix to strip so we get basenames.
    const char* after_drive = fm->current_path;
    for (int k = 0; after_drive[k]; k++) {
        if (after_drive[k] == ':' && after_drive[k + 1] == '/') {
            after_drive += k + 2;
            break;
        }
    }
    char prefix[256] = {0};
    int prefix_len = 0;
    if (after_drive[0] != '\0') {
        montauk::strcpy(prefix, after_drive);
        prefix_len = montauk::slen(prefix);
        if (prefix_len > 0 && prefix[prefix_len - 1] != '/') {
            prefix[prefix_len++] = '/';
            prefix[prefix_len] = '\0';
        }
    }

    for (int i = 0; i < fm->entry_count; i++) {
        const char* raw = names[i];
        // Strip directory prefix if it matches
        if (prefix_len > 0) {
            bool match = true;
            for (int k = 0; k < prefix_len; k++) {
                if (raw[k] != prefix[k]) { match = false; break; }
            }
            if (match) raw += prefix_len;
        }
        montauk::strncpy(fm->entry_names[i], raw, 63);
        int len = montauk::slen(fm->entry_names[i]);

        // Detect directory
        if (len > 0 && fm->entry_names[i][len - 1] == '/') {
            fm->is_dir[i] = true;
            fm->entry_names[i][len - 1] = '\0';
        } else {
            fm->is_dir[i] = false;
        }

        fm->entry_types[i] = detect_file_type(fm->entry_names[i], fm->is_dir[i]);

        // Get file size
        fm->entry_sizes[i] = 0;
        if (!fm->is_dir[i]) {
            char fullpath[512];
            montauk::strcpy(fullpath, fm->current_path);
            int plen = montauk::slen(fullpath);
            if (plen > 0 && fullpath[plen - 1] != '/') {
                str_append(fullpath, "/", 512);
            }
            str_append(fullpath, fm->entry_names[i], 512);
            int fd = montauk::open(fullpath);
            if (fd >= 0) {
                fm->entry_sizes[i] = (int)montauk::getsize(fd);
                montauk::close(fd);
            }
        }
    }

    // Sort: directories first, then alphabetical (case-insensitive)
    for (int i = 1; i < fm->entry_count; i++) {
        char tmp_name[64];
        int tmp_type = fm->entry_types[i];
        int tmp_size = fm->entry_sizes[i];
        bool tmp_isdir = fm->is_dir[i];
        montauk::strcpy(tmp_name, fm->entry_names[i]);

        int j = i - 1;
        while (j >= 0) {
            bool swap = false;
            if (tmp_isdir && !fm->is_dir[j]) {
                swap = true;
            } else if (tmp_isdir == fm->is_dir[j]) {
                if (str_compare_ci(tmp_name, fm->entry_names[j]) < 0) {
                    swap = true;
                }
            }
            if (!swap) break;

            montauk::strcpy(fm->entry_names[j + 1], fm->entry_names[j]);
            fm->entry_types[j + 1] = fm->entry_types[j];
            fm->entry_sizes[j + 1] = fm->entry_sizes[j];
            fm->is_dir[j + 1] = fm->is_dir[j];
            j--;
        }
        montauk::strcpy(fm->entry_names[j + 1], tmp_name);
        fm->entry_types[j + 1] = tmp_type;
        fm->entry_sizes[j + 1] = tmp_size;
        fm->is_dir[j + 1] = tmp_isdir;
    }

    fm->selected = -1;
    fm->scroll_offset = 0;
    fm->scrollbar.scroll_offset = 0;
    fm->last_click_item = -1;
    fm->last_click_time = 0;
}

void filemanager_free_app_icons(FileManagerState* fm) {
    for (int i = 0; i < fm->app_icon_count; i++) {
        if (fm->app_icons_lg[i].pixels) {
            montauk::mfree(fm->app_icons_lg[i].pixels);
            fm->app_icons_lg[i].pixels = nullptr;
        }
        if (fm->app_icons_sm[i].pixels) {
            montauk::mfree(fm->app_icons_sm[i].pixels);
            fm->app_icons_sm[i].pixels = nullptr;
        }
    }
    fm->app_icon_count = 0;
}

void filemanager_read_apps(FileManagerState* fm) {
    filemanager_free_app_icons(fm);

    fm->at_drives_root = false;
    fm->at_apps_view = true;
    fm->entry_count = 0;

    DesktopState* ds = fm->desktop;
    if (!ds) return;

    // Scan manifests directly (like desktop_scan_apps but load icons at FM sizes)
    montauk::fmkdir("0:/apps");
    const char* entries[32];
    int count = montauk::readdir("0:/apps", entries, 32);

    // Extract basenames from readdir results (strip "apps/" prefix)

    /*
        TODO: Fix crash if 16-app ceiling is removed and remove ceiling
    */
    for (int i = 0; i < count && fm->entry_count < 16; i++) {
        const char* raw = entries[i];
        // Strip "apps/" prefix
        if (raw[0] == 'a' && raw[1] == 'p' && raw[2] == 'p' && raw[3] == 's' && raw[4] == '/')
            raw += 5;
        char dirname[64];
        montauk::strncpy(dirname, raw, 63);
        int dlen = montauk::slen(dirname);
        if (dlen > 0 && dirname[dlen - 1] == '/') dirname[dlen - 1] = '\0';
        if (dirname[0] == '\0') continue;

        // Open manifest
        char manifest_path[128];
        snprintf(manifest_path, 128, "0:/apps/%s/manifest.toml", dirname);
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

        const char* name = doc.get_string("app.name", "Unknown");
        const char* binary = doc.get_string("app.binary", "");
        const char* icon_file = doc.get_string("app.icon", "");

        int idx = fm->entry_count;
        montauk::strncpy(fm->entry_names[idx], name, 63);
        fm->entry_types[idx] = 6; // app entry
        fm->entry_sizes[idx] = 0;
        fm->is_dir[idx] = false;

        // Store binary path in drive_indices as an app_map index
        fm->app_map[idx] = -1;
        // Find matching external_app by binary path
        char bin_path[128];
        snprintf(bin_path, 128, "0:/apps/%s/%s", dirname, binary);
        for (int x = 0; x < ds->external_app_count; x++) {
            if (montauk::streq(ds->external_apps[x].binary_path, bin_path)) {
                fm->app_map[idx] = x;
                break;
            }
        }

        // Load icons at file manager sizes
        fm->app_icons_lg[idx] = {};
        fm->app_icons_sm[idx] = {};
        if (icon_file[0]) {
            char icon_path[128];
            snprintf(icon_path, 128, "0:/apps/%s/%s", dirname, icon_file);
            Color defColor = colors::ICON_COLOR;
            fm->app_icons_lg[idx] = svg_load(icon_path, 48, 48, defColor);
            fm->app_icons_sm[idx] = svg_load(icon_path, 16, 16, defColor);
        }

        doc.destroy();
        fm->entry_count++;
        fm->app_icon_count = fm->entry_count;
    }

    fm->selected = -1;
    fm->scroll_offset = 0;
    fm->scrollbar.scroll_offset = 0;
    fm->last_click_item = -1;
    fm->last_click_time = 0;
}

bool filemanager_delete_recursive(const char* path) {
    // Try deleting as a file (or empty directory) first
    if (montauk::fdelete(path) == 0) return true;

    // If that failed, it may be a non-empty directory -- enumerate and delete children
    const char* names[64];
    int count = montauk::readdir(path, names, 64);
    if (count < 0) return false;

    // Compute the prefix to strip (readdir returns paths relative to drive root)
    const char* after_drive = path;
    for (int k = 0; after_drive[k]; k++) {
        if (after_drive[k] == ':' && after_drive[k + 1] == '/') {
            after_drive += k + 2;
            break;
        }
    }
    char prefix[256] = {0};
    int prefix_len = 0;
    if (after_drive[0] != '\0') {
        montauk::strcpy(prefix, after_drive);
        prefix_len = montauk::slen(prefix);
        if (prefix_len > 0 && prefix[prefix_len - 1] != '/') {
            prefix[prefix_len++] = '/';
            prefix[prefix_len] = '\0';
        }
    }

    for (int i = 0; i < count; i++) {
        const char* raw = names[i];
        if (prefix_len > 0) {
            bool match = true;
            for (int k = 0; k < prefix_len; k++) {
                if (raw[k] != prefix[k]) { match = false; break; }
            }
            if (match) raw += prefix_len;
        }

        char child[512];
        montauk::strcpy(child, path);
        int plen = montauk::slen(child);
        if (plen > 0 && child[plen - 1] != '/')
            str_append(child, "/", 512);
        str_append(child, raw, 512);

        // Strip trailing slash if present (directory marker)
        int clen = montauk::slen(child);
        if (clen > 0 && child[clen - 1] == '/') child[clen - 1] = '\0';

        filemanager_delete_recursive(child);
    }

    // Now the directory should be empty -- delete it
    return montauk::fdelete(path) == 0;
}

bool filemanager_copy_file(const char* src, const char* dst) {
    int sfd = montauk::open(src);
    if (sfd < 0) return false;
    int size = (int)montauk::getsize(sfd);

    // Create destination
    int dfd = montauk::fcreate(dst);
    if (dfd < 0) {
        montauk::close(sfd);
        return false;
    }

    if (size > 0) {
        // Cap at 4 MB to avoid exhausting memory
        if (size > 4 * 1024 * 1024) {
            montauk::close(sfd);
            montauk::close(dfd);
            montauk::fdelete(dst);
            return false;
        }
        uint8_t* buf = (uint8_t*)montauk::malloc(size);
        if (!buf) {
            montauk::close(sfd);
            montauk::close(dfd);
            montauk::fdelete(dst);
            return false;
        }
        montauk::read(sfd, buf, 0, size);
        montauk::fwrite(dfd, buf, 0, size);
        montauk::mfree(buf);
    }

    montauk::close(sfd);
    montauk::close(dfd);
    return true;
}

// Recursive directory copy
bool filemanager_copy_dir_recursive(const char* src, const char* dst) {
    montauk::fmkdir(dst);

    const char* names[64];
    int count = montauk::readdir(src, names, 64);
    if (count < 0) return false;

    // Compute prefix to strip
    const char* after_drive = src;
    for (int k = 0; after_drive[k]; k++) {
        if (after_drive[k] == ':' && after_drive[k + 1] == '/') {
            after_drive += k + 2;
            break;
        }
    }
    char prefix[256] = {0};
    int prefix_len = 0;
    if (after_drive[0] != '\0') {
        montauk::strcpy(prefix, after_drive);
        prefix_len = montauk::slen(prefix);
        if (prefix_len > 0 && prefix[prefix_len - 1] != '/') {
            prefix[prefix_len++] = '/';
            prefix[prefix_len] = '\0';
        }
    }

    for (int i = 0; i < count; i++) {
        const char* raw = names[i];
        if (prefix_len > 0) {
            bool match = true;
            for (int k = 0; k < prefix_len; k++) {
                if (raw[k] != prefix[k]) { match = false; break; }
            }
            if (match) raw += prefix_len;
        }

        bool child_is_dir = false;
        char basename[64];
        montauk::strncpy(basename, raw, 63);
        int blen = montauk::slen(basename);
        if (blen > 0 && basename[blen - 1] == '/') {
            child_is_dir = true;
            basename[blen - 1] = '\0';
        }

        char src_child[512], dst_child[512];
        filemanager_build_fullpath(src_child, 512, src, basename);
        filemanager_build_fullpath(dst_child, 512, dst, basename);

        if (child_is_dir)
            filemanager_copy_dir_recursive(src_child, dst_child);
        else
            filemanager_copy_file(src_child, dst_child);
    }

    return true;
}

} // namespace filemanager
