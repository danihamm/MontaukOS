/*
    * libloader.h
    * Dynamic library loading interface for MontaukOS
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <montauk/syscall.h>

// Library handle - opaque to users
struct LibHandle {
    int handle;       // syscall handle (slot + 1)
    uint64_t base;    // base virtual address
    char path[256];
    int symbolCount;
    struct SymbolEntry {
        char name[64];
        uint64_t offset;
    }* symbols;
};

namespace libloader {

// Maximum libraries that can be loaded per process
static constexpr int MaxLoadedLibs = 8;

// Load a shared library.
// Returns a LibHandle* on success, or nullptr on failure.
LibHandle* dlopen(const char* path);

// Look up a symbol in a loaded library.
// Returns the function/data pointer, or nullptr if not found.
void* dlsym(LibHandle* lib, const char* symbolName);

// Unload a library.
// Returns 0 on success, -1 on failure.
int dlclose(LibHandle* lib);

}  // namespace libloader
