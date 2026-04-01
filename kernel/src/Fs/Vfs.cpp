/*
    * Vfs.cpp
    * Virtual File System with numerical logical drive identifiers
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Vfs.hpp"
#include <Terminal/Terminal.hpp>
#include <CppLib/Spinlock.hpp>

namespace Fs::Vfs {

    static FsDriver* driveTable[MaxDrives];

    // Protects handle table and driver dispatch from concurrent CPU access.
    // Uses Mutex (not Spinlock) so interrupts stay enabled while held --
    // VFS is never called from interrupt context.
    static kcp::Mutex vfsLock;

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

    void Initialize() {
        for (int i = 0; i < MaxDrives; i++) {
            driveTable[i] = nullptr;
        }

        Kt::KernelLogStream(Kt::OK, "VFS") << "Initialized (" << MaxDrives << " drives)";
    }

    int RegisterDrive(int driveNumber, FsDriver* driver) {
        if (driveNumber < 0 || driveNumber >= MaxDrives) return -1;
        if (driver == nullptr) return -1;

        driveTable[driveNumber] = driver;
        Kt::KernelLogStream(Kt::OK, "VFS") << "Registered drive " << driveNumber;
        return 0;
    }

    int OpenBackendFile(const char* path, BackendFile& outFile) {
        outFile.driveNumber = -1;
        outFile.localHandle = -1;

        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) return -1;
        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) return -1;

        vfsLock.Acquire();
        int localHandle = driveTable[drive]->Open(localPath);
        vfsLock.Release();
        if (localHandle < 0) return -1;

        outFile.driveNumber = drive;
        outFile.localHandle = localHandle;
        return 0;
    }

    int ReadBackendFile(const BackendFile& file, uint8_t* buffer, uint64_t offset, uint64_t size) {
        vfsLock.Acquire();
        if (file.driveNumber < 0 || file.driveNumber >= MaxDrives || driveTable[file.driveNumber] == nullptr ||
            file.localHandle < 0) {
            vfsLock.Release();
            return -1;
        }

        int result = driveTable[file.driveNumber]->Read(file.localHandle, buffer, offset, size);
        vfsLock.Release();
        return result;
    }

    uint64_t GetBackendFileSize(const BackendFile& file) {
        vfsLock.Acquire();
        if (file.driveNumber < 0 || file.driveNumber >= MaxDrives || driveTable[file.driveNumber] == nullptr ||
            file.localHandle < 0) {
            vfsLock.Release();
            return 0;
        }

        uint64_t result = driveTable[file.driveNumber]->GetSize(file.localHandle);
        vfsLock.Release();
        return result;
    }

    bool BackendFileCanWrite(const BackendFile& file) {
        vfsLock.Acquire();
        bool canWrite = file.driveNumber >= 0 && file.driveNumber < MaxDrives &&
            driveTable[file.driveNumber] != nullptr &&
            file.localHandle >= 0 &&
            driveTable[file.driveNumber]->Write != nullptr;
        vfsLock.Release();
        return canWrite;
    }

    void CloseBackendFile(BackendFile& file) {
        vfsLock.Acquire();
        if (file.driveNumber < 0 || file.driveNumber >= MaxDrives || driveTable[file.driveNumber] == nullptr ||
            file.localHandle < 0) {
            vfsLock.Release();
            file.driveNumber = -1;
            file.localHandle = -1;
            return;
        }

        driveTable[file.driveNumber]->Close(file.localHandle);
        vfsLock.Release();

        file.driveNumber = -1;
        file.localHandle = -1;
    }

    int WriteBackendFile(const BackendFile& file, const uint8_t* buffer, uint64_t offset, uint64_t size) {
        vfsLock.Acquire();
        if (file.driveNumber < 0 || file.driveNumber >= MaxDrives || driveTable[file.driveNumber] == nullptr ||
            file.localHandle < 0) {
            vfsLock.Release();
            return -1;
        }

        if (driveTable[file.driveNumber]->Write == nullptr) { vfsLock.Release(); return -1; }
        int result = driveTable[file.driveNumber]->Write(file.localHandle, buffer, offset, size);
        vfsLock.Release();
        return result;
    }

    int CreateBackendFile(const char* path, BackendFile& outFile) {
        outFile.driveNumber = -1;
        outFile.localHandle = -1;

        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) return -1;
        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) return -1;
        if (driveTable[drive]->Create == nullptr) return -1;

        vfsLock.Acquire();

        int localHandle = driveTable[drive]->Create(localPath);
        vfsLock.Release();
        if (localHandle < 0) return -1;

        outFile.driveNumber = drive;
        outFile.localHandle = localHandle;
        return 0;
    }

    int VfsDelete(const char* path) {
        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) return -1;
        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) return -1;
        if (driveTable[drive]->Delete == nullptr) return -1;

        vfsLock.Acquire();
        int result = driveTable[drive]->Delete(localPath);
        vfsLock.Release();
        return result;
    }

    int VfsMkdir(const char* path) {
        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) return -1;
        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) return -1;
        if (driveTable[drive]->Mkdir == nullptr) return -1;

        vfsLock.Acquire();
        int result = driveTable[drive]->Mkdir(localPath);
        vfsLock.Release();
        return result;
    }

    int VfsDriveList(int* outDrives, int maxEntries) {
        vfsLock.Acquire();
        int count = 0;
        for (int i = 0; i < MaxDrives && count < maxEntries; i++) {
            if (driveTable[i] != nullptr) {
                outDrives[count++] = i;
            }
        }
        vfsLock.Release();
        return count;
    }

    int VfsRename(const char* oldPath, const char* newPath) {
        int oldDrive, newDrive;
        const char* oldLocal;
        const char* newLocal;

        if (!ParsePath(oldPath, oldDrive, oldLocal)) return -1;
        if (!ParsePath(newPath, newDrive, newLocal)) return -1;

        // Cross-drive rename not supported
        if (oldDrive != newDrive) return -1;
        if (oldDrive < 0 || oldDrive >= MaxDrives || driveTable[oldDrive] == nullptr) return -1;
        if (driveTable[oldDrive]->Rename == nullptr) return -1;

        vfsLock.Acquire();
        int result = driveTable[oldDrive]->Rename(oldLocal, newLocal);
        vfsLock.Release();
        return result;
    }

    int VfsReadDir(const char* path, const char** outNames, int maxEntries) {
        int drive;
        const char* localPath;

        if (!ParsePath(path, drive, localPath)) return -1;
        if (drive < 0 || drive >= MaxDrives || driveTable[drive] == nullptr) return -1;

        vfsLock.Acquire();
        int result = driveTable[drive]->ReadDir(localPath, outNames, maxEntries);
        vfsLock.Release();
        return result;
    }

}
