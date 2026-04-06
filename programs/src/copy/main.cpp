/*
    * main.cpp
    * copy - command to copy files
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>

extern "C" void _start() {
    char args[256];
    montauk::getargs((char *)&args, 256);

    const char* p = montauk::skip_spaces((const char *)&args);
    if (*p == '\0') {
        montauk::print("usage: cp [source] [dest]\n");
        montauk::exit(-1);
    }

    // Parse source path
    const char* src = p;
    while (*p && *p != ' ') p++;
    if (*p == '\0') {
        montauk::print("copy: missing destination\n");
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
        montauk::print("copy: missing destination\n");
        montauk::exit(-1);
    }
    const char* dst = p;

    // Open source file
    int srcFd = montauk::open(srcPath);
    if (srcFd < 0) {
        montauk::print("copy: cannot open source file\n");
        montauk::exit(-1);
    }

    // Get source size
    uint64_t size = montauk::getsize(srcFd);
    if (size == 0) {
        montauk::close(srcFd);
        // Create empty destination
        int dstFd = montauk::fcreate(dst);
        if (dstFd < 0) {
            montauk::print("copy: cannot create destination file\n");
            montauk::exit(-1);
        }
        montauk::close(dstFd);
        montauk::exit(0);
    }

    // Allocate buffer for file contents
    uint8_t* buf = (uint8_t*)montauk::alloc(size);
    if (!buf) {
        montauk::close(srcFd);
        montauk::print("copy: out of memory\n");
        montauk::exit(-1);
    }

    // Read source file
    int bytesRead = montauk::read(srcFd, buf, 0, size);
    montauk::close(srcFd);
    if (bytesRead < 0 || (uint64_t)bytesRead != size) {
        montauk::free(buf);
        montauk::print("copy: failed to read source file\n");
        montauk::exit(-1);
    }

    // Create destination file
    int dstFd = montauk::fcreate(dst);
    if (dstFd < 0) {
        montauk::free(buf);
        montauk::print("copy: cannot create destination file\n");
        montauk::exit(-1);
    }

    // Write to destination
    int written = montauk::fwrite(dstFd, buf, 0, size);
    montauk::close(dstFd);
    montauk::free(buf);

    if (written < 0 || (uint64_t)written != size) {
        montauk::print("copy: failed to write destination file\n");
        montauk::exit(-1);
    }

    montauk::exit(0);
}
