/*
    * libhello.cpp
    * Sample shared GUI library for MontaukOS
    * Creates a window and renders text as a demo of the shared library + GUI system
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>

// Exported demo function - creates a window and renders until closed
extern "C" int hello_run() {
    // Create window
    Montauk::WinCreateResult wres;
    if (montauk::win_create("libhello Demo", 320, 200, &wres) < 0 || wres.id < 0) {
        return -1;
    }

    int winId = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;
    int width = 200;
    int height = 320;

    // Simple render - fill with a color pattern
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Dark blue background
            pixels[y * width + x] = 0xFF1a1a2e;
        }
    }

    // Present the initial frame
    montauk::win_present(winId);

    // Event loop
    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(winId, &ev);

        if (r < 0) {
            break;
        }
        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) {  // close
            break;
        }
        if (ev.type == 2) {  // resize
            width = ev.resize.w;
            height = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(winId, width, height);

            // Re-render on resize
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    pixels[y * width + x] = 0xFF1a1a2e;
                }
            }
            montauk::win_present(winId);
        }
    }

    montauk::win_destroy(winId);
    return 0;
}
