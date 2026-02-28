/*
    * PageFrameAllocator.hpp
    * Memory allocator for physical pages
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include "Memmap.hpp"
#include <Api/Syscall.hpp>

#include <CppLib/Spinlock.hpp>

namespace Memory {
    class PageFrameAllocator {
        struct Page {
            std::size_t size;
            Page* next;
        };

        Page head{};
        kcp::Spinlock Lock{};
        LargestSection g_section;
public:
        PageFrameAllocator(LargestSection section);

        void* Allocate();
        void* AllocateZeroed();
        void* ReallocConsecutive(void* ptr, int n);
        void Free(void* ptr);
        void Free(void* ptr, int n);

        void GetStats(Montauk::MemStats* out);
    };

    extern PageFrameAllocator* g_pfa;
};