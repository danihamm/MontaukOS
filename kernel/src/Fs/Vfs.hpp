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

    struct BackendFile {
        int driveNumber;
        int localHandle;
    };

    struct FsDriver {
        int (*Open)(const char* path);
        int (*Read)(int handle, uint8_t* buffer, uint64_t offset, uint64_t size);
        uint64_t (*GetSize)(int handle);
        void (*Close)(int handle);
        int (*ReadDir)(const char* path, const char** outNames, int maxEntries);
        int (*Write)(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size);
        int (*Create)(const char* path);
        int (*Delete)(const char* path);
        int (*Mkdir)(const char* path);
        int (*Rename)(const char* oldPath, const char* newPath);
    };

    void Initialize();
    int RegisterDrive(int driveNumber, FsDriver* driver);

    int OpenBackendFile(const char* path, BackendFile& outFile);
    int CreateBackendFile(const char* path, BackendFile& outFile);
    int ReadBackendFile(const BackendFile& file, uint8_t* buffer, uint64_t offset, uint64_t size);
    int WriteBackendFile(const BackendFile& file, const uint8_t* buffer, uint64_t offset, uint64_t size);
    uint64_t GetBackendFileSize(const BackendFile& file);
    bool BackendFileCanWrite(const BackendFile& file);
    void CloseBackendFile(BackendFile& file);

    int VfsDelete(const char* path);
    int VfsReadDir(const char* path, const char** outNames, int maxEntries);
    int VfsMkdir(const char* path);
    int VfsRename(const char* oldPath, const char* newPath);

    // Returns number of registered drives, fills outDrives[] with their indices
    int VfsDriveList(int* outDrives, int maxEntries);

}
