/*
    * main.cpp
    * clear - Clear terminal screen
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>

extern "C" void _start() {
    montauk::print("\033[2J");   // Clear entire screen
    montauk::print("\033[H");    // Move cursor to top-left
    montauk::exit(0);
}
