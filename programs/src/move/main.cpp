/*
    * main.cpp
    * move - command to move/rename files
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>

extern "C" void _start() {
    char args[256];
    montauk::getargs((char *)&args, 256);

    const char* p = montauk::skip_spaces((const char *)&args);
    if (*p == '\0') {
        montauk::print("usage: move [source] [dest]\n");
        montauk::exit(-1);
    }

    // Parse source path
    const char* src = p;
    while (*p && *p != ' ') p++;
    if (*p == '\0') {
        montauk::print("move: missing destination\n");
        montauk::exit(-1);
    }
    char srcPath[256];
    size_t len = p - src;
    if (len >= sizeof(srcPath)) len = sizeof(srcPath) - 1;
    for (size_t i = 0; i < len; i++) srcPath[i] = src[i];
    srcPath[len] = '\0';

    // Parse dest path
    p = montauk::skip_spaces(p);
    if (*p == '\0') {
        montauk::print("move: missing destination\n");
        montauk::exit(-1);
    }
    const char* dst = p;

    if (montauk::frename(srcPath, dst) < 0) {
        montauk::print("move: failed to move file\n");
        montauk::exit(-1);
    }

    montauk::exit(0);
}
