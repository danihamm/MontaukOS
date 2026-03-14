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
        if (page == nullptr) return nullptr;
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

    // Core free implementation: sorted-insert with coalescing.
    // The free list is kept sorted by address so adjacent blocks can be merged,
    // preventing fragmentation from accumulating over time.
    void PageFrameAllocator::FreeRange(void* ptr, size_t size) {
        if (ptr == nullptr || size == 0) return;

        Lock.Acquire();

        uint64_t addr = (uint64_t)ptr;

        // Walk to find the sorted insertion point: prev < addr < current
        Page* prev = &head;
        Page* current = head.next;

        while (current != nullptr && (uint64_t)current < addr) {
            // Double-free check: addr falls within an existing free block
            if (addr < (uint64_t)current + current->size) {
                Kt::KernelLogStream(Kt::WARNING, "PFA")
                    << "Double-free detected at " << addr << ", ignoring";
                Lock.Release();
                return;
            }
            prev = current;
            current = current->next;
        }

        // Double-free check: exact match with next block
        if (current != nullptr && (uint64_t)current == addr) {
            Kt::KernelLogStream(Kt::WARNING, "PFA")
                << "Double-free detected at " << addr << ", ignoring";
            Lock.Release();
            return;
        }

        // Try to coalesce with previous block (if prev ends where new block starts)
        bool merged_prev = false;
        if (prev != &head) {
            uint64_t prev_end = (uint64_t)prev + prev->size;
            if (prev_end == addr) {
                prev->size += size;
                merged_prev = true;
            }
        }

        // Try to coalesce with next block (if new block ends where next starts)
        if (current != nullptr && addr + size == (uint64_t)current) {
            if (merged_prev) {
                // Three-way merge: prev absorbs new block and current
                prev->size += current->size;
                prev->next = current->next;
            } else {
                // Forward merge: new block absorbs current
                Page* new_page = (Page*)addr;
                new_page->size = size + current->size;
                new_page->next = current->next;
                prev->next = new_page;
            }
        } else if (!merged_prev) {
            // No merging possible: insert new block between prev and current
            Page* new_page = (Page*)addr;
            new_page->size = size;
            new_page->next = current;
            prev->next = new_page;
        }

        Lock.Release();
    }

    void PageFrameAllocator::Free(void* ptr) {
        FreeRange(ptr, 0x1000);
    }

    void PageFrameAllocator::Free(void* ptr, int n) {
        if (ptr == nullptr || n <= 0) return;
        // Free the entire contiguous range as a single block.
        // This is more efficient than freeing one page at a time and
        // guarantees the range stays coalesced.
        FreeRange(ptr, (size_t)n * 0x1000);
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
