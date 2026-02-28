/*
 * main.cpp
 * MontaukOS Image Viewer - standalone Window Server process
 * Displays JPEG images with pan via mouse drag, scroll wheel, and arrow keys
 * Copyright (c) 2026 Daniel Hammer
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>

extern "C" {
#include <string.h>
#include <stdlib.h>
#include <gui/stb_image.h>
}

using namespace gui;

// ============================================================================
// Constants
// ============================================================================

static constexpr int INIT_W      = 800;
static constexpr int INIT_H      = 600;
static constexpr int STATUS_H    = 24;
static constexpr int PAN_STEP    = 40;
static constexpr int FONT_SIZE   = 13;

static constexpr Color BG_COLOR     = Color::from_rgb(0x30, 0x30, 0x30);
static constexpr Color STATUS_BG    = Color::from_rgb(0x24, 0x24, 0x24);
static constexpr Color STATUS_TEXT  = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color ERR_COLOR    = Color::from_rgb(0xCC, 0x33, 0x33);

// ============================================================================
// App state
// ============================================================================

static int       g_win_w   = INIT_W;
static int       g_win_h   = INIT_H;

// Image data
static uint32_t* g_image   = nullptr;   // ARGB pixel buffer
static int       g_img_w   = 0;
static int       g_img_h   = 0;

// Pan offset (image position relative to viewport)
static int       g_pan_x   = 0;
static int       g_pan_y   = 0;

// Mouse drag state
static bool      g_dragging   = false;
static int       g_drag_start_x = 0;
static int       g_drag_start_y = 0;
static int       g_drag_pan_x   = 0;
static int       g_drag_pan_y   = 0;

// File info
static char      g_filename[128] = {};
static char      g_status[256]   = {};
static bool      g_load_ok       = false;

// Font
static TrueTypeFont* g_font = nullptr;

// ============================================================================
// Helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x,   y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

// Extract basename from a VFS path (e.g. "0:/photos/cat.jpg" -> "cat.jpg")
static const char* basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

// Format an integer into a decimal string
static void int_to_str(char* buf, int val) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    bool neg = val < 0;
    if (neg) val = -val;
    char tmp[16];
    int i = 0;
    while (val > 0 && i < 15) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void str_append(char* dst, const char* src, int maxlen) {
    int len = montauk::slen(dst);
    int i = 0;
    while (src[i] && len + i < maxlen - 1) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = '\0';
}

// ============================================================================
// Image loading
// ============================================================================

static bool load_image(const char* path) {
    int fd = montauk::open(path);
    if (fd < 0) {
        montauk::strcpy(g_status, "Error: could not open file");
        return false;
    }

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > 16 * 1024 * 1024) {
        montauk::close(fd);
        montauk::strcpy(g_status, "Error: file too large or empty");
        return false;
    }

    uint8_t* filedata = (uint8_t*)montauk::malloc(size);
    if (!filedata) {
        montauk::close(fd);
        montauk::strcpy(g_status, "Error: out of memory");
        return false;
    }

    int bytes_read = montauk::read(fd, filedata, 0, size);
    montauk::close(fd);

    if (bytes_read <= 0) {
        montauk::mfree(filedata);
        montauk::strcpy(g_status, "Error: could not read file");
        return false;
    }

    int w, h, channels;
    unsigned char* rgb = stbi_load_from_memory(filedata, bytes_read, &w, &h, &channels, 3);
    montauk::mfree(filedata);

    if (!rgb) {
        montauk::strcpy(g_status, "Error: ");
        const char* reason = stbi_failure_reason();
        if (reason) str_append(g_status, reason, 256);
        else str_append(g_status, "unknown decode error", 256);
        return false;
    }

    // Convert RGB to ARGB
    g_image = (uint32_t*)montauk::malloc((uint64_t)w * h * 4);
    if (!g_image) {
        stbi_image_free(rgb);
        montauk::strcpy(g_status, "Error: out of memory for image");
        return false;
    }

    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        g_image[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    stbi_image_free(rgb);

    g_img_w = w;
    g_img_h = h;

    // Build status string: "filename.jpg  (1920 x 1080)"
    const char* name = basename(path);
    montauk::strncpy(g_filename, name, 127);

    montauk::strcpy(g_status, g_filename);
    str_append(g_status, "  (", 256);
    char num[16];
    int_to_str(num, w);
    str_append(g_status, num, 256);
    str_append(g_status, " x ", 256);
    int_to_str(num, h);
    str_append(g_status, num, 256);
    str_append(g_status, ")", 256);

    return true;
}

// ============================================================================
// Pan clamping
// ============================================================================

static void clamp_pan() {
    int view_h = g_win_h - STATUS_H;

    // If image fits in viewport, center it
    if (g_img_w <= g_win_w) {
        g_pan_x = (g_win_w - g_img_w) / 2;
    } else {
        // Clamp so image edges don't leave the viewport
        if (g_pan_x > 0) g_pan_x = 0;
        if (g_pan_x < g_win_w - g_img_w) g_pan_x = g_win_w - g_img_w;
    }

    if (g_img_h <= view_h) {
        g_pan_y = (view_h - g_img_h) / 2;
    } else {
        if (g_pan_y > 0) g_pan_y = 0;
        if (g_pan_y < view_h - g_img_h) g_pan_y = view_h - g_img_h;
    }
}

static void center_image() {
    int view_h = g_win_h - STATUS_H;
    g_pan_x = (g_win_w - g_img_w) / 2;
    g_pan_y = (view_h - g_img_h) / 2;
    clamp_pan();
}

// ============================================================================
// Rendering
// ============================================================================

static void render(uint32_t* pixels) {
    int view_h = g_win_h - STATUS_H;

    // Fill background
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, view_h, BG_COLOR);

    // Draw image
    if (g_image && g_load_ok) {
        for (int row = 0; row < g_img_h; row++) {
            int dy = g_pan_y + row;
            if (dy < 0 || dy >= view_h) continue;
            for (int col = 0; col < g_img_w; col++) {
                int dx = g_pan_x + col;
                if (dx < 0 || dx >= g_win_w) continue;
                pixels[dy * g_win_w + dx] = g_image[row * g_img_w + col];
            }
        }
    } else if (!g_load_ok && g_font) {
        // Show error message centered
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            20, view_h / 2 - 8, g_status, ERR_COLOR, 15);
    }

    // Status bar
    px_fill(pixels, g_win_w, g_win_h, 0, view_h, g_win_w, STATUS_H, STATUS_BG);
    if (g_font) {
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
            8, view_h + (STATUS_H - FONT_SIZE) / 2, g_status, STATUS_TEXT, FONT_SIZE);
    }
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Get file path from arguments
    char filepath[512] = {};
    int arglen = montauk::getargs(filepath, sizeof(filepath));
    if (arglen <= 0) {
        montauk::strcpy(filepath, "");
    }

    // Load font
    auto load_font = [](const char* path) -> TrueTypeFont* {
        TrueTypeFont* f = (TrueTypeFont*)montauk::malloc(sizeof(TrueTypeFont));
        if (!f) return nullptr;
        montauk::memset(f, 0, sizeof(TrueTypeFont));
        if (!f->init(path)) { montauk::mfree(f); return nullptr; }
        return f;
    };
    g_font = load_font("0:/fonts/Roboto-Medium.ttf");

    // Determine window title from filename
    char title[64] = "Image Viewer";
    if (filepath[0]) {
        const char* name = basename(filepath);
        if (name[0]) montauk::strncpy(title, name, 63);
    }

    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create(title, INIT_W, INIT_H, &wres) < 0 || wres.id < 0)
        montauk::exit(1);

    int       win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    // Load image
    if (filepath[0]) {
        g_load_ok = load_image(filepath);
    } else {
        montauk::strcpy(g_status, "No file specified");
        g_load_ok = false;
    }

    // Center image initially
    if (g_load_ok) center_image();

    // Initial render
    render(pixels);
    montauk::win_present(win_id);

    // Event loop
    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;

        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        // Close event
        if (ev.type == 3) break;

        // Resize event
        if (ev.type == 2) {
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            if (g_load_ok) clamp_pan();
            render(pixels);
            montauk::win_present(win_id);
            continue;
        }

        // Key event
        if (ev.type == 0 && ev.key.pressed) {
            bool redraw = false;

            // Q or Escape to close
            if (ev.key.ascii == 'q' || ev.key.ascii == 'Q' || ev.key.scancode == 0x01) {
                break;
            }

            // Arrow keys to pan
            if (ev.key.scancode == 0x48) { g_pan_y += PAN_STEP; redraw = true; }  // Up
            if (ev.key.scancode == 0x50) { g_pan_y -= PAN_STEP; redraw = true; }  // Down
            if (ev.key.scancode == 0x4B) { g_pan_x += PAN_STEP; redraw = true; }  // Left
            if (ev.key.scancode == 0x4D) { g_pan_x -= PAN_STEP; redraw = true; }  // Right

            // Home key to re-center
            if (ev.key.scancode == 0x47) { center_image(); redraw = true; }

            if (redraw && g_load_ok) {
                clamp_pan();
                render(pixels);
                montauk::win_present(win_id);
            }
            continue;
        }

        // Mouse event
        if (ev.type == 1) {
            bool redraw = false;
            bool left_now  = ev.mouse.buttons & 1;
            bool left_prev = ev.mouse.prev_buttons & 1;

            // Start drag
            if (left_now && !left_prev) {
                g_dragging = true;
                g_drag_start_x = ev.mouse.x;
                g_drag_start_y = ev.mouse.y;
                g_drag_pan_x = g_pan_x;
                g_drag_pan_y = g_pan_y;
            }

            // Continue drag
            if (left_now && g_dragging) {
                g_pan_x = g_drag_pan_x + (ev.mouse.x - g_drag_start_x);
                g_pan_y = g_drag_pan_y + (ev.mouse.y - g_drag_start_y);
                redraw = true;
            }

            // End drag
            if (!left_now && g_dragging) {
                g_dragging = false;
            }

            // Scroll wheel for vertical pan
            if (ev.mouse.scroll != 0) {
                g_pan_y += ev.mouse.scroll * PAN_STEP;
                redraw = true;
            }

            if (redraw && g_load_ok) {
                clamp_pan();
                render(pixels);
                montauk::win_present(win_id);
            }
            continue;
        }
    }

    // Cleanup
    if (g_image) montauk::mfree(g_image);
    montauk::win_destroy(win_id);
    montauk::exit(0);
}
