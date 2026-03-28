/*
    * app_filemanager.cpp
    * MontaukOS Desktop - Enhanced File Manager application
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"
#include <montauk/toml.h>

// ============================================================================
// File Manager state
// ============================================================================

static constexpr int FM_MAX_DRIVES = 16;

struct FileManagerState {
    char current_path[256];
    char history[16][256];
    int history_pos;
    int history_count;
    char entry_names[64][64];
    int entry_types[64];   // 0=file, 1=dir, 2=exec, 3=drive, 4=home, 5=apps, 6=app, 7=special_dir
    int entry_sizes[64];
    int entry_count;
    int selected;
    int scroll_offset;
    bool is_dir[64];
    int last_click_item;
    uint64_t last_click_time;
    Scrollbar scrollbar;
    DesktopState* desktop;
    bool grid_view;
    bool at_drives_root;
    int drive_indices[FM_MAX_DRIVES]; // which drive number each entry maps to

    // Clipboard
    char clipboard_path[256];
    bool clipboard_has_data;
    bool clipboard_is_cut;

    // Context menu
    bool ctx_open;
    int ctx_x, ctx_y;
    int ctx_target_idx; // -1 = background
    int ctx_items[8];
    int ctx_item_count;
    int ctx_hover;

    // Rename
    bool rename_active;
    int rename_idx;
    char rename_buf[64];
    int rename_cursor;
    int rename_len;

    // Path bar editing
    bool pathbar_editing;
    char pathbar_buf[256];
    int pathbar_cursor;
    int pathbar_len;

    // Apps view
    bool at_apps_view;
    int app_map[16];           // entry index -> external_apps[] index
    SvgIcon app_icons_lg[16];  // 48x48 icons for grid view
    SvgIcon app_icons_sm[16];  // 16x16 icons for list view
    int app_icon_count;
};

static constexpr int FM_TOOLBAR_H = 32;
static constexpr int FM_PATHBAR_H = 24;
static constexpr int FM_HEADER_H  = 20;
static constexpr int FM_ITEM_H    = 24;
static constexpr int FM_SCROLLBAR_W = 12;
static constexpr int FM_GRID_CELL_W = 80;
static constexpr int FM_GRID_CELL_H = 80;
static constexpr int FM_GRID_ICON   = 48;
static constexpr int FM_GRID_PAD    = 4;

// Special user folders: name, icon filename, index into DesktopState arrays
static constexpr int SF_COUNT = 6;
static const char* sf_names[SF_COUNT] = {
    "Documents", "Desktop", "Music", "Videos", "Pictures", "Downloads"
};
static const char* sf_icons[SF_COUNT] = {
    "folder-blue-documents.svg", "folder-blue-desktop.svg",
    "folder-blue-music.svg", "folder-blue-videos.svg",
    "folder-blue-pictures.svg", "folder-blue-downloads.svg"
};

// Return special folder index (0-5) or -1 if not a special folder name
static int special_folder_index(const char* name) {
    for (int i = 0; i < SF_COUNT; i++) {
        if (montauk::streq(name, sf_names[i])) return i;
    }
    return -1;
}

// Context menu action codes
static constexpr int CTX_OPEN       = 0;
static constexpr int CTX_COPY       = 1;
static constexpr int CTX_CUT        = 2;
static constexpr int CTX_PASTE      = 3;
static constexpr int CTX_RENAME     = 4;
static constexpr int CTX_DELETE     = 5;
static constexpr int CTX_NEW_FOLDER = 6;

static constexpr int CTX_MENU_W    = 140;
static constexpr int CTX_ITEM_H    = 24;

static const char* ctx_label(int action) {
    switch (action) {
    case CTX_OPEN:       return "Open";
    case CTX_COPY:       return "Copy";
    case CTX_CUT:        return "Cut";
    case CTX_PASTE:      return "Paste";
    case CTX_RENAME:     return "Rename";
    case CTX_DELETE:     return "Delete";
    case CTX_NEW_FOLDER: return "New Folder";
    default:             return "?";
    }
}

// ============================================================================
// File type detection
// ============================================================================

static bool str_ends_with(const char* s, const char* suffix) {
    int slen = montauk::slen(s);
    int suflen = montauk::slen(suffix);
    if (suflen > slen) return false;
    for (int i = 0; i < suflen; i++) {
        char sc = s[slen - suflen + i];
        char ec = suffix[i];
        if (sc >= 'A' && sc <= 'Z') sc += 32;
        if (ec >= 'A' && ec <= 'Z') ec += 32;
        if (sc != ec) return false;
    }
    return true;
}

static bool is_image_file(const char* name) {
    return str_ends_with(name, ".jpg") || str_ends_with(name, ".jpeg");
}

static bool is_font_file(const char* name) {
    return str_ends_with(name, ".ttf");
}

static bool is_pdf_file(const char* name) {
    return str_ends_with(name, ".pdf");
}

static bool is_spreadsheet_file(const char* name) {
    return str_ends_with(name, ".mss");
}

static bool is_wordprocessor_file(const char* name) {
    return str_ends_with(name, ".mwp");
}

static bool is_audio_file(const char* name) {
    return str_ends_with(name, ".mp3") || str_ends_with(name, ".wav");
}

static bool is_video_file(const char* name) {
    return str_ends_with(name, ".avi");
}

static int detect_file_type(const char* name, bool is_dir) {
    if (is_dir) return 1;
    if (str_ends_with(name, ".elf")) return 2;
    return 0;
}

// Forward declarations
static void filemanager_free_app_icons(FileManagerState* fm);

// ============================================================================
// Path helpers
// ============================================================================

static void filemanager_build_fullpath(char* out, int out_max,
                                       const char* dir, const char* name) {
    montauk::strcpy(out, dir);
    int plen = montauk::slen(out);
    if (plen > 0 && out[plen - 1] != '/') str_append(out, "/", out_max);
    str_append(out, name, out_max);
}

// Extract basename from a path (pointer into the path string)
static const char* path_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' && *(p + 1)) last = p + 1;
    }
    return last;
}

// ============================================================================
// Directory reading with sorting and file sizes
// ============================================================================

static void filemanager_read_drives(FileManagerState* fm) {
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

static void filemanager_read_dir(FileManagerState* fm) {
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

// ============================================================================
// Apps view population
// ============================================================================

static void filemanager_free_app_icons(FileManagerState* fm) {
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

static void filemanager_read_apps(FileManagerState* fm) {
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

// ============================================================================
// Recursive directory deletion
// ============================================================================

static bool filemanager_delete_recursive(const char* path) {
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

// ============================================================================
// File copy helper (single file)
// ============================================================================

static bool filemanager_copy_file(const char* src, const char* dst) {
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
static bool filemanager_copy_dir_recursive(const char* src, const char* dst) {
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

// ============================================================================
// Clipboard operations
// ============================================================================

static void filemanager_do_copy(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    filemanager_build_fullpath(fm->clipboard_path, 256,
                               fm->current_path, fm->entry_names[fm->selected]);
    fm->clipboard_has_data = true;
    fm->clipboard_is_cut = false;
}

static void filemanager_do_cut(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    filemanager_build_fullpath(fm->clipboard_path, 256,
                               fm->current_path, fm->entry_names[fm->selected]);
    fm->clipboard_has_data = true;
    fm->clipboard_is_cut = true;
}

static void filemanager_do_paste(FileManagerState* fm) {
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

// ============================================================================
// Rename operations
// ============================================================================

static void filemanager_start_rename(FileManagerState* fm) {
    if (fm->selected < 0 || fm->selected >= fm->entry_count) return;
    if (fm->at_drives_root || fm->entry_types[fm->selected] == 3) return;

    fm->rename_active = true;
    fm->rename_idx = fm->selected;
    montauk::strcpy(fm->rename_buf, fm->entry_names[fm->selected]);
    fm->rename_len = montauk::slen(fm->rename_buf);
    fm->rename_cursor = fm->rename_len;
}

static void filemanager_finish_rename(FileManagerState* fm) {
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

static void filemanager_cancel_rename(FileManagerState* fm) {
    fm->rename_active = false;
}

// ============================================================================
// New folder
// ============================================================================

static void filemanager_new_folder(FileManagerState* fm) {
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

// ============================================================================
// Delete selected entry
// ============================================================================

static void filemanager_delete_selected(FileManagerState* fm) {
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

// ============================================================================
// Context menu
// ============================================================================

static void filemanager_open_ctx_menu(FileManagerState* fm, int local_x, int local_y,
                                       int target_idx) {
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

static void filemanager_close_ctx_menu(FileManagerState* fm) {
    fm->ctx_open = false;
}

// ============================================================================
// History management
// ============================================================================

static void filemanager_push_history(FileManagerState* fm) {
    // Don't push if same as current position
    if (fm->history_count > 0 && fm->history_pos >= 0) {
        if (montauk::streq(fm->history[fm->history_pos], fm->current_path)) return;
    }
    fm->history_pos++;
    if (fm->history_pos >= 16) fm->history_pos = 15;
    montauk::strcpy(fm->history[fm->history_pos], fm->current_path);
    fm->history_count = fm->history_pos + 1;
}

static void filemanager_navigate(FileManagerState* fm, const char* name) {
    int path_len = montauk::slen(fm->current_path);
    if (path_len > 0 && fm->current_path[path_len - 1] != '/') {
        str_append(fm->current_path, "/", 256);
    }
    str_append(fm->current_path, name, 256);
    filemanager_push_history(fm);
    filemanager_read_dir(fm);
}

static void filemanager_go_up(FileManagerState* fm) {
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

static void filemanager_navigate_to_history(FileManagerState* fm) {
    montauk::strcpy(fm->current_path, fm->history[fm->history_pos]);
    if (fm->current_path[0] == '\0') {
        filemanager_read_drives(fm);
    } else {
        filemanager_read_dir(fm);
    }
}

static void filemanager_go_back(FileManagerState* fm) {
    if (fm->history_pos <= 0) return;
    fm->history_pos--;
    filemanager_navigate_to_history(fm);
}

static void filemanager_go_forward(FileManagerState* fm) {
    if (fm->history_pos >= fm->history_count - 1) return;
    fm->history_pos++;
    filemanager_navigate_to_history(fm);
}

static void filemanager_go_home(FileManagerState* fm) {
    if (fm->at_apps_view) {
        fm->at_apps_view = false;
        filemanager_free_app_icons(fm);
    }
    filemanager_read_drives(fm);
    filemanager_push_history(fm);
}

// ============================================================================
// Path bar editing
// ============================================================================

static void filemanager_start_pathbar(FileManagerState* fm) {
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

static void filemanager_cancel_pathbar(FileManagerState* fm) {
    fm->pathbar_editing = false;
}

static void filemanager_commit_pathbar(FileManagerState* fm) {
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

// ============================================================================
// Open file/directory/drive
// ============================================================================

static void filemanager_open_entry(FileManagerState* fm, int idx) {
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

// ============================================================================
// Drawing
// ============================================================================

static void filemanager_draw_header(Canvas& c, FileManagerState* fm,
                                    Color toolbar_color, Color btn_bg) {
    DesktopState* ds = fm->desktop;

    // ---- Toolbar (32px) ----
    c.fill_rect(0, 0, c.w, FM_TOOLBAR_H, toolbar_color);

    // Navigation buttons: Back, Forward, Up, Home
    struct ToolBtn { int x; SvgIcon* icon; };
    ToolBtn nav_btns[4] = {
        {  4, ds ? &ds->icon_go_back    : nullptr },
        { 32, ds ? &ds->icon_go_forward : nullptr },
        { 60, ds ? &ds->icon_go_up      : nullptr },
        { 88, ds ? &ds->icon_home       : nullptr },
    };

    for (int i = 0; i < 4; i++) {
        int bx = nav_btns[i].x;
        int by = 4;
        c.fill_rect(bx, by, 24, 24, btn_bg);
        if (nav_btns[i].icon) {
            int ix = bx + (24 - nav_btns[i].icon->width) / 2;
            int iy = by + (24 - nav_btns[i].icon->height) / 2;
            c.icon(ix, iy, *nav_btns[i].icon);
        }
    }

    // View toggle button
    {
        int bx = 120, by = 4;
        c.fill_rect(bx, by, 24, 24, btn_bg);
        if (fm->grid_view) {
            for (int r = 0; r < 2; r++)
                for (int cc = 0; cc < 2; cc++)
                    c.fill_rect(bx + 5 + cc * 8, by + 5 + r * 8, 6, 6, colors::TEXT_COLOR);
        } else {
            for (int r = 0; r < 3; r++)
                c.fill_rect(bx + 5, by + 5 + r * 5, 14, 2, colors::TEXT_COLOR);
        }
    }

    // Separator line between nav group and action group
    c.vline(152, 6, 20, colors::BORDER);

    // Action buttons: Copy, Cut, Paste, Rename, New Folder, Delete
    bool has_sel = fm->selected >= 0 && fm->selected < fm->entry_count
                   && !fm->at_drives_root && !fm->at_apps_view
                   && fm->entry_types[fm->selected] != 3;
    bool has_clip = fm->clipboard_has_data && !fm->at_drives_root && !fm->at_apps_view;
    Color dim_bg = Color::from_rgb(0xF0, 0xF0, 0xF0);

    // Icon toolbar buttons: Copy, Cut, Paste, Rename, New Folder, Delete
    struct ActionBtn { int x; SvgIcon* icon; bool enabled; };
    ActionBtn action_btns[] = {
        { 160, ds ? &ds->icon_copy       : nullptr, has_sel },
        { 188, ds ? &ds->icon_cut        : nullptr, has_sel },
        { 216, ds ? &ds->icon_paste      : nullptr, has_clip },
        { 244, ds ? &ds->icon_rename     : nullptr, has_sel },
        { 272, ds ? &ds->icon_folder_new : nullptr, !fm->at_drives_root && !fm->at_apps_view },
        { 300, ds ? &ds->icon_delete     : nullptr, has_sel },
    };

    for (int i = 0; i < 6; i++) {
        int bx = action_btns[i].x;
        int by = 4;
        c.fill_rect(bx, by, 24, 24, action_btns[i].enabled ? btn_bg : dim_bg);
        if (action_btns[i].icon && action_btns[i].icon->pixels) {
            int ix = bx + (24 - action_btns[i].icon->width) / 2;
            int iy = by + (24 - action_btns[i].icon->height) / 2;
            c.icon(ix, iy, *action_btns[i].icon);
        }
    }

    // Toolbar separator
    c.hline(0, FM_TOOLBAR_H - 1, c.w, colors::BORDER);

    // ---- Path bar ----
    int pathbar_y = FM_TOOLBAR_H;
    if (fm->pathbar_editing) {
        // Editable text input
        c.fill_rect(0, pathbar_y, c.w, FM_PATHBAR_H, colors::WHITE);
        c.rect(0, pathbar_y, c.w, FM_PATHBAR_H, colors::ACCENT);
        int fh = system_font_height();
        int ty = pathbar_y + (FM_PATHBAR_H - fh) / 2;
        c.text(8, ty, fm->pathbar_buf, colors::TEXT_COLOR);
        // Cursor
        char prefix[256];
        int cpos = fm->pathbar_cursor;
        if (cpos > 255) cpos = 255;
        for (int k = 0; k < cpos; k++) prefix[k] = fm->pathbar_buf[k];
        prefix[cpos] = '\0';
        int cx = 8 + text_width(prefix);
        c.vline(cx, ty, fh, colors::ACCENT);
    } else {
        c.fill_rect(0, pathbar_y, c.w, FM_PATHBAR_H, Color::from_rgb(0xF0, 0xF0, 0xF0));
        const char* pathbar_text = fm->current_path;
        if (fm->at_drives_root) pathbar_text = "Computer";
        else if (fm->at_apps_view) pathbar_text = "Apps";
        c.text(8, pathbar_y + 4, pathbar_text, colors::TEXT_COLOR);
    }

    // Path bar separator
    c.hline(0, pathbar_y + FM_PATHBAR_H - 1, c.w, colors::BORDER);
}

static void filemanager_on_draw(Window* win, Framebuffer& fb) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    Color toolbar_color = Color::from_rgb(0xF5, 0xF5, 0xF5);
    Color btn_bg = Color::from_rgb(0xE8, 0xE8, 0xE8);
    Color dim = Color::from_rgb(0x88, 0x88, 0x88);
    DesktopState* ds = fm->desktop;

    // Draw header (toolbar + path bar)
    filemanager_draw_header(c, fm, toolbar_color, btn_bg);

    if (fm->grid_view) {
        // ---- Grid View ----
        int list_y = FM_TOOLBAR_H + FM_PATHBAR_H;
        int list_h = c.h - list_y;
        int cols = (c.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
        if (cols < 1) cols = 1;
        int rows = (fm->entry_count + cols - 1) / cols;
        int content_h = rows * FM_GRID_CELL_H;

        // Update scrollbar
        fm->scrollbar.bounds = {c.w - FM_SCROLLBAR_W, list_y, FM_SCROLLBAR_W, list_h};
        fm->scrollbar.content_height = content_h;
        fm->scrollbar.view_height = list_h;

        for (int i = 0; i < fm->entry_count; i++) {
            int col = i % cols;
            int row = i / cols;
            int cell_x = col * FM_GRID_CELL_W;
            int cell_y = list_y + row * FM_GRID_CELL_H - fm->scrollbar.scroll_offset;

            // Skip if entirely off-screen
            if (cell_y + FM_GRID_CELL_H <= list_y || cell_y >= c.h) continue;

            // Selection highlight
            if (i == fm->selected) {
                int sy = gui_max(cell_y, list_y);
                int sh = gui_min(cell_y + FM_GRID_CELL_H, c.h) - sy;
                int sw = gui_min(FM_GRID_CELL_W, c.w - FM_SCROLLBAR_W - cell_x);
                if (sh > 0 && sw > 0)
                    c.fill_rect(cell_x, sy, sw, sh, colors::MENU_HOVER);
            }

            // Large icon centered horizontally
            int icon_x = cell_x + (FM_GRID_CELL_W - FM_GRID_ICON) / 2;
            int icon_y = cell_y + FM_GRID_PAD;
            // Check for special folder icon (type 7 in Computer, or type 1 dir with matching name)
            int sfi = -1;
            if (fm->entry_types[i] == 7) sfi = fm->drive_indices[i];
            else if (ds && fm->entry_types[i] == 1) sfi = special_folder_index(fm->entry_names[i]);

            if (sfi >= 0 && ds && ds->icon_special_folder_lg[sfi].pixels) {
                c.icon(icon_x, icon_y, ds->icon_special_folder_lg[sfi]);
            } else if (fm->entry_types[i] == 6 && i < fm->app_icon_count && fm->app_icons_lg[i].pixels) {
                c.icon(icon_x, icon_y, fm->app_icons_lg[i]);
            } else if (ds && fm->entry_types[i] == 5 && ds->icon_apps_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_apps_lg);
            } else if (ds && fm->entry_types[i] == 4 && ds->icon_home_folder_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_home_folder_lg);
            } else if (ds && fm->entry_types[i] == 3 && ds->icon_drive_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_drive_lg);
            } else if (ds && fm->entry_types[i] == 1 && ds->icon_folder_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_folder_lg);
            } else if (ds && fm->entry_types[i] == 2 && ds->icon_exec_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_exec_lg);
            } else if (ds && ds->icon_file_lg.pixels) {
                c.icon(icon_x, icon_y, ds->icon_file_lg);
            } else {
                Color icon_c = fm->is_dir[i]
                    ? Color::from_rgb(0xFF, 0xBD, 0x2E)
                    : Color::from_rgb(0x90, 0x90, 0x90);
                int iy_clip = gui_max(icon_y, list_y);
                int ih_clip = FM_GRID_ICON - (iy_clip - icon_y);
                if (ih_clip > 0)
                    c.fill_rect(icon_x, iy_clip, FM_GRID_ICON, ih_clip, icon_c);
            }

            // Filename centered below icon, truncated if needed
            // If renaming this entry, draw the rename textbox instead
            if (fm->rename_active && fm->rename_idx == i) {
                int ty = icon_y + FM_GRID_ICON + 2;
                int tw = FM_GRID_CELL_W - 4;
                if (ty >= list_y && ty + system_font_height() + 4 <= c.h) {
                    c.fill_rect(cell_x + 2, ty - 1, tw, system_font_height() + 2, colors::WHITE);
                    c.rect(cell_x + 2, ty - 1, tw, system_font_height() + 2, colors::ACCENT);
                    // Draw rename text (truncated to cell width)
                    char display[16];
                    int rlen = fm->rename_len;
                    if (rlen > 10) rlen = 10;
                    for (int k = 0; k < rlen; k++) display[k] = fm->rename_buf[k];
                    display[rlen] = '\0';
                    c.text(cell_x + 4, ty, display, colors::TEXT_COLOR);
                    // Cursor
                    char prefix[64];
                    int cpos = fm->rename_cursor < rlen ? fm->rename_cursor : rlen;
                    for (int k = 0; k < cpos; k++) prefix[k] = fm->rename_buf[k];
                    prefix[cpos] = '\0';
                    int cx = cell_x + 4 + text_width(prefix);
                    c.vline(cx, ty, system_font_height(), colors::ACCENT);
                }
            } else {
                char label[16];
                int nlen = montauk::slen(fm->entry_names[i]);
                if (nlen > 9) {
                    for (int k = 0; k < 9; k++) label[k] = fm->entry_names[i][k];
                    label[9] = '.';
                    label[10] = '.';
                    label[11] = '\0';
                } else {
                    montauk::strncpy(label, fm->entry_names[i], 15);
                }
                int tw = text_width(label);
                int tx = cell_x + (FM_GRID_CELL_W - tw) / 2;
                if (tx < cell_x) tx = cell_x;
                int ty = icon_y + FM_GRID_ICON + 2;
                if (ty >= list_y && ty + system_font_height() <= c.h)
                    c.text(tx, ty, label, colors::TEXT_COLOR);
            }
        }
    } else {
        // ---- List View ----

        // ---- Column headers ----
        int header_y = FM_TOOLBAR_H + FM_PATHBAR_H;
        c.fill_rect(0, header_y, c.w, FM_HEADER_H, Color::from_rgb(0xF8, 0xF8, 0xF8));

        int name_col_x = 8;
        int size_col_x = c.w - FM_SCROLLBAR_W - 120;
        int type_col_x = c.w - FM_SCROLLBAR_W - 60;

        c.text(name_col_x, header_y + 2, "Name", dim);
        if (size_col_x > 100)
            c.text(size_col_x, header_y + 2, "Size", dim);
        if (type_col_x > 160)
            c.text(type_col_x, header_y + 2, "Type", dim);

        // Header separator
        c.hline(0, header_y + FM_HEADER_H - 1, c.w, colors::BORDER);

        // Column separator lines
        if (size_col_x > 100) {
            c.vline(size_col_x - 4, header_y, c.h - header_y, colors::BORDER);
        }

        // ---- File entries ----
        int list_y = header_y + FM_HEADER_H;
        int list_h = c.h - list_y;
        int visible_items = list_h / FM_ITEM_H;
        int content_h = fm->entry_count * FM_ITEM_H;

        // Update scrollbar
        fm->scrollbar.bounds = {c.w - FM_SCROLLBAR_W, list_y, FM_SCROLLBAR_W, list_h};
        fm->scrollbar.content_height = content_h;
        fm->scrollbar.view_height = list_h;

        int scroll_items = fm->scrollbar.scroll_offset / FM_ITEM_H;

        for (int i = scroll_items; i < fm->entry_count && (i - scroll_items) < visible_items + 1; i++) {
            int iy = list_y + (i - scroll_items) * FM_ITEM_H - (fm->scrollbar.scroll_offset % FM_ITEM_H);
            if (iy + FM_ITEM_H <= list_y || iy >= c.h) continue;

            // Highlight selected
            if (i == fm->selected) {
                int sy = gui_max(iy, list_y);
                int sh = gui_min(iy + FM_ITEM_H, c.h) - sy;
                if (sh > 0)
                    c.fill_rect(0, sy, c.w - FM_SCROLLBAR_W, sh, colors::MENU_HOVER);
            }

            // Icon (skip if it would bleed above the list area)
            int ico_x = 8;
            int ico_y = iy + (FM_ITEM_H - 16) / 2;
            // Check for special folder icon
            int sfi_sm = -1;
            if (fm->entry_types[i] == 7) sfi_sm = fm->drive_indices[i];
            else if (ds && fm->entry_types[i] == 1) sfi_sm = special_folder_index(fm->entry_names[i]);

            if (ico_y < list_y) { /* clipped by header repaint */ }
            else if (sfi_sm >= 0 && ds && ds->icon_special_folder[sfi_sm].pixels) {
                c.icon(ico_x, ico_y, ds->icon_special_folder[sfi_sm]);
            } else if (fm->entry_types[i] == 6 && i < fm->app_icon_count && fm->app_icons_sm[i].pixels) {
                c.icon(ico_x, ico_y, fm->app_icons_sm[i]);
            } else if (ds && fm->entry_types[i] == 5 && ds->icon_apps.pixels) {
                c.icon(ico_x, ico_y, ds->icon_apps);
            } else if (ds && fm->entry_types[i] == 4 && ds->icon_home_folder.pixels) {
                c.icon(ico_x, ico_y, ds->icon_home_folder);
            } else if (ds && fm->entry_types[i] == 3 && ds->icon_drive.pixels) {
                c.icon(ico_x, ico_y, ds->icon_drive);
            } else if (ds && fm->entry_types[i] == 1 && ds->icon_folder.pixels) {
                c.icon(ico_x, ico_y, ds->icon_folder);
            } else if (ds && fm->entry_types[i] == 2 && ds->icon_exec.pixels) {
                c.icon(ico_x, ico_y, ds->icon_exec);
            } else if (ds && ds->icon_file.pixels) {
                c.icon(ico_x, ico_y, ds->icon_file);
            } else {
                Color icon_c = fm->is_dir[i]
                    ? Color::from_rgb(0xFF, 0xBD, 0x2E)
                    : Color::from_rgb(0x90, 0x90, 0x90);
                int iy_clip = gui_max(ico_y, list_y);
                int ih_clip = 16 - (iy_clip - ico_y);
                if (ih_clip > 0)
                    c.fill_rect(ico_x, iy_clip, 16, ih_clip, icon_c);
            }

            // Name (or rename textbox)
            int tx = 30;
            int fm_sfh = system_font_height();
            int ty = iy + (FM_ITEM_H - fm_sfh) / 2;

            if (fm->rename_active && fm->rename_idx == i && ty >= list_y && ty + fm_sfh <= c.h) {
                // Rename textbox inline
                int rw = (size_col_x > 100 ? size_col_x - 8 : c.w - FM_SCROLLBAR_W - 8) - tx;
                c.fill_rect(tx - 2, ty - 1, rw, fm_sfh + 2, colors::WHITE);
                c.rect(tx - 2, ty - 1, rw, fm_sfh + 2, colors::ACCENT);
                c.text(tx, ty, fm->rename_buf, colors::TEXT_COLOR);
                // Cursor
                char prefix[64];
                int cpos = fm->rename_cursor;
                if (cpos > 63) cpos = 63;
                for (int k = 0; k < cpos; k++) prefix[k] = fm->rename_buf[k];
                prefix[cpos] = '\0';
                int cx = tx + text_width(prefix);
                c.vline(cx, ty, fm_sfh, colors::ACCENT);
            } else {
                if (ty >= list_y && ty + fm_sfh <= c.h)
                    c.text(tx, ty, fm->entry_names[i], colors::TEXT_COLOR);
            }

            // Size
            if (size_col_x > 100 && !fm->is_dir[i] && ty >= list_y && ty + fm_sfh <= c.h) {
                char size_str[16];
                format_size(size_str, fm->entry_sizes[i]);
                c.text(size_col_x, ty, size_str, dim);
            }

            // Type
            if (type_col_x > 160 && ty >= list_y && ty + fm_sfh <= c.h) {
                const char* type_str = "File";
                if (fm->entry_types[i] == 6) type_str = "App";
                else if (fm->entry_types[i] == 5) type_str = "Apps";
                else if (fm->entry_types[i] == 4) type_str = "Home";
                else if (fm->entry_types[i] == 3) type_str = "Drive";
                else if (fm->entry_types[i] == 1) type_str = "Dir";
                else if (fm->entry_types[i] == 2) type_str = "Exec";
                c.text(type_col_x, ty, type_str, dim);
            }
        }
    }

    // ---- Scrollbar ----
    if (fm->scrollbar.content_height > fm->scrollbar.view_height) {
        Color sb_fg_color = (fm->scrollbar.hovered || fm->scrollbar.dragging)
            ? fm->scrollbar.hover_fg : fm->scrollbar.fg;

        int sbx = fm->scrollbar.bounds.x;
        int sby = fm->scrollbar.bounds.y;
        int sbw = fm->scrollbar.bounds.w;
        int sbh = fm->scrollbar.bounds.h;

        c.fill_rect(sbx, sby, sbw, sbh, colors::SCROLLBAR_BG);

        int th = fm->scrollbar.thumb_height();
        int tty = fm->scrollbar.thumb_y();
        c.fill_rect(sbx + 1, tty, sbw - 2, th, sb_fg_color);
    }

    // Repaint header on top of content to clip scrolled icon overflow
    if (fm->scrollbar.scroll_offset > 0)
        filemanager_draw_header(c, fm, toolbar_color, btn_bg);

    // ---- Context menu overlay ----
    if (fm->ctx_open && fm->ctx_item_count > 0) {
        int cmx = fm->ctx_x;
        int cmy = fm->ctx_y;
        int cmh = fm->ctx_item_count * CTX_ITEM_H + 8;

        // Clamp to window bounds
        if (cmx + CTX_MENU_W > c.w) cmx = c.w - CTX_MENU_W;
        if (cmy + cmh > c.h) cmy = c.h - cmh;
        if (cmx < 0) cmx = 0;
        if (cmy < 0) cmy = 0;

        // Shadow
        c.fill_rect(cmx + 2, cmy + 2, CTX_MENU_W, cmh, Color::from_rgb(0x80, 0x80, 0x80));
        // Background
        c.fill_rounded_rect(cmx, cmy, CTX_MENU_W, cmh, 4, colors::WHITE);
        c.rect(cmx, cmy, CTX_MENU_W, cmh, colors::BORDER);

        for (int i = 0; i < fm->ctx_item_count; i++) {
            int iy = cmy + 4 + i * CTX_ITEM_H;

            // Hover highlight
            if (i == fm->ctx_hover)
                c.fill_rect(cmx + 2, iy, CTX_MENU_W - 4, CTX_ITEM_H, colors::MENU_HOVER);

            int fh = system_font_height();
            int ty = iy + (CTX_ITEM_H - fh) / 2;
            c.text(cmx + 12, ty, ctx_label(fm->ctx_items[i]), colors::TEXT_COLOR);
        }
    }
}

