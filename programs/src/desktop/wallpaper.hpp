/*
    * wallpaper.hpp
    * MontaukOS Desktop - JPEG wallpaper loading, scaling, and directory scanning
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/desktop.hpp>

// Forward-declare stb_image functions (implementation in libjpeg.a).
// We avoid including stb_image.h directly because its declaration section
// pulls in <stdlib.h> which is unavailable in the desktop's freestanding build.
extern "C" {
    unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                         int* x, int* y, int* channels_in_file,
                                         int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
    const char* stbi_failure_reason(void);
}

namespace gui {

// ============================================================================
// Wallpaper loading
// ============================================================================

// Load a JPEG file and scale it to cover the given screen dimensions.
// Stores the scaled ARGB pixel buffer in settings.  Returns true on success.
inline bool wallpaper_load(DesktopSettings* s, const char* path,
                           int screen_w, int screen_h) {
    // Free existing wallpaper
    if (s->bg_wallpaper) {
        montauk::mfree(s->bg_wallpaper);
        s->bg_wallpaper = nullptr;
        s->bg_wallpaper_w = 0;
        s->bg_wallpaper_h = 0;
    }

    // Read file
    int fd = montauk::open(path);
    if (fd < 0) return false;

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > 16 * 1024 * 1024) {
        montauk::close(fd);
        return false;
    }

    uint8_t* filedata = (uint8_t*)montauk::malloc(size);
    if (!filedata) { montauk::close(fd); return false; }

    int bytes_read = montauk::read(fd, filedata, 0, size);
    montauk::close(fd);
    if (bytes_read <= 0) { montauk::mfree(filedata); return false; }

    // Decode JPEG
    int img_w, img_h, channels;
    unsigned char* rgb = stbi_load_from_memory(filedata, bytes_read,
                                               &img_w, &img_h, &channels, 3);
    montauk::mfree(filedata);
    if (!rgb) return false;

    // Scale to cover screen (crop to fill, maintain aspect ratio)
    int dst_w = screen_w;
    int dst_h = screen_h;

    uint32_t* scaled = (uint32_t*)montauk::malloc((uint64_t)dst_w * dst_h * 4);
    if (!scaled) { stbi_image_free(rgb); return false; }

    // Compute source crop region for "cover" scaling
    int src_crop_w, src_crop_h, src_x0, src_y0;
    if ((int64_t)img_w * dst_h > (int64_t)img_h * dst_w) {
        // Image is wider — crop sides
        src_crop_h = img_h;
        src_crop_w = (int)((int64_t)img_h * dst_w / dst_h);
        src_x0 = (img_w - src_crop_w) / 2;
        src_y0 = 0;
    } else {
        // Image is taller — crop top/bottom
        src_crop_w = img_w;
        src_crop_h = (int)((int64_t)img_w * dst_h / dst_w);
        src_x0 = 0;
        src_y0 = (img_h - src_crop_h) / 2;
    }

    // Nearest-neighbor scale from cropped region to destination
    for (int y = 0; y < dst_h; y++) {
        int sy = src_y0 + (int)((int64_t)y * src_crop_h / dst_h);
        if (sy < 0) sy = 0;
        if (sy >= img_h) sy = img_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = src_x0 + (int)((int64_t)x * src_crop_w / dst_w);
            if (sx < 0) sx = 0;
            if (sx >= img_w) sx = img_w - 1;
            int si = (sy * img_w + sx) * 3;
            scaled[y * dst_w + x] = 0xFF000000u
                | ((uint32_t)rgb[si] << 16)
                | ((uint32_t)rgb[si + 1] << 8)
                | (uint32_t)rgb[si + 2];
        }
    }

    stbi_image_free(rgb);

    s->bg_wallpaper   = scaled;
    s->bg_wallpaper_w = dst_w;
    s->bg_wallpaper_h = dst_h;
    montauk::strncpy(s->bg_image_path, path, 127);
    s->bg_image = true;
    s->bg_gradient = false;

    return true;
}

inline void wallpaper_free(DesktopSettings* s) {
    if (s->bg_wallpaper) {
        montauk::mfree(s->bg_wallpaper);
        s->bg_wallpaper = nullptr;
        s->bg_wallpaper_w = 0;
        s->bg_wallpaper_h = 0;
    }
    s->bg_image_path[0] = '\0';
    s->bg_image = false;
}

// ============================================================================
// Directory scanning for JPEG files
// ============================================================================

static constexpr int WALLPAPER_MAX_FILES = 16;

struct WallpaperFileList {
    char names[WALLPAPER_MAX_FILES][64];
    int count;
};

inline void wallpaper_scan_dir(const char* dir_path, WallpaperFileList* list) {
    list->count = 0;

    const char* raw_names[64];
    int total = montauk::readdir(dir_path, raw_names, 64);
    if (total <= 0) return;

    // Compute prefix to strip (readdir returns full paths from VFS root)
    const char* after_drive = dir_path;
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

    auto to_lower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    };

    for (int i = 0; i < total && list->count < WALLPAPER_MAX_FILES; i++) {
        const char* name = raw_names[i];

        // Strip prefix
        if (prefix_len > 0) {
            bool match = true;
            for (int k = 0; k < prefix_len; k++) {
                if (name[k] != prefix[k]) { match = false; break; }
            }
            if (match) name += prefix_len;
        }

        int nlen = montauk::slen(name);

        // Skip directories
        if (nlen > 0 && name[nlen - 1] == '/') continue;

        // Check for .jpg
        bool is_jpeg = false;
        if (nlen >= 4 &&
            to_lower(name[nlen - 4]) == '.' &&
            to_lower(name[nlen - 3]) == 'j' &&
            to_lower(name[nlen - 2]) == 'p' &&
            to_lower(name[nlen - 1]) == 'g') {
            is_jpeg = true;
        }
        // Check for .jpeg
        if (!is_jpeg && nlen >= 5 &&
            to_lower(name[nlen - 5]) == '.' &&
            to_lower(name[nlen - 4]) == 'j' &&
            to_lower(name[nlen - 3]) == 'p' &&
            to_lower(name[nlen - 2]) == 'e' &&
            to_lower(name[nlen - 1]) == 'g') {
            is_jpeg = true;
        }

        if (is_jpeg) {
            montauk::strncpy(list->names[list->count], name, 63);
            list->count++;
        }
    }
}

} // namespace gui
