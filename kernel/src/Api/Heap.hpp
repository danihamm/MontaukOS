/*
    * Heap.hpp
    * SYS_ALLOC, SYS_FREE syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#include <cstdint>
#include <Sched/Scheduler.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/PageFrameAllocator.hpp>

namespace Montauk {

    // Per-process heap allocation tracking (separate from Process struct to avoid bloating it)
    struct HeapAlloc {
        uint64_t va;
        uint64_t numPages;
    };

    static constexpr int MaxHeapAllocs = 128;

    static HeapAlloc g_heapAllocs[Sched::MaxProcesses][MaxHeapAllocs];
    static int g_heapAllocCount[Sched::MaxProcesses];

    // Get the process table slot index for the current process
    static int GetCurrentSlot() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return -1;
        // Process slot index = pointer offset from slot 0
        auto* slot0 = Sched::GetProcessSlot(0);
        return (int)(proc - slot0);
    }

    static uint64_t Sys_Alloc(uint64_t size) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;

        // Round up to page boundary
        size = (size + 0xFFF) & ~0xFFFULL;
        if (size == 0) size = 0x1000;

        uint64_t userVa = proc->heapNext;
        uint64_t numPages = size / 0x1000;

        for (uint64_t i = 0; i < numPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) return 0;
            uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
            Memory::VMM::Paging::MapUserIn(proc->pml4Phys, physAddr, userVa + i * 0x1000);
        }

        proc->heapNext += size;

        // Track the allocation so Sys_Free can release it
        int slot = GetCurrentSlot();
        if (slot >= 0 && g_heapAllocCount[slot] < MaxHeapAllocs) {
            g_heapAllocs[slot][g_heapAllocCount[slot]++] = { userVa, numPages };
        }

        return userVa;
    }

    // Reset heap allocation tracking for a process slot.
    // The actual physical pages are freed by Paging::FreeUserHalf() during process cleanup.
    static void CleanupHeapForSlot(int slot, uint64_t /*pml4Phys*/) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return;
        g_heapAllocCount[slot] = 0;
    }

    static void Sys_Free(uint64_t addr) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return;

        int slot = GetCurrentSlot();
        if (slot < 0) return;

        // Find the allocation record matching this address
        int idx = -1;
        for (int i = 0; i < g_heapAllocCount[slot]; i++) {
            if (g_heapAllocs[slot][i].va == addr) {
                idx = i;
                break;
            }
        }
        if (idx < 0) return;  // Unknown address — ignore

        uint64_t va = g_heapAllocs[slot][idx].va;
        uint64_t numPages = g_heapAllocs[slot][idx].numPages;

        // Free each physical page and unmap the virtual address
        for (uint64_t i = 0; i < numPages; i++) {
            uint64_t pageVa = va + i * 0x1000;
            uint64_t physAddr = Memory::VMM::Paging::GetPhysAddr(proc->pml4Phys, pageVa);
            if (physAddr != 0) {
                Memory::g_pfa->Free((void*)Memory::HHDM(physAddr));
            }
            Memory::VMM::Paging::UnmapUserIn(proc->pml4Phys, pageVa);
        }

        // Remove tracking entry by swapping with the last element
        g_heapAllocs[slot][idx] = g_heapAllocs[slot][g_heapAllocCount[slot] - 1];
        g_heapAllocCount[slot]--;
    }
};
