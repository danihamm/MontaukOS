/*
    * heap.h
    * Userspace heap allocator for MontaukOS programs
   * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <montauk/syscall.h>
#include <montauk/string.h>

namespace montauk {
namespace heap_detail {

    static constexpr uint64_t HEADER_MAGIC = 0x5A484541;  // "ZHEA"
    static constexpr uint64_t FREED_MAGIC  = 0xDEADFEEE;

    struct Header {
        uint64_t magic;
        uint64_t size;     // user-requested size
    } __attribute__((packed));

    struct FreeNode {
        uint64_t  size;    // total size of this free block (including node)
        FreeNode* next;
    };

    // Segregated free lists: power-of-2 size classes for blocks <= 4096 bytes.
    // Blocks larger than 4096 go to the overflow list.
    static constexpr int NUM_BUCKETS = 8;
    static constexpr uint64_t BUCKET_SIZES[NUM_BUCKETS] = {
        32, 64, 128, 256, 512, 1024, 2048, 4096
    };

    // Per-process heap state — must be `inline` (not `static`) so that all
    // translation units in a multi-TU program share a single heap.
    inline FreeNode* g_buckets[NUM_BUCKETS] = {};
    inline FreeNode  g_overflow{0, nullptr};
    inline bool      g_initialized = false;

    static inline Header* get_header(void* block) {
        return (Header*)((uint8_t*)block - sizeof(Header));
    }

    // Determine which bucket a block size belongs to, or -1 for overflow
    static inline int bucket_index(uint64_t blockSize) {
        if (blockSize <= 32)   return 0;
        if (blockSize <= 64)   return 1;
        if (blockSize <= 128)  return 2;
        if (blockSize <= 256)  return 3;
        if (blockSize <= 512)  return 4;
        if (blockSize <= 1024) return 5;
        if (blockSize <= 2048) return 6;
        if (blockSize <= 4096) return 7;
        return -1;
    }

    // Insert into overflow list (sorted by address, with adjacent-block coalescing)
    static inline void insert_overflow(void* ptr, uint64_t size) {
        auto* node = (FreeNode*)ptr;
        node->size = size;

        FreeNode* prev = &g_overflow;
        FreeNode* cur  = g_overflow.next;
        while (cur != nullptr && cur < node) {
            prev = cur;
            cur  = cur->next;
        }

        bool merged_prev = false;
        if (prev != &g_overflow &&
            (uint8_t*)prev + prev->size == (uint8_t*)node) {
            prev->size += size;
            node = prev;
            merged_prev = true;
        }

        if (cur != nullptr &&
            (uint8_t*)node + node->size == (uint8_t*)cur) {
            node->size += cur->size;
            node->next  = cur->next;
            if (!merged_prev) prev->next = node;
        } else if (!merged_prev) {
            node->next = cur;
            prev->next = node;
        }
    }

    // Take a block of at least `needed` bytes from the overflow list.
    // Splits remainder back into overflow if worthwhile.
    static inline void* take_from_overflow(uint64_t needed) {
        FreeNode* prev = &g_overflow;
        FreeNode* cur  = g_overflow.next;

        while (cur != nullptr) {
            if (cur->size >= needed) {
                uint64_t blockSize = cur->size;
                prev->next = cur->next;

                if (blockSize > needed + sizeof(FreeNode) + 16) {
                    insert_overflow((uint8_t*)cur + needed, blockSize - needed);
                }
                return (void*)cur;
            }
            prev = cur;
            cur  = cur->next;
        }
        return nullptr;
    }

    static inline bool grow(uint64_t bytes) {
        uint64_t pages = (bytes + 0xFFF) / 0x1000;
        if (pages < 4) pages = 4;

        void* mem = montauk::alloc(pages * 0x1000);
        if (mem == nullptr) return false;
        insert_overflow(mem, pages * 0x1000);
        return true;
    }

    // Refill a small-block bucket by carving a page-sized chunk from overflow
    static inline bool refill_bucket(int idx) {
        uint64_t bsize = BUCKET_SIZES[idx];
        uint64_t chunk = (bsize < 4096) ? 4096 : bsize;

        void* block = take_from_overflow(chunk);
        if (block == nullptr) {
            if (!grow(chunk)) return false;
            block = take_from_overflow(chunk);
            if (block == nullptr) return false;
        }

        uint64_t count = chunk / bsize;
        for (uint64_t i = 0; i < count; i++) {
            auto* node = (FreeNode*)((uint8_t*)block + i * bsize);
            node->size = bsize;
            node->next = g_buckets[idx];
            g_buckets[idx] = node;
        }
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

        // Guard against overflow: size + Header must not wrap
        if (size > UINT64_MAX - sizeof(Header) - 15)
            return nullptr;

        uint64_t needed = size + sizeof(Header);
        needed = (needed + 15) & ~15ULL;

        int idx = bucket_index(needed);

        if (idx >= 0) {
            // Small allocation — use segregated bucket (O(1))
            if (g_buckets[idx] == nullptr && !refill_bucket(idx))
                return nullptr;

            FreeNode* node = g_buckets[idx];
            g_buckets[idx] = node->next;

            Header* header = (Header*)node;
            header->magic  = HEADER_MAGIC;
            header->size   = size;
            return (void*)((uint8_t*)header + sizeof(Header));
        }

        // Large allocation — search overflow list
        void* block = take_from_overflow(needed);
        if (block == nullptr) {
            if (!grow(needed)) return nullptr;
            block = take_from_overflow(needed);
            if (block == nullptr) return nullptr;
        }

        Header* header = (Header*)block;
        header->magic  = HEADER_MAGIC;
        header->size   = size;
        return (void*)((uint8_t*)header + sizeof(Header));
    }

    inline void mfree(void* ptr) {
        using namespace heap_detail;

        if (ptr == nullptr) return;

        Header* header = get_header(ptr);

        if (header->magic == FREED_MAGIC) return;    // double-free
        if (header->magic != HEADER_MAGIC) return;   // corrupt
        header->magic = FREED_MAGIC;

        uint64_t blockSize = header->size + sizeof(Header);
        blockSize = (blockSize + 15) & ~15ULL;

        int idx = bucket_index(blockSize);

        if (idx >= 0) {
            // Small block — push onto bucket (O(1))
            auto* node  = (FreeNode*)header;
            node->size  = BUCKET_SIZES[idx];
            node->next  = g_buckets[idx];
            g_buckets[idx] = node;
        } else {
            // Large block — sorted insert with coalescing
            insert_overflow((void*)header, blockSize);
        }
    }

    inline void* realloc(void* ptr, uint64_t size) {
        if (ptr == nullptr) return malloc(size);

        auto* header = heap_detail::get_header(ptr);
        uint64_t old = header->size;

        // Compute actual block size (accounting for bucket rounding)
        uint64_t oldBlock = (old + sizeof(heap_detail::Header) + 15) & ~15ULL;
        int idx = heap_detail::bucket_index(oldBlock);
        if (idx >= 0) oldBlock = heap_detail::BUCKET_SIZES[idx];

        uint64_t newNeed = (size + sizeof(heap_detail::Header) + 15) & ~15ULL;
        if (newNeed <= oldBlock) {
            header->size = size;
            return ptr;
        }

        void* newBlock = malloc(size);
        if (newBlock == nullptr) return nullptr;

        uint64_t copySize = (old < size) ? old : size;
        memcpy(newBlock, ptr, copySize);

        mfree(ptr);
        return newBlock;
    }

} // namespace montauk
