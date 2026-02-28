/*
    * main.cpp
    * reset - Reboot the system
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>

extern "C" void _start() {
    montauk::print("Rebooting...\n");
    montauk::reset();
}
