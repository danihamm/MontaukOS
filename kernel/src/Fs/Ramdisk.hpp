/*
    * Ramdisk.hpp
    * USTAR tar-based ramdisk filesystem backed by Limine modules
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <cstddef>

namespace Fs::Ramdisk {

    static constexpr int MaxFiles = 128;
    static constexpr int MaxNameLen = 100;

    struct FileEntry {
        char name[MaxNameLen];
        uint8_t* data;
        uint64_t size;
        uint64_t capacity;
        bool isDirectory;
        bool heapAllocated;
    };

    void Initialize(void* moduleData, uint64_t moduleSize);

    int Open(const char* path);
    int Read(int handle, uint8_t* buffer, uint64_t offset, uint64_t size);
    int Write(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size);
    int Create(const char* path);
    uint64_t GetSize(int handle);
    void Close(int handle);

    int ReadDir(const char* path, const char** outNames, int maxEntries);
    int GetFileCount();

}
