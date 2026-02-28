/*
    * heap.h
    * Userspace heap allocator for MontaukOS programs
    * Free-list allocator backed by SYS_ALLOC page requests.
    * Adapted from the kernel HeapAllocator.
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <montauk/syscall.h>

namespace montauk {
namespace heap_detail {

    static constexpr uint64_t HEADER_MAGIC = 0x5A484541;  // "ZHEA"

    struct Header {
        uint64_t magic;
        uint64_t size;     // user-requested size
    } __attribute__((packed));

    struct FreeNode {
        uint64_t  size;    // total size of this free block (including node)
        FreeNode* next;
    };

    // Per-process heap state (single TU per program, so static is fine)
    static FreeNode  g_head{0, nullptr};
    static bool      g_initialized = false;

    static inline Header* get_header(void* block) {
        return (Header*)((uint8_t*)block - sizeof(Header));
    }

    static inline void insert_free(void* ptr, uint64_t size) {
        auto* node  = (FreeNode*)ptr;
        node->size  = size;
        node->next  = g_head.next;
        g_head.next = node;
    }

    static inline bool grow(uint64_t bytes) {
        uint64_t pages = (bytes + 0xFFF) / 0x1000;
        if (pages < 4) pages = 4;          // grow at least 16 KiB at a time

        void* mem = montauk::alloc(pages * 0x1000);
        if (mem == nullptr) return false;
        insert_free(mem, pages * 0x1000);
        return true;
    }

} // namespace heap_detail

    // ---- Public API ----

    inline void* malloc(uint64_t size) {
        using namespace heap_detail;

        if (!g_initialized) {
            grow(16 * 0x1000);             // seed with 64 KiB
            g_initialized = true;
        }

        uint64_t needed = size + sizeof(Header);
        needed = (needed + 15) & ~15ULL;   // 16-byte alignment

        FreeNode* prev    = &g_head;
        FreeNode* current = g_head.next;

        while (current != nullptr) {
            if (current->size >= needed) {
                uint64_t blockSize = current->size;

                // Unlink
                prev->next = current->next;

                // Split if worthwhile
                if (blockSize > needed + sizeof(FreeNode) + 16) {
                    void*    rest     = (void*)((uint8_t*)current + needed);
                    uint64_t restSize = blockSize - needed;
                    insert_free(rest, restSize);
                }

                // Write allocation header
                Header* header = (Header*)current;
                header->magic  = HEADER_MAGIC;
                header->size   = size;

                return (void*)((uint8_t*)header + sizeof(Header));
            }

            prev    = current;
            current = current->next;
        }

        // No fit — grow and retry
        if (!grow(needed))
            return nullptr;
        return malloc(size);
    }

    inline void mfree(void* ptr) {
        using namespace heap_detail;

        if (ptr == nullptr) return;

        Header* header = get_header(ptr);

        uint64_t blockSize = header->size + sizeof(Header);
        blockSize = (blockSize + 15) & ~15ULL;

        insert_free((void*)header, blockSize);
    }

    inline void* realloc(void* ptr, uint64_t size) {
        if (ptr == nullptr) return malloc(size);

        auto* header  = heap_detail::get_header(ptr);
        uint64_t  old = header->size;

        void* newBlock = malloc(size);
        if (newBlock == nullptr) return nullptr;

        // Copy the smaller of old/new sizes
        uint64_t copySize = (old < size) ? old : size;
        uint8_t* dst = (uint8_t*)newBlock;
        uint8_t* src = (uint8_t*)ptr;
        for (uint64_t i = 0; i < copySize; i++)
            dst[i] = src[i];

        mfree(ptr);
        return newBlock;
    }

} // namespace montauk
