/*
    * PageFrameAllocator.hpp
    * Memory allocator for physical pages
    * Copyright (c) 2025 Daniel Hammer
*/

#include "PageFrameAllocator.hpp"
#include "HHDM.hpp"
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Spinlock.hpp>
#include <Common/Panic.hpp>

namespace Memory {
    PageFrameAllocator::PageFrameAllocator(LargestSection section) {
        /* we need the virtual address rather than the physical address, so we call the helper */
        g_section = LargestSection{
            .address = HHDM(section.address),
            .size = section.size
        };

        head.next = (Page*)g_section.address;
        head.next->size = section.size;
        head.next->next = nullptr;

        Kt::KernelLogStream(Kt::DEBUG, "PageFrameAllocator") << "New pool size: " << section.size;
    }

    void* PageFrameAllocator::Allocate() {
        Lock.Acquire();

        Page* current = head.next;
        Page* prev = &head;

        while (current != nullptr) {
            if (current->size >= 0x1000) {
                uint64_t current_addr = (uint64_t)current;
                uint64_t current_end_addr = current_addr + current->size;
                uint64_t new_size = current->size - 0x1000;

                if (new_size == 0) {
                    prev->next = current->next;

                    Lock.Release();
                    return (void*)current;
                }
                else
                {
                    current->size -= 0x1000;

                    Lock.Release();
                    return (void*)current_end_addr - 0x1000;
                }
            }

            prev = current;
            current = current->next;
        }

        Lock.Release();
        return nullptr;
    }

    void* PageFrameAllocator::AllocateZeroed() {
        auto page = Allocate();
        memset(page, 0, 0x1000);

        return page;
    }

    void* PageFrameAllocator::ReallocConsecutive(void* ptr, int n) {
        Lock.Acquire();

        // Search the free list for a single contiguous region >= n pages.
        // The old implementation assumed N consecutive Allocate() calls gave
        // adjacent pages, which breaks when individual pages are freed back.
        size_t needed = (size_t)n * 0x1000;
        Page* current = head.next;
        Page* prev = &head;

        while (current != nullptr) {
            if (current->size >= needed) {
                // Carve from the top of this contiguous region
                current->size -= needed;
                void* base;
                if (current->size == 0) {
                    base = (void*)current;
                    prev->next = current->next;
                } else {
                    base = (void*)((uint64_t)current + current->size);
                }

                Lock.Release();

                if (ptr != nullptr) {
                    memcpy(base, ptr, 0x1000);  // copy one page (ptr is always a single page)
                    Free(ptr);
                }

                return base;
            }
            prev = current;
            current = current->next;
        }

        Lock.Release();
        Panic("PageFrameAllocator: no contiguous region available", nullptr);
        return nullptr;
    }

    void PageFrameAllocator::Free(void* ptr) {
        Lock.Acquire();
        auto prev_next = head.next;
        head.next = (Page*)ptr;

        head.next->next = prev_next;
        head.next->size = 0x1000;
        Lock.Release();
    }

    void PageFrameAllocator::Free(void* ptr, int n) {
        for (int i = 0; i < n; i++) {
            Free((void*)((uint64_t)ptr + 0x1000 * i));
        }
    }

    void PageFrameAllocator::GetStats(Montauk::MemStats* out) {
        if (!out) return;
        Lock.Acquire();
        uint64_t freeBytes = 0;
        Page* current = head.next;
        while (current != nullptr) {
            freeBytes += current->size;
            current = current->next;
        }
        Lock.Release();

        out->totalBytes = g_section.size;
        out->freeBytes = freeBytes;
        out->usedBytes = g_section.size > freeBytes ? g_section.size - freeBytes : 0;
        out->pageSize = 0x1000;
    }
};