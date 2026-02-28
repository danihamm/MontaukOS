/*
    * Random.hpp
    * SYS_GETRANDOM syscall
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Montauk {

    // ---- Random number generation ----
    // Uses RDTSC mixed with xorshift64* PRNG for entropy.
    // RDRAND is intentionally avoided: some firmware disables the RDRAND
    // hardware unit while CPUID still advertises support (bit 30 of ECX),
    // causing #UD on real hardware. RDTSC-based entropy is sufficient for
    // seeding BearSSL's PRNG for TLS session keys.

    static int64_t Sys_GetRandom(uint8_t* buf, uint64_t len) {
        uint64_t tsc;
        asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) :: "rdx");
        uint64_t state = tsc;

        for (uint64_t i = 0; i < len; i += 8) {
            asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) :: "rdx");
            state ^= tsc;
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            uint64_t val = state * 0x2545F4914F6CDD1DULL;

            uint64_t remaining = len - i;
            uint64_t toCopy = remaining < 8 ? remaining : 8;
            for (uint64_t j = 0; j < toCopy; j++)
                buf[i + j] = (uint8_t)(val >> (j * 8));
        }
        return (int64_t)len;
    }
};