// ============================================================================
// Mouse handling
// ============================================================================

static void filemanager_on_mouse(Window* win, MouseEvent& ev) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm) return;

    Rect cr = win->content_rect();
    int local_x = ev.x - cr.x;
    int local_y = ev.y - cr.y;
    int cw = cr.w;

    // Scrollbar interaction
    MouseEvent local_ev = ev;
    local_ev.x = local_x;
    local_ev.y = local_y;
    fm->scrollbar.handle_mouse(local_ev);

    // ---- Context menu interaction ----
    if (fm->ctx_open) {
        int cmx = fm->ctx_x;
        int cmy = fm->ctx_y;
        int cmh = fm->ctx_item_count * CTX_ITEM_H + 8;
        if (cmx + CTX_MENU_W > cw) cmx = cw - CTX_MENU_W;
        if (cmy + cmh > win->content_h) cmy = win->content_h - cmh;
        if (cmx < 0) cmx = 0;
        if (cmy < 0) cmy = 0;

        // Update hover
        if (local_x >= cmx && local_x < cmx + CTX_MENU_W &&
            local_y >= cmy + 4 && local_y < cmy + 4 + fm->ctx_item_count * CTX_ITEM_H) {
            fm->ctx_hover = (local_y - cmy - 4) / CTX_ITEM_H;
        } else {
            fm->ctx_hover = -1;
        }

        if (ev.left_pressed()) {
            if (fm->ctx_hover >= 0 && fm->ctx_hover < fm->ctx_item_count) {
                int action = fm->ctx_items[fm->ctx_hover];
                int target = fm->ctx_target_idx;
                filemanager_close_ctx_menu(fm);

                // Select target if needed
                if (target >= 0 && target < fm->entry_count)
                    fm->selected = target;

                switch (action) {
                case CTX_OPEN:       filemanager_open_entry(fm, target); break;
                case CTX_COPY:       filemanager_do_copy(fm); break;
                case CTX_CUT:        filemanager_do_cut(fm); break;
                case CTX_PASTE:      filemanager_do_paste(fm); break;
                case CTX_RENAME:     filemanager_start_rename(fm); break;
                case CTX_DELETE:     filemanager_delete_selected(fm); break;
                case CTX_NEW_FOLDER: filemanager_new_folder(fm); break;
                }
            } else {
                filemanager_close_ctx_menu(fm);
            }
            return;
        }
        if (ev.right_pressed()) {
            filemanager_close_ctx_menu(fm);
            return;
        }
        return;
    }

    // ---- Cancel rename on click outside rename area ----
    if (fm->rename_active && ev.left_pressed()) {
        filemanager_finish_rename(fm);
    }

    if (ev.left_pressed()) {
        // Click on path bar area
        if (local_y >= FM_TOOLBAR_H && local_y < FM_TOOLBAR_H + FM_PATHBAR_H) {
            if (!fm->pathbar_editing)
                filemanager_start_pathbar(fm);
            return;
        }

        // Clicking anywhere else while pathbar is editing commits the path
        if (fm->pathbar_editing) {
            filemanager_commit_pathbar(fm);
        }

        // Toolbar button clicks
        if (local_y < FM_TOOLBAR_H) {
            // Navigation buttons
            if (local_x >= 4 && local_x < 28)       filemanager_go_back(fm);
            else if (local_x >= 32 && local_x < 56)  filemanager_go_forward(fm);
            else if (local_x >= 60 && local_x < 84)  filemanager_go_up(fm);
            else if (local_x >= 88 && local_x < 112)  filemanager_go_home(fm);
            else if (local_x >= 120 && local_x < 144) {
                fm->grid_view = !fm->grid_view;
                fm->scrollbar.scroll_offset = 0;
            }
            // Action buttons
            else if (local_x >= 160 && local_x < 184) filemanager_do_copy(fm);
            else if (local_x >= 188 && local_x < 212) filemanager_do_cut(fm);
            else if (local_x >= 216 && local_x < 240) filemanager_do_paste(fm);
            else if (local_x >= 244 && local_x < 268) filemanager_start_rename(fm);
            else if (local_x >= 272 && local_x < 296) filemanager_new_folder(fm);
            else if (local_x >= 300 && local_x < 324) filemanager_delete_selected(fm);
            return;
        }

        // File clicks (grid vs list)
        if (fm->grid_view) {
            int list_y = FM_TOOLBAR_H + FM_PATHBAR_H;
            if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
                int cols = (cw - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
                if (cols < 1) cols = 1;
                int col = local_x / FM_GRID_CELL_W;
                int row = (local_y - list_y + fm->scrollbar.scroll_offset) / FM_GRID_CELL_H;
                int clicked_idx = row * cols + col;

                if (clicked_idx >= 0 && clicked_idx < fm->entry_count && col < cols) {
                    uint64_t now = montauk::get_milliseconds();

                    if (fm->last_click_item == clicked_idx &&
                        (now - fm->last_click_time) < 400) {
                        filemanager_open_entry(fm, clicked_idx);
                        fm->last_click_item = -1;
                        fm->last_click_time = 0;
                    } else {
                        fm->selected = clicked_idx;
                        fm->last_click_item = clicked_idx;
                        fm->last_click_time = now;
                    }
                } else {
                    fm->selected = -1;
                }
            }
        } else {
            // List view clicks
            int list_y = FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
            if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
                int rel_y = local_y - list_y + fm->scrollbar.scroll_offset;
                int clicked_idx = rel_y / FM_ITEM_H;

                if (clicked_idx >= 0 && clicked_idx < fm->entry_count) {
                    uint64_t now = montauk::get_milliseconds();

                    // Double-click detection
                    if (fm->last_click_item == clicked_idx &&
                        (now - fm->last_click_time) < 400) {
                        filemanager_open_entry(fm, clicked_idx);
                        fm->last_click_item = -1;
                        fm->last_click_time = 0;
                    } else {
                        fm->selected = clicked_idx;
                        fm->last_click_item = clicked_idx;
                        fm->last_click_time = now;
                    }
                } else {
                    fm->selected = -1;
                }
            }
        }
    }

    // ---- Right-click: open context menu ----
    if (ev.right_pressed()) {
        filemanager_cancel_rename(fm);

        // Determine what was right-clicked
        int target_idx = -1;

        if (local_y >= FM_TOOLBAR_H + FM_PATHBAR_H) {
            if (fm->grid_view) {
                int list_y = FM_TOOLBAR_H + FM_PATHBAR_H;
                if (local_x < cw - FM_SCROLLBAR_W) {
                    int cols = (cw - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
                    if (cols < 1) cols = 1;
                    int col = local_x / FM_GRID_CELL_W;
                    int row = (local_y - list_y + fm->scrollbar.scroll_offset) / FM_GRID_CELL_H;
                    int idx = row * cols + col;
                    if (idx >= 0 && idx < fm->entry_count && col < cols) {
                        target_idx = idx;
                        fm->selected = idx;
                    }
                }
            } else {
                int list_y = FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
                if (local_y >= list_y && local_x < cw - FM_SCROLLBAR_W) {
                    int rel_y = local_y - list_y + fm->scrollbar.scroll_offset;
                    int idx = rel_y / FM_ITEM_H;
                    if (idx >= 0 && idx < fm->entry_count) {
                        target_idx = idx;
                        fm->selected = idx;
                    }
                }
            }
        }

        filemanager_open_ctx_menu(fm, local_x, local_y, target_idx);
    }

    // Scroll handling
    if (ev.scroll != 0) {
        int list_y_start = fm->grid_view
            ? FM_TOOLBAR_H + FM_PATHBAR_H
            : FM_TOOLBAR_H + FM_PATHBAR_H + FM_HEADER_H;
        int scroll_step = fm->grid_view ? FM_GRID_CELL_H : FM_ITEM_H;
        if (local_y >= list_y_start) {
            fm->scrollbar.scroll_offset -= ev.scroll * scroll_step;
            int ms = fm->scrollbar.max_scroll();
            if (fm->scrollbar.scroll_offset < 0) fm->scrollbar.scroll_offset = 0;
            if (fm->scrollbar.scroll_offset > ms) fm->scrollbar.scroll_offset = ms;
        }
    }
}

