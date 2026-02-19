/*
    * main.cpp
    * clear - Clear terminal screen and framebuffer
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>

extern "C" void _start() {
    // Clear the raw framebuffer (needed after graphical programs like DOOM)
    Zenith::FbInfo fb;
    zenith::fb_info(&fb);
    uint8_t* pixels = (uint8_t*)zenith::fb_map();
    if (pixels) {
        for (uint64_t y = 0; y < fb.height; y++) {
            uint32_t* row = (uint32_t*)(pixels + y * fb.pitch);
            for (uint64_t x = 0; x < fb.width; x++) {
                row[x] = 0x00000000;
            }
        }
    }

    // Reset the text console
    zenith::print("\033[2J");   // Clear entire screen
    zenith::print("\033[H");    // Move cursor to top-left
    zenith::exit(0);
}
