/*
    * Filesystem.hpp
    * SYS_OPEN, SYS_READ, SYS_GETSIZE, SYS_CLOSE, SYS_READDIR,
    * SYS_FWRITE, SYS_FCREATE syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Fs/Vfs.hpp>
#include <Sched/Scheduler.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Libraries/Memory.hpp>
#include <Ipc/Ipc.hpp>
#include "Path.hpp"

namespace Montauk {
    static int Sys_Open(const char* path) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;
        return Ipc::OpenFileHandle(resolved);
    }

    static int Sys_Read(int handle, uint8_t* buffer, uint64_t offset, uint64_t size) {
        return Ipc::FileReadHandle(handle, buffer, offset, size);
    }

    static uint64_t Sys_GetSize(int handle) {
        return Ipc::FileGetSizeHandle(handle);
    }

    static void Sys_Close(int handle) {
        Ipc::CloseHandle(handle);
    }

    static int Sys_ReadDir(const char* path, const char** outNames, int maxEntries) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;

        // Get entries from VFS into a kernel-local array
        const char* kernelNames[256];
        int max = maxEntries;
        if (max > 256) max = 256;
        int count = Fs::Vfs::VfsReadDir(resolved, kernelNames, max);
        if (count <= 0) return count;

        // Allocate a user-accessible page for string data via process heap
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return -1;

        // Use a rotating ring of scratch pages below the heap instead of
        // bumping heapNext on every call. This keeps repeated directory scans
        // from leaking user heap space while still allowing nested callers to
        // hold multiple readdir results at once.
        uint32_t slot = proc->readdirCursor % Sched::UserReadDirSlots;
        proc->readdirCursor = (slot + 1) % Sched::UserReadDirSlots;

        uint64_t userVa = Sched::UserReadDirBase + (uint64_t)slot * 0x1000ULL;
        uint64_t physAddr = Memory::VMM::Paging::GetPhysAddr(proc->pml4Phys, userVa);
        uint8_t* pageBuf = nullptr;

        if (physAddr == 0) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) return -1;
            physAddr = Memory::SubHHDM((uint64_t)page);
            if (!Memory::VMM::Paging::MapUserIn(proc->pml4Phys, physAddr, userVa)) {
                Memory::g_pfa->Free(page);
                return -1;
            }
            pageBuf = (uint8_t*)page;
        } else {
            pageBuf = (uint8_t*)Memory::HHDM(physAddr);
            memset(pageBuf, 0, 0x1000);
        }

        // Copy strings into the user page and write pointers to outNames
        uint64_t offset = 0;
        int copied = 0;
        for (int i = 0; i < count; i++) {
            int len = Lib::strlen(kernelNames[i]) + 1;
            if (offset + len > 0x1000) break;
            memcpy(pageBuf + offset, kernelNames[i], len);
            outNames[i] = (const char*)(userVa + offset);
            offset += len;
            copied++;
        }

        return copied;
    }

    static int Sys_FWrite(int handle, const uint8_t* data, uint64_t offset, uint64_t size) {
        return Ipc::FileWriteHandle(handle, data, offset, size);
    }

    static int Sys_FCreate(const char* path) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;
        return Ipc::CreateFileHandle(resolved);
    }

    static int Sys_FDelete(const char* path) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;
        return Fs::Vfs::VfsDelete(resolved);
    }

    static int Sys_FMkdir(const char* path) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;
        return Fs::Vfs::VfsMkdir(resolved);
    }

    static int Sys_FRename(const char* oldPath, const char* newPath) {
        char resolvedOld[256];
        char resolvedNew[256];
        if (!ResolveProcessPath(oldPath, resolvedOld, sizeof(resolvedOld))) return -1;
        if (!ResolveProcessPath(newPath, resolvedNew, sizeof(resolvedNew))) return -1;
        return Fs::Vfs::VfsRename(resolvedOld, resolvedNew);
    }

    static int Sys_DriveList(int* outDrives, int maxEntries) {
        return Fs::Vfs::VfsDriveList(outDrives, maxEntries);
    }
};