// ============================================================================
// Keyboard handling
// ============================================================================

static void filemanager_on_key(Window* win, const Montauk::KeyEvent& key) {
    FileManagerState* fm = (FileManagerState*)win->app_data;
    if (!fm || !key.pressed) return;

    // ---- Path bar editing key handling ----
    if (fm->pathbar_editing) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            filemanager_commit_pathbar(fm);
        } else if (key.scancode == 0x01) {
            filemanager_cancel_pathbar(fm);
        } else if (key.ascii == '\b' || key.scancode == 0x0E) {
            if (fm->pathbar_cursor > 0) {
                for (int i = fm->pathbar_cursor - 1; i < fm->pathbar_len - 1; i++)
                    fm->pathbar_buf[i] = fm->pathbar_buf[i + 1];
                fm->pathbar_len--;
                fm->pathbar_cursor--;
                fm->pathbar_buf[fm->pathbar_len] = '\0';
            }
        } else if (key.scancode == 0x53) {
            if (fm->pathbar_cursor < fm->pathbar_len) {
                for (int i = fm->pathbar_cursor; i < fm->pathbar_len - 1; i++)
                    fm->pathbar_buf[i] = fm->pathbar_buf[i + 1];
                fm->pathbar_len--;
                fm->pathbar_buf[fm->pathbar_len] = '\0';
            }
        } else if (key.scancode == 0x4B) {
            if (fm->pathbar_cursor > 0) fm->pathbar_cursor--;
        } else if (key.scancode == 0x4D) {
            if (fm->pathbar_cursor < fm->pathbar_len) fm->pathbar_cursor++;
        } else if (key.scancode == 0x47) {
            fm->pathbar_cursor = 0;
        } else if (key.scancode == 0x4F) {
            fm->pathbar_cursor = fm->pathbar_len;
        } else if (key.ascii >= 32 && key.ascii < 127 && fm->pathbar_len < 254) {
            for (int i = fm->pathbar_len; i > fm->pathbar_cursor; i--)
                fm->pathbar_buf[i] = fm->pathbar_buf[i - 1];
            fm->pathbar_buf[fm->pathbar_cursor] = key.ascii;
            fm->pathbar_cursor++;
            fm->pathbar_len++;
            fm->pathbar_buf[fm->pathbar_len] = '\0';
        }
        return;
    }

    // ---- Rename mode key handling ----
    if (fm->rename_active) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            filemanager_finish_rename(fm);
        } else if (key.scancode == 0x01) {
            // Escape
            filemanager_cancel_rename(fm);
        } else if (key.ascii == '\b' || key.scancode == 0x0E) {
            // Backspace
            if (fm->rename_cursor > 0) {
                for (int i = fm->rename_cursor - 1; i < fm->rename_len - 1; i++)
                    fm->rename_buf[i] = fm->rename_buf[i + 1];
                fm->rename_len--;
                fm->rename_cursor--;
                fm->rename_buf[fm->rename_len] = '\0';
            }
        } else if (key.scancode == 0x53) {
            // Delete key
            if (fm->rename_cursor < fm->rename_len) {
                for (int i = fm->rename_cursor; i < fm->rename_len - 1; i++)
                    fm->rename_buf[i] = fm->rename_buf[i + 1];
                fm->rename_len--;
                fm->rename_buf[fm->rename_len] = '\0';
            }
        } else if (key.scancode == 0x4B) {
            // Left arrow
            if (fm->rename_cursor > 0) fm->rename_cursor--;
        } else if (key.scancode == 0x4D) {
            // Right arrow
            if (fm->rename_cursor < fm->rename_len) fm->rename_cursor++;
        } else if (key.scancode == 0x47) {
            // Home
            fm->rename_cursor = 0;
        } else if (key.scancode == 0x4F) {
            // End
            fm->rename_cursor = fm->rename_len;
        } else if (key.ascii >= 32 && key.ascii < 127) {
            // Printable character (reject / and other FS-unsafe chars)
            if (key.ascii != '/' && key.ascii != '\\' && fm->rename_len < 62) {
                for (int i = fm->rename_len; i > fm->rename_cursor; i--)
                    fm->rename_buf[i] = fm->rename_buf[i - 1];
                fm->rename_buf[fm->rename_cursor] = key.ascii;
                fm->rename_cursor++;
                fm->rename_len++;
                fm->rename_buf[fm->rename_len] = '\0';
            }
        }
        return;
    }

    // ---- Context menu: Escape to close ----
    if (fm->ctx_open && key.scancode == 0x01) {
        filemanager_close_ctx_menu(fm);
        return;
    }

    // ---- Keyboard shortcuts ----
    if (key.ctrl) {
        if (key.ascii == 'c' || key.ascii == 'C') {
            filemanager_do_copy(fm);
            return;
        }
        if (key.ascii == 'x' || key.ascii == 'X') {
            filemanager_do_cut(fm);
            return;
        }
        if (key.ascii == 'v' || key.ascii == 'V') {
            filemanager_do_paste(fm);
            return;
        }
    }

    // F2 = rename
    if (key.scancode == 0x3C) {
        filemanager_start_rename(fm);
        return;
    }

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        filemanager_go_up(fm);
    } else if (key.scancode == 0x48) {
        // Up arrow
        if (fm->grid_view) {
            Rect cr_r = win->content_rect();
            int cols = (cr_r.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
            if (cols < 1) cols = 1;
            if (fm->selected >= cols) fm->selected -= cols;
        } else {
            if (fm->selected > 0) fm->selected--;
        }
    } else if (key.scancode == 0x50) {
        // Down arrow
        if (fm->grid_view) {
            Rect cr_r = win->content_rect();
            int cols = (cr_r.w - FM_SCROLLBAR_W) / FM_GRID_CELL_W;
            if (cols < 1) cols = 1;
            if (fm->selected + cols < fm->entry_count) fm->selected += cols;
        } else {
            if (fm->selected < fm->entry_count - 1) fm->selected++;
        }
    } else if (key.scancode == 0x4B && !key.alt && fm->grid_view) {
        // Left arrow (grid view only)
        if (fm->selected > 0) fm->selected--;
    } else if (key.scancode == 0x4D && !key.alt && fm->grid_view) {
        // Right arrow (grid view only)
        if (fm->selected < fm->entry_count - 1) fm->selected++;
    } else if (key.ascii == '\n' || key.ascii == '\r') {
        if (fm->selected >= 0 && fm->selected < fm->entry_count)
            filemanager_open_entry(fm, fm->selected);
    } else if (key.scancode == 0x53) {
        // Delete key
        filemanager_delete_selected(fm);
    } else if (key.alt && key.scancode == 0x4B) {
        // Alt+Left: go back
        filemanager_go_back(fm);
    } else if (key.alt && key.scancode == 0x4D) {
        // Alt+Right: go forward
        filemanager_go_forward(fm);
    }
}

