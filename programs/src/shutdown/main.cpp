/*
    * main.cpp
    * shutdown - Shut down the system
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>

extern "C" void _start() {
    montauk::print("Shutting down...\n");
    montauk::shutdown();
}
