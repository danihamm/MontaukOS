/*
    * main.cpp
    * cat - Display file contents
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>

extern "C" void _start() {
    char args[256];
    int len = zenith::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        zenith::print("Usage: cat <filename>\n");
        zenith::exit(1);
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

    int handle = zenith::open(path);
    if (handle < 0) {
        zenith::print("cat: cannot open '");
        zenith::print(args);
        zenith::print("'\n");
        zenith::exit(1);
    }

    uint64_t size = zenith::getsize(handle);
    if (size == 0) {
        zenith::close(handle);
        zenith::exit(0);
    }

    uint8_t buf[512];
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t chunk = size - offset;
        if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;
        int bytesRead = zenith::read(handle, buf, offset, chunk);
        if (bytesRead <= 0) break;
        buf[bytesRead] = '\0';
        zenith::print((const char*)buf);
        offset += bytesRead;
    }

    zenith::close(handle);
    zenith::putchar('\n');
    zenith::exit(0);
}