static void filemanager_on_close(Window* win) {
    if (win->app_data) {
        FileManagerState* fm = (FileManagerState*)win->app_data;
        filemanager_free_app_icons(fm);
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// File Manager launcher
// ============================================================================

void ensure_filemanager_icons_loaded(DesktopState* ds) {
    if (!ds || ds->icon_drive.pixels) return;

    Color defColor = colors::ICON_COLOR;
    ds->icon_drive          = svg_load("0:/icons/drive-harddisk.svg", 16, 16, defColor);
    ds->icon_drive_lg       = svg_load("0:/icons/drive-harddisk.svg", 48, 48, defColor);
    ds->icon_home_folder    = svg_load("0:/icons/folder-blue-home.svg", 16, 16, defColor);
    ds->icon_home_folder_lg = svg_load("0:/icons/folder-blue-home.svg", 48, 48, defColor);
    ds->icon_apps           = svg_load("0:/icons/folder-blue-development.svg", 16, 16, defColor);
    ds->icon_apps_lg        = svg_load("0:/icons/folder-blue-development.svg", 48, 48, defColor);

    for (int sf = 0; sf < SF_COUNT; sf++) {
        char icon_path[128];
        snprintf(icon_path, 128, "0:/icons/%s", sf_icons[sf]);
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

void open_filemanager(DesktopState* ds) {
    open_filemanager_internal(ds, nullptr);
}

void open_filemanager_path(DesktopState* ds, const char* path) {
    open_filemanager_internal(ds, path);
}
