/*
    * main.cpp
    * whoami - Print the current username
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/config.h>

extern "C" void _start() {
    auto doc = montauk::config::load("session");
    const char* name = doc.get_string("session.username", "");
    if (name[0]) {
        montauk::print(name);
        montauk::putchar('\n');
    } else {
        montauk::print("unknown\n");
    }
    doc.destroy();
    montauk::exit(0);
}
