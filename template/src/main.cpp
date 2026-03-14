/*
 * main.cpp
 * Template standalone GUI app for MontaukOS
 *
 * This is a minimal working app that creates a window, renders text,
 * and handles input events. Use it as a starting point for your app.
 */

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>

using namespace gui;

// ============================================================================
// State
// ============================================================================

static TrueTypeFont* g_font;
static int g_win_w = 500;
static int g_win_h = 400;
static int g_click_count = 0;

// ============================================================================
// Drawing helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h && row < bh; row++)
        for (int col = x; col < x + w && col < bw; col++)
            if (row >= 0 && col >= 0)
                px[row * bw + col] = color;
}

static void px_fill_rounded(uint32_t* px, int bw, int bh,
                            int x, int y, int w, int h, int r, uint32_t color) {
    for (int row = y; row < y + h && row < bh; row++) {
        for (int col = x; col < x + w && col < bw; col++) {
            if (row < 0 || col < 0) continue;
            int dx = 0, dy = 0;
            if (col < x + r && row < y + r)       { dx = x + r - col; dy = y + r - row; }
            else if (col >= x+w-r && row < y + r)  { dx = col-(x+w-r-1); dy = y + r - row; }
            else if (col < x + r && row >= y+h-r)  { dx = x + r - col; dy = row-(y+h-r-1); }
            else if (col >= x+w-r && row >= y+h-r) { dx = col-(x+w-r-1); dy = row-(y+h-r-1); }
            if (dx * dx + dy * dy <= r * r)
                px[row * bw + col] = color;
            else if (dx == 0 && dy == 0)
                px[row * bw + col] = color;
        }
    }
}

static void px_text(uint32_t* px, int bw, int bh,
                    int x, int y, const char* text, Color c, int size) {
    if (g_font)
        g_font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}

static void px_hline(uint32_t* px, int bw, int bh,
                     int x, int y, int w, uint32_t color) {
    if (y < 0 || y >= bh) return;
    for (int col = x; col < x + w && col < bw; col++)
        if (col >= 0) px[y * bw + col] = color;
}

// ============================================================================
// Rendering
// ============================================================================

static void render(uint32_t* pixels) {
    // Background
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, g_win_h,
            Color::from_rgb(0xFF, 0xFF, 0xFF).to_pixel());

    // Toolbar
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, 36,
            Color::from_rgb(0xF5, 0xF5, 0xF5).to_pixel());
    px_hline(pixels, g_win_w, g_win_h, 0, 36, g_win_w,
             Color::from_rgb(0xD0, 0xD0, 0xD0).to_pixel());

    // Title in toolbar
    px_text(pixels, g_win_w, g_win_h, 12, 10, "My App",
            Color::from_rgb(0x33, 0x33, 0x33), 18);

    // Content area
    px_text(pixels, g_win_w, g_win_h, 20, 60, "Hello from MontaukOS!",
            Color::from_rgb(0x33, 0x33, 0x33), 18);

    // Click counter
    char buf[64];
    const char* prefix = "Clicks: ";
    int i = 0;
    while (prefix[i]) { buf[i] = prefix[i]; i++; }
    int n = g_click_count;
    if (n == 0) { buf[i++] = '0'; }
    else {
        char tmp[16]; int ti = 0;
        while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
        for (int j = ti - 1; j >= 0; j--) buf[i++] = tmp[j];
    }
    buf[i] = 0;

    px_text(pixels, g_win_w, g_win_h, 20, 90, buf,
            Color::from_rgb(0x66, 0x66, 0x66), 16);

    // A styled button
    px_fill_rounded(pixels, g_win_w, g_win_h,
                    20, 130, 120, 36, 4,
                    Color::from_rgb(0x36, 0x7B, 0xF0).to_pixel());
    px_text(pixels, g_win_w, g_win_h, 42, 139, "Click me",
            Color::from_rgb(0xFF, 0xFF, 0xFF), 16);
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    // Load font
    g_font = new TrueTypeFont();
    if (!g_font->init("0:/fonts/Roboto-Medium.ttf"))
        g_font = nullptr;

    // Create window
    Montauk::WinCreateResult wres;
    montauk::win_create("My App", g_win_w, g_win_h, &wres);
    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

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

        switch (ev.type) {
        case 0: // Keyboard
            if (ev.key.pressed && ev.key.scancode == 0x01)
                goto done; // ESC to close
            break;

        case 1: // Mouse
            if ((ev.mouse.buttons & 0x01) && !(ev.mouse.prev_buttons & 0x01)) {
                // Left click — check if inside button
                int mx = ev.mouse.x, my = ev.mouse.y;
                if (mx >= 20 && mx < 140 && my >= 130 && my < 166)
                    g_click_count++;
            }
            break;

        case 2: // Resize
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
            break;

        case 3: // Close
            goto done;
        }

        render(pixels);
        montauk::win_present(win_id);
    }

done:
    montauk::win_destroy(win_id);
    montauk::exit(0);
}
