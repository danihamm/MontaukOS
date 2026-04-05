/*
    * LibSyscall.hpp
    * SYS_LOAD_LIB, SYS_UNLOAD_LIB, SYS_DLSYM syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#include <cstdint>
#include <Sched/Scheduler.hpp>
#include <Sched/ElfLoader.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Terminal/Terminal.hpp>
#include <Libraries/Memory.hpp>
#include <Libraries/String.hpp>

namespace Montauk {

    // Maximum libraries per process
    static constexpr int MaxLibsPerProcess = 8;
    static constexpr int MaxProcesses = 64;

    // Library entry tracking
    struct LibEntry {
        char path[128];
        uint64_t baseAddr;
        uint32_t refcount;
        bool inUse;
    };

    // Per-process library table
    static LibEntry g_libTable[MaxProcesses][MaxLibsPerProcess];

    // Initialize library table for a process slot
    static void InitLibTable(int slot) {
        if (slot < 0 || slot >= MaxProcesses) return;
        for (int i = 0; i < MaxLibsPerProcess; i++) {
            g_libTable[slot][i].inUse = false;
            g_libTable[slot][i].refcount = 0;
            g_libTable[slot][i].baseAddr = 0;
            g_libTable[slot][i].path[0] = '\0';
        }
    }

    // Load a shared library into the current process's address space.
    // Returns a library handle (slot index + 1) on success, or 0 on failure.
    // The library is loaded at a fixed address based on the slot.
    static uint64_t Sys_LoadLib(const char* path) {
        int slot = GetCurrentSlot();
        if (slot < 0) return 0;

        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;

        // Validate path
        if (path == nullptr) return 0;
        size_t pathLen = Lib::strlen(path);
        if (pathLen == 0 || pathLen >= sizeof(LibEntry::path)) return 0;

        // Find a free slot or reuse an existing slot with the same path
        int libSlot = -1;
        for (int i = 0; i < MaxLibsPerProcess; i++) {
            if (g_libTable[slot][i].inUse) {
                if (Lib::strncmp(g_libTable[slot][i].path, path, sizeof(LibEntry::path)) == 0) {
                    // Same library already loaded - just increment refcount
                    g_libTable[slot][i].refcount++;
                    return (uint64_t)(i + 1);  // Handle = slot + 1 (0 = invalid)
                }
            } else if (libSlot < 0) {
                libSlot = i;
            }
        }

        if (libSlot < 0) {
            Kt::KernelLogStream(Kt::ERROR, "Lib") << "No free library slots";
            return 0;
        }

        // Load the library into the process's address space
        uint64_t libBase = Sched::ElfLoadLib(path, proc->pml4Phys, libSlot);
        if (libBase == 0) {
            Kt::KernelLogStream(Kt::ERROR, "Lib") << "Failed to load library: " << path;
            return 0;
        }

        // Store library info
        g_libTable[slot][libSlot].inUse = true;
        g_libTable[slot][libSlot].refcount = 1;
        g_libTable[slot][libSlot].baseAddr = libBase;
        Lib::strncpy(g_libTable[slot][libSlot].path, path, sizeof(LibEntry::path));

        Kt::KernelLogStream(Kt::OK, "Lib") << "Loaded library: " << path << " at base " << kcp::hex << libBase << kcp::dec;

        return (uint64_t)(libSlot + 1);  // Handle = slot + 1 (0 = invalid)
    }

    // Unload a shared library.
    // Decrements refcount; actually unmaps when refcount reaches 0.
    static int Sys_UnloadLib(uint64_t handle) {
        int slot = GetCurrentSlot();
        if (slot < 0) return -1;

        int libSlot = (int)handle - 1;
        if (libSlot < 0 || libSlot >= MaxLibsPerProcess) return -1;

        if (!g_libTable[slot][libSlot].inUse) return -1;

        g_libTable[slot][libSlot].refcount--;
        if (g_libTable[slot][libSlot].refcount == 0) {
            // Actually unload - free the pages
            uint64_t libBase = g_libTable[slot][libSlot].baseAddr;
            uint64_t libEnd = libBase + Sched::LIB_MAX_SIZE;

            auto* proc = Sched::GetCurrentProcessPtr();
            if (proc != nullptr) {
                // Unmap all pages in the library region
                for (uint64_t va = libBase; va < libEnd; va += 0x1000) {
                    uint64_t physAddr = Memory::VMM::Paging::GetPhysAddr(proc->pml4Phys, va);
                    if (physAddr != 0) {
                        Memory::g_pfa->Free((void*)Memory::HHDM(physAddr));
                        Memory::VMM::Paging::UnmapUserIn(proc->pml4Phys, va);
                    }
                }
            }

            g_libTable[slot][libSlot].inUse = false;
            g_libTable[slot][libSlot].baseAddr = 0;
            g_libTable[slot][libSlot].path[0] = '\0';
        }

        return 0;
    }

    // Resolve a symbol in a loaded library.
    // handle = library handle from LoadLib
    // symbolOffset = offset of the symbol from the library's base address
    // Returns the virtual address of the symbol, or 0 if not found.
    static uint64_t Sys_DLSym(uint64_t handle, uint64_t symbolOffset) {
        int slot = GetCurrentSlot();
        if (slot < 0) return 0;

        int libSlot = (int)handle - 1;
        if (libSlot < 0 || libSlot >= MaxLibsPerProcess) return 0;

        if (!g_libTable[slot][libSlot].inUse) return 0;

        uint64_t libBase = g_libTable[slot][libSlot].baseAddr;
        return libBase + symbolOffset;
    }

    // Get library base address (for userspace symbol resolution)
    static uint64_t Sys_GetLibBase(uint64_t handle) {
        int slot = GetCurrentSlot();
        if (slot < 0) return 0;

        int libSlot = (int)handle - 1;
        if (libSlot < 0 || libSlot >= MaxLibsPerProcess) return 0;

        if (!g_libTable[slot][libSlot].inUse) return 0;

        return g_libTable[slot][libSlot].baseAddr;
    }

    // Cleanup library table for a process slot
    static void CleanupLibTable(int slot) {
        if (slot < 0 || slot >= MaxProcesses) return;

        auto* proc = Sched::GetProcessSlot(slot);
        if (proc == nullptr) return;

        // Unmap all libraries for this process
        for (int i = 0; i < MaxLibsPerProcess; i++) {
            if (g_libTable[slot][i].inUse) {
                uint64_t libBase = g_libTable[slot][i].baseAddr;
                uint64_t libEnd = libBase + Sched::LIB_MAX_SIZE;

                for (uint64_t va = libBase; va < libEnd; va += 0x1000) {
                    uint64_t physAddr = Memory::VMM::Paging::GetPhysAddr(proc->pml4Phys, va);
                    if (physAddr != 0) {
                        Memory::g_pfa->Free((void*)Memory::HHDM(physAddr));
                        Memory::VMM::Paging::UnmapUserIn(proc->pml4Phys, va);
                    }
                }

                g_libTable[slot][i].inUse = false;
                g_libTable[slot][i].refcount = 0;
                g_libTable[slot][i].baseAddr = 0;
            }
        }
    }

}
