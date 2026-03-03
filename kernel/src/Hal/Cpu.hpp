/*
    * Cpu.hpp
    * CPU feature enablement helpers
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Hal {

    // Enable SSE/SSE2 — required for userspace programs compiled with SSE.
    // CR0: clear EM (bit 2), set MP (bit 1)
    // CR4: set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
    inline void EnableSSE() {
        uint64_t cr0;
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2);  // Clear EM
        cr0 |=  (1ULL << 1);  // Set MP
        asm volatile("mov %0, %%cr0" :: "r"(cr0));

        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);   // OSFXSR
        cr4 |= (1ULL << 10);  // OSXMMEXCPT
        asm volatile("mov %0, %%cr4" :: "r"(cr4));
    }

}
