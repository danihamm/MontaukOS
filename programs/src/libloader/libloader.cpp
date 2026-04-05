/*
    * libloader.cpp
    * Dynamic library loading implementation for MontaukOS
    * Copyright (c) 2026 Daniel Hammer
*/

#include <libloader/libloader.h>
#include <montauk/syscall.h>
#include <libc/string.h>
#include <stdlib.h>

namespace libloader {

// Forward declaration
static uint64_t hexToUint64(const char* str);

// Table of currently loaded libraries
static LibHandle g_loadedLibs[MaxLoadedLibs];
static int g_libCount = 0;

static LibHandle* findLoadedLib(const char* path) {
    for (int i = 0; i < g_libCount; i++) {
        if (g_loadedLibs[i].handle > 0 &&
            strcmp(g_loadedLibs[i].path, path) == 0) {
            return &g_loadedLibs[i];
        }
    }
    return nullptr;
}

static bool parseSymbolFile(const char* symPath, LibHandle* lib) {
    int fd = montauk::open(symPath);
    if (fd < 0) {
        // No symbol file - that's okay, we'll still load the lib
        lib->symbolCount = 0;
        lib->symbols = nullptr;
        return true;
    }

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > 65536) {
        montauk::close(fd);
        lib->symbolCount = 0;
        lib->symbols = nullptr;
        return true;
    }

    char* buf = (char*)montauk::alloc(size + 1);
    if (!buf) {
        montauk::close(fd);
        return false;
    }

    int read = montauk::read(fd, (uint8_t*)buf, 0, size);
    montauk::close(fd);
    if (read != (int)size) {
        montauk::free(buf);
        return false;
    }
    buf[size] = '\0';

    // Count lines first
    int lineCount = 0;
    for (uint64_t i = 0; i < size; i++) {
        if (buf[i] == '\n') lineCount++;
    }

    if (lineCount == 0) {
        montauk::free(buf);
        lib->symbolCount = 0;
        lib->symbols = nullptr;
        return true;
    }

    lib->symbols = (LibHandle::SymbolEntry*)montauk::alloc(lineCount * sizeof(LibHandle::SymbolEntry));
    if (!lib->symbols) {
        montauk::free(buf);
        return false;
    }

    lib->symbolCount = lineCount;

    // Parse lines: "symbol_name,offset_hex"
    int entryIdx = 0;
    char* lineStart = buf;
    for (uint64_t i = 0; i <= size; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            buf[i] = '\0';
            char* comma = strchr(lineStart, ',');
            if (comma && entryIdx < lineCount) {
                size_t nameLen = comma - lineStart;
                if (nameLen < sizeof(lib->symbols[0].name) - 1) {
                    memcpy(lib->symbols[entryIdx].name, lineStart, nameLen);
                    lib->symbols[entryIdx].name[nameLen] = '\0';
                    lib->symbols[entryIdx].offset = hexToUint64(comma + 1);
                    entryIdx++;
                }
            }
            lineStart = &buf[i + 1];
        }
    }

    lib->symbolCount = entryIdx;
    montauk::free(buf);
    return true;
}

static uint64_t hexToUint64(const char* str) {
    uint64_t result = 0;
    while (*str) {
        char c = *str;
        uint64_t val = 0;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else break;
        result = (result << 4) | val;
        str++;
    }
    return result;
}

LibHandle* dlopen(const char* path) {
    if (!path) return nullptr;

    // Check if already loaded
    LibHandle* existing = findLoadedLib(path);
    if (existing) {
        // Just return existing handle
        return existing;
    }

    if (g_libCount >= MaxLoadedLibs) {
        return nullptr;
    }

    // Load the library via syscall
    int handle = montauk::load_lib(path);
    if (handle <= 0) {
        return nullptr;
    }

    // Create symbol file path: append ".sym"
    char symPath[320];
    size_t pathLen = strlen(path);
    if (pathLen >= sizeof(symPath) - 5) return nullptr;

    strcpy(symPath, path);
    strcpy(symPath + pathLen, ".sym");

    LibHandle* lib = &g_loadedLibs[g_libCount++];
    lib->handle = handle;
    lib->base = 0;  // Will be retrieved on first dlsym
    strcpy(lib->path, path);
    lib->symbolCount = 0;
    lib->symbols = nullptr;

    // Parse symbol file
    parseSymbolFile(symPath, lib);

    return lib;
}

void* dlsym(LibHandle* lib, const char* symbolName) {
    if (!lib || !symbolName) return nullptr;

    // Get base address if we don't have it
    if (lib->base == 0) {
        // The base is stored in the handle - we need a syscall to get it
        // For now, we'll use a workaround: calculate base from slot
        // handle = slot + 1, so slot = handle - 1
        // base = 0x60000000 + slot * 0x200000
        int slot = lib->handle - 1;
        lib->base = 0x60000000ULL + (uint64_t)slot * 0x200000ULL;
    }

    // Look up in symbol table
    for (int i = 0; i < lib->symbolCount; i++) {
        if (strcmp(lib->symbols[i].name, symbolName) == 0) {
            uint64_t offset = lib->symbols[i].offset;
            return (void*)montauk::dlsym(lib->handle, offset);
        }
    }

    return nullptr;  // Symbol not found
}

int dlclose(LibHandle* lib) {
    if (!lib) return -1;

    int result = montauk::unload_lib(lib->handle);
    if (result == 0) {
        // Free symbol table if allocated
        if (lib->symbols) {
            montauk::free((void*)lib->symbols);
            lib->symbols = nullptr;
        }
        lib->handle = 0;
        lib->base = 0;
        lib->symbolCount = 0;
        lib->path[0] = '\0';
    }

    return result;
}

}  // namespace libloader
