/*
    * string.h
    * Common string and memory utility functions for ZenithOS programs
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace zenith {

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
        for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    }

    inline void memmove(void* dst, const void* src, uint64_t n) {
        auto* d = (uint8_t*)dst;
        auto* s = (const uint8_t*)src;
        if (d < s) {
            for (uint64_t i = 0; i < n; i++) d[i] = s[i];
        } else {
            for (uint64_t i = n; i > 0; i--) d[i-1] = s[i-1];
        }
    }

    inline void memset(void* dst, int val, uint64_t n) {
        auto* d = (uint8_t*)dst;
        for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    }

    inline void strcpy(char* dst, const char* src) {
        while (*src) *dst++ = *src++;
        *dst = '\0';
    }

    inline void strncpy(char* dst, const char* src, int max) {
        int i = 0;
        while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
        dst[i] = '\0';
    }

}
