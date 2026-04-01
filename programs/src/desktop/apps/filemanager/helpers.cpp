/*
    * helpers.cpp
    * Shared helpers for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#include "filemanager_internal.hpp"

namespace filemanager {

int special_folder_index(const char* name) {
    for (int i = 0; i < SF_COUNT; i++) {
        if (montauk::streq(name, sf_names[i])) return i;
    }
    return -1;
}

const char* ctx_label(int action) {
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

bool str_ends_with(const char* s, const char* suffix) {
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

bool is_image_file(const char* name) {
    return str_ends_with(name, ".jpg") || str_ends_with(name, ".jpeg");
}

bool is_font_file(const char* name) {
    return str_ends_with(name, ".ttf");
}

bool is_pdf_file(const char* name) {
    return str_ends_with(name, ".pdf");
}

bool is_spreadsheet_file(const char* name) {
    return str_ends_with(name, ".mss");
}

bool is_wordprocessor_file(const char* name) {
    return str_ends_with(name, ".mwp");
}

bool is_audio_file(const char* name) {
    return str_ends_with(name, ".mp3") || str_ends_with(name, ".wav");
}

bool is_video_file(const char* name) {
    return str_ends_with(name, ".avi");
}

int detect_file_type(const char* name, bool is_dir) {
    if (is_dir) return 1;
    if (str_ends_with(name, ".elf")) return 2;
    return 0;
}

void filemanager_build_fullpath(char* out, int out_max, const char* dir, const char* name) {
    montauk::strcpy(out, dir);
    int plen = montauk::slen(out);
    if (plen > 0 && out[plen - 1] != '/') str_append(out, "/", out_max);
    str_append(out, name, out_max);
}

// Extract basename from a path (pointer into the path string)
const char* path_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' && *(p + 1)) last = p + 1;
    }
    return last;
}

} // namespace filemanager
