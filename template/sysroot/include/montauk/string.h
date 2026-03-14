/*
    * string.h
    * Common string and memory utility functions for MontaukOS programs
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace montauk {

    inline int slen(const char* s) {
        int n = 0;
        while (s[n]) n++;
        return n;
    }

    inline bool streq(const char* a, const char* b) {
        while (*a && *b) {
            if (*a != *b) return false;
            a++; b++;
        }
        return *a == *b;
    }

    inline bool starts_with(const char* str, const char* prefix) {
        while (*prefix) {
            if (*str != *prefix) return false;
            str++; prefix++;
        }
        return true;
    }

    inline const char* skip_spaces(const char* s) {
        while (*s == ' ') s++;
        return s;
    }

    inline void memcpy(void* dst, const void* src, uint64_t n) {
        auto* d = (uint8_t*)dst;
        auto* s = (const uint8_t*)src;

        // Byte copy until 8-byte aligned
        while (n && ((uint64_t)d & 7)) { *d++ = *s++; n--; }

        // Bulk 8-byte copy
        auto* d8 = (uint64_t*)d;
        auto* s8 = (const uint64_t*)s;
        uint64_t words = n / 8;
        for (uint64_t i = 0; i < words; i++) d8[i] = s8[i];

        // Remainder
        d = (uint8_t*)(d8 + words);
        s = (const uint8_t*)(s8 + words);
        for (uint64_t i = 0; i < (n & 7); i++) d[i] = s[i];
    }

    inline void memmove(void* dst, const void* src, uint64_t n) {
        auto* d = (uint8_t*)dst;
        auto* s = (const uint8_t*)src;
        if (d < s || d >= s + n) {
            memcpy(dst, src, n);
        } else {
            // Backward copy — bulk 8 bytes at a time from end
            d += n; s += n;
            while (n && ((uint64_t)d & 7)) { *--d = *--s; n--; }
            auto* d8 = (uint64_t*)d;
            auto* s8 = (const uint64_t*)s;
            uint64_t words = n / 8;
            for (uint64_t i = 1; i <= words; i++) d8[-i] = s8[-i];
            d = (uint8_t*)(d8 - words);
            s = (const uint8_t*)(s8 - words);
            for (uint64_t i = 1; i <= (n & 7); i++) d[-i] = s[-i];
        }
    }

    inline void memset(void* dst, int val, uint64_t n) {
        auto* d = (uint8_t*)dst;
        uint8_t v = (uint8_t)val;

        // Byte fill until 8-byte aligned
        while (n && ((uint64_t)d & 7)) { *d++ = v; n--; }

        // Bulk 8-byte fill
        uint64_t v8 = v;
        v8 |= v8 << 8;  v8 |= v8 << 16;  v8 |= v8 << 32;
        auto* d8 = (uint64_t*)d;
        uint64_t words = n / 8;
        for (uint64_t i = 0; i < words; i++) d8[i] = v8;

        // Remainder
        d = (uint8_t*)(d8 + words);
        for (uint64_t i = 0; i < (n & 7); i++) d[i] = v;
    }

    inline void strcpy(char* dst, const char* src) {
        while (*src) *dst++ = *src++;
        *dst = '\0';
    }

    inline void strncpy(char* dst, const char* src, int max) {
        if (max <= 0) return;
        int i = 0;
        while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
        dst[i] = '\0';
    }

}
