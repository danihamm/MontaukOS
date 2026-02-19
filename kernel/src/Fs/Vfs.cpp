/*
    * Vfs.cpp
    * Virtual File System with numerical logical drive identifiers
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Vfs.hpp"
#include <Terminal/Terminal.hpp>

namespace Fs::Vfs {

    struct HandleEntry {
        bool inUse;
        int driveNumber;
        int localHandle;
    };

    static FsDriver* driveTable[MaxDrives];
    static HandleEntry handleTable[MaxHandles];

    // Parse "N:/path" into drive number and local path.
    // Returns true on success, sets outDrive and outPath.
    static bool ParsePath(const char* path, int& outDrive, const char*& outPath) {
        if (path == nullptr) return false;

        // Parse decimal drive number before ':'
        int drive = 0;
        int i = 0;
        bool hasDigit = false;

        while (path[i] >= '0' && path[i] <= '9') {
            drive = drive * 10 + (path[i] - '0');
            hasDigit = true;
            i++;
        }

        if (!hasDigit) return false;
        if (path[i] != ':') return false;

        // Everything after "N:" is the local path
        outDrive = drive;
        outPath = &path[i + 1];
        return true;
    }

    static int AllocHandle() {
        for (int i = 0; i < MaxHandles; i++) {
            if (!handleTable[i].inUse) return i;
        }
        return -1;
    }

    void Initialize() {
        for (int i = 0; i < MaxDrives; i++) {
            driveTable[i] = nullptr;
        }
        for (int i = 0; i < MaxHandles; i++) {
            handleTable[i].inUse = false;
        }

        Kt::KernelLogStream(Kt::OK, "VFS") << "Initialized (" << MaxDrives << " drives, " << MaxHandles << " handles)";
    }

    int RegisterDrive(int driveNumber, FsDriver* driver) {
        if (driveNumber < 0 || driveNumber >= MaxDrives) return -1;
        if (driver == nullptr) return -1;

        driveTable[driveNumber] = driver;
        Kt::KernelLogStream(Kt::OK, "VFS") << "Registered drive " << driveNumber;
        return 0;
    }

    int VfsOpen(const char* path) {
        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "Invalid path format";
            return -1;
        }

        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "Drive " << drive << " not registered";
            return -1;
        }

        int localHandle = driveTable[drive]->Open(localPath);
        if (localHandle < 0) return -1;

        int globalHandle = AllocHandle();
        if (globalHandle < 0) {
            driveTable[drive]->Close(localHandle);
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "No free handles";
            return -1;
        }

        handleTable[globalHandle].inUse = true;
        handleTable[globalHandle].driveNumber = drive;
        handleTable[globalHandle].localHandle = localHandle;

        return globalHandle;
    }

    int VfsRead(int handle, uint8_t* buffer, uint64_t offset, uint64_t size) {
        if (handle < 0 || handle >= MaxHandles || !handleTable[handle].inUse) return -1;

        HandleEntry& entry = handleTable[handle];
        return driveTable[entry.driveNumber]->Read(entry.localHandle, buffer, offset, size);
    }

    uint64_t VfsGetSize(int handle) {
        if (handle < 0 || handle >= MaxHandles || !handleTable[handle].inUse) return 0;

        HandleEntry& entry = handleTable[handle];
        return driveTable[entry.driveNumber]->GetSize(entry.localHandle);
    }

    void VfsClose(int handle) {
        if (handle < 0 || handle >= MaxHandles || !handleTable[handle].inUse) return;

        HandleEntry& entry = handleTable[handle];
        driveTable[entry.driveNumber]->Close(entry.localHandle);
        entry.inUse = false;
    }

    int VfsWrite(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size) {
        if (handle < 0 || handle >= MaxHandles || !handleTable[handle].inUse) return -1;

        HandleEntry& entry = handleTable[handle];
        if (driveTable[entry.driveNumber]->Write == nullptr) return -1;
        return driveTable[entry.driveNumber]->Write(entry.localHandle, buffer, offset, size);
    }

    int VfsCreate(const char* path) {
        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "Invalid path format for Create";
            return -1;
        }

        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "Drive " << drive << " not registered";
            return -1;
        }

        if (driveTable[drive]->Create == nullptr) return -1;

        int localHandle = driveTable[drive]->Create(localPath);
        if (localHandle < 0) return -1;

        int globalHandle = AllocHandle();
        if (globalHandle < 0) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "No free handles";
            return -1;
        }

        handleTable[globalHandle].inUse = true;
        handleTable[globalHandle].driveNumber = drive;
        handleTable[globalHandle].localHandle = localHandle;

        return globalHandle;
    }

    int VfsReadDir(const char* path, const char** outNames, int maxEntries) {
        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "Invalid path format for ReadDir";
            return -1;
        }

        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "VFS") << "Drive " << drive << " not registered";
            return -1;
        }

        return driveTable[drive]->ReadDir(localPath, outNames, maxEntries);
    }

}
