/*
    * main.cpp
    * cat - Display file contents
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>

extern "C" void _start() {
    char args[256];
    int len = montauk::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        montauk::print("Usage: cat <filename>\n");
        montauk::exit(1);
    }

    // Build VFS path. If the path already starts with "<digit>:", use as-is.
    // Otherwise, prefix "0:/" to it.
    char path[256];
    bool hasPrefix = false;
    if (args[0] >= '0' && args[0] <= '9' && args[1] == ':') {
        hasPrefix = true;
    }

    int i = 0;
    if (!hasPrefix) {
        path[0] = '0'; path[1] = ':'; path[2] = '/';
        i = 3;
    }
    int j = 0;
    while (args[j] && i < 255) {
        path[i++] = args[j++];
    }
    path[i] = '\0';

    int handle = montauk::open(path);
    if (handle < 0) {
        montauk::print("cat: cannot open '");
        montauk::print(args);
        montauk::print("'\n");
        montauk::exit(1);
    }

    uint64_t size = montauk::getsize(handle);
    if (size == 0) {
        montauk::close(handle);
        montauk::exit(0);
    }

    uint8_t buf[512];
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t chunk = size - offset;
        if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;
        int bytesRead = montauk::read(handle, buf, offset, chunk);
        if (bytesRead <= 0) break;
        buf[bytesRead] = '\0';
        montauk::print((const char*)buf);
        offset += bytesRead;
    }

    montauk::close(handle);
    montauk::putchar('\n');
    montauk::exit(0);
}
