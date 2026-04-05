#pragma once
#include <cstdint>
#include <cstddef>

namespace Lib {
    inline int strlen(const char *string) {
        int c = 0;

        while (*string != '\0') {
            string++;
            c++;
        }

        return c;
    }

    inline int strcmp(const char *a, const char *b) {
        while (*a && *b) {
            if (*a != *b) {
                return (unsigned char)*a - (unsigned char)*b;
            }
            a++;
            b++;
        }
        return (unsigned char)*a - (unsigned char)*b;
    }

    inline int strncmp(const char *a, const char *b, size_t max_len) {
        for (size_t i = 0; i < max_len; i++) {
            if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
            if (a[i] == '\0') return 0;
        }
        return 0;
    }

    inline char *strncpy(char *dest, const char *src, size_t max_len) {
        size_t i = 0;
        while (i < max_len - 1 && src[i]) {
            dest[i] = src[i];
            i++;
        }
        dest[i] = '\0';
        return dest;
    }

    char *int2basestr(int num, size_t radix);
    char *u64_2_basestr(uint64_t num, size_t radix);
    char *uint2basestr(uint32_t num, size_t radix);
}