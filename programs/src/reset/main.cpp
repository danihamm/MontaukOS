/*
    * main.cpp
    * reset - Reboot the system
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>

extern "C" void _start() {
    zenith::print("Rebooting...\n");
    zenith::reset();
}
