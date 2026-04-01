/*
 * main.cpp
 * MontaukOS Screenshot Tool
 * Captures the screen framebuffer and saves as JPEG to the user's Pictures directory.
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>

extern "C" {
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define STBI_WRITE_NO_STDIO
#include <gui/stb_image_write.h>
}

// ============================================================================
// JPEG write callback - accumulates encoded bytes into a resizable buffer
// ============================================================================

struct JpegBuffer {
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
};

static void jpeg_write_callback(void* context, void* data, int size) {
    JpegBuffer* buf = (JpegBuffer*)context;
    if (size <= 0) return;

    uint64_t needed = buf->size + (uint64_t)size;
    if (needed > buf->capacity) {
        uint64_t new_cap = buf->capacity * 2;
        if (new_cap < needed) new_cap = needed;
        uint8_t* new_data = (uint8_t*)montauk::realloc(buf->data, new_cap);
        if (!new_data) return;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    montauk::memcpy(buf->data + buf->size, data, size);
    buf->size += (uint64_t)size;
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Get framebuffer info before doing anything else
    Montauk::FbInfo fb;
    montauk::fb_info(&fb);

    int width  = (int)fb.width;
    int height = (int)fb.height;
    int pitch  = (int)fb.pitch;

    if (width <= 0 || height <= 0) {
        montauk::print("screenshot: no framebuffer available\n");
        montauk::exit(1);
    }

    // Map the hardware framebuffer (read-only access to screen pixels)
    uint32_t* hw_fb = (uint32_t*)montauk::fb_map();
    if (!hw_fb) {
        montauk::print("screenshot: failed to map framebuffer\n");
        montauk::exit(1);
    }

    // Copy framebuffer and convert from BGRA (0xAARRGGBB) to packed RGB
    uint64_t rgb_size = (uint64_t)width * height * 3;
    uint8_t* rgb = (uint8_t*)montauk::malloc(rgb_size);
    if (!rgb) {
        montauk::print("screenshot: out of memory\n");
        montauk::exit(1);
    }

    for (int y = 0; y < height; y++) {
        uint32_t* src_row = (uint32_t*)((uint8_t*)hw_fb + y * pitch);
        uint8_t* dst_row = rgb + y * width * 3;
        for (int x = 0; x < width; x++) {
            uint32_t px = src_row[x];
            dst_row[x * 3 + 0] = (px >> 16) & 0xFF; // R
            dst_row[x * 3 + 1] = (px >> 8)  & 0xFF; // G
            dst_row[x * 3 + 2] = px & 0xFF;          // B
        }
    }

    // Encode as JPEG (quality 100 - maximum quality)
    JpegBuffer jpeg;
    jpeg.data = (uint8_t*)montauk::malloc(256 * 1024);
    jpeg.size = 0;
    jpeg.capacity = 256 * 1024;

    if (!jpeg.data) {
        montauk::mfree(rgb);
        montauk::print("screenshot: out of memory for JPEG buffer\n");
        montauk::exit(1);
    }

    int ok = stbi_write_jpg_to_func(jpeg_write_callback, &jpeg,
                                     width, height, 3, rgb, 100);
    montauk::mfree(rgb);

    if (!ok || jpeg.size == 0) {
        montauk::mfree(jpeg.data);
        montauk::print("screenshot: JPEG encoding failed\n");
        montauk::exit(1);
    }

    // Build output path: 0:/users/<username>/Pictures/screenshot_YYYYMMDD_HHMMSS.jpg
    char username[32] = {};
    if (montauk::getuser(username, sizeof(username)) <= 0 || username[0] == '\0') {
        montauk::mfree(jpeg.data);
        montauk::print("screenshot: no user session\n");
        montauk::exit(1);
    }

    char home[128];
    snprintf(home, sizeof(home), "0:/users/%s", username);

    // Ensure Pictures directory exists
    char pictures_dir[192];
    snprintf(pictures_dir, sizeof(pictures_dir), "%s/Pictures", home);
    montauk::fmkdir(pictures_dir);

    // Build filename with timestamp
    Montauk::DateTime dt;
    montauk::gettime(&dt);

    char filepath[256];
    snprintf(filepath, sizeof(filepath),
             "%s/screenshot_%04d%02d%02d_%02d%02d%02d.jpg",
             pictures_dir,
             (int)dt.Year, (int)dt.Month, (int)dt.Day,
             (int)dt.Hour, (int)dt.Minute, (int)dt.Second);

    // Write JPEG file
    int fd = montauk::fcreate(filepath);
    if (fd < 0) {
        montauk::mfree(jpeg.data);
        montauk::print("screenshot: failed to create file\n");
        montauk::exit(1);
    }

    montauk::fwrite(fd, jpeg.data, 0, jpeg.size);
    montauk::close(fd);
    montauk::mfree(jpeg.data);

    // Print confirmation
    char msg[320];
    snprintf(msg, sizeof(msg), "Screenshot saved: %s (%dx%d, %llu bytes)\n",
             filepath, width, height, (unsigned long long)jpeg.size);
    montauk::print(msg);

    montauk::exit(0);
}
