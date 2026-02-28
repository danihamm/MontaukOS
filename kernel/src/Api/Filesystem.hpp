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

namespace Montauk {
    static int Sys_Open(const char* path) {
        return Fs::Vfs::VfsOpen(path);
    }

    static int Sys_Read(int handle, uint8_t* buffer, uint64_t offset, uint64_t size) {
        return Fs::Vfs::VfsRead(handle, buffer, offset, size);
    }

    static uint64_t Sys_GetSize(int handle) {
        return Fs::Vfs::VfsGetSize(handle);
    }

    static void Sys_Close(int handle) {
        Fs::Vfs::VfsClose(handle);
    }

    static int Sys_ReadDir(const char* path, const char** outNames, int maxEntries) {
        // Get entries from VFS into a kernel-local array
        const char* kernelNames[64];
        int max = maxEntries;
        if (max > 64) max = 64;
        int count = Fs::Vfs::VfsReadDir(path, kernelNames, max);
        if (count <= 0) return count;

        // Allocate a user-accessible page for string data via process heap
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return -1;

        void* page = Memory::g_pfa->AllocateZeroed();
        if (page == nullptr) return -1;
        uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
        uint64_t userVa = proc->heapNext;
        proc->heapNext += 0x1000;
        Memory::VMM::Paging::MapUserIn(proc->pml4Phys, physAddr, userVa);

        // Copy strings into the user page and write pointers to outNames
        uint64_t offset = 0;
        uint8_t* pageBuf = (uint8_t*)Memory::HHDM(physAddr);
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
        return Fs::Vfs::VfsWrite(handle, data, offset, size);
    }

    static int Sys_FCreate(const char* path) {
        return Fs::Vfs::VfsCreate(path);
    }
};