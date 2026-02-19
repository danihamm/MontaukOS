/*
    * Vfs.hpp
    * Virtual File System with numerical logical drive identifiers
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <cstddef>

namespace Fs::Vfs {

    static constexpr int MaxDrives = 16;
    static constexpr int MaxHandles = 64;

    struct FsDriver {
        int (*Open)(const char* path);
        int (*Read)(int handle, uint8_t* buffer, uint64_t offset, uint64_t size);
        uint64_t (*GetSize)(int handle);
        void (*Close)(int handle);
        int (*ReadDir)(const char* path, const char** outNames, int maxEntries);
        int (*Write)(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size);
        int (*Create)(const char* path);
    };

    void Initialize();
    int RegisterDrive(int driveNumber, FsDriver* driver);

    int VfsOpen(const char* path);
    int VfsRead(int handle, uint8_t* buffer, uint64_t offset, uint64_t size);
    int VfsWrite(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size);
    int VfsCreate(const char* path);
    uint64_t VfsGetSize(int handle);
    void VfsClose(int handle);
    int VfsReadDir(const char* path, const char** outNames, int maxEntries);

}
