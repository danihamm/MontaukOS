/*
    * libc.c
    * Minimal C standard library for MontaukOS userspace programs
    * Based on the proven libc from the DOOM port.
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

/* ========================================================================
   Raw syscall wrappers (C versions matching kernel ABI)
   ======================================================================== */

static inline long _zos_syscall0(long nr) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long _zos_syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile(
        "mov %[a1], %%rdi\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), [a1] "r"(a1)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long _zos_syscall4(long nr, long a1, long a2, long a3, long a4) {
    long ret;
    __asm__ volatile(
        "mov %[a1], %%rdi\n\t"
        "mov %[a2], %%rsi\n\t"
        "mov %[a3], %%rdx\n\t"
        "mov %[a4], %%r10\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), [a1] "r"(a1), [a2] "r"(a2), [a3] "r"(a3), [a4] "r"(a4)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

/* Syscall numbers */
#define SYS_EXIT    0
#define SYS_PRINT   4
#define SYS_PUTCHAR 5
#define SYS_ALLOC   11
#define SYS_FREE    12

/* ========================================================================
   errno
   ======================================================================== */

int errno = 0;

/* ========================================================================
   string.h functions
   ======================================================================== */

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    /* Byte copy until 8-byte aligned */
    while (n && ((uint64_t)d & 7)) { *d++ = *s++; n--; }

    /* Bulk 8-byte copy */
    uint64_t *d8 = (uint64_t *)d;
    const uint64_t *s8 = (const uint64_t *)s;
    size_t words = n / 8;
    for (size_t i = 0; i < words; i++) d8[i] = s8[i];

    /* Remainder */
    d = (unsigned char *)(d8 + words);
    s = (const unsigned char *)(s8 + words);
    for (size_t i = 0; i < (n & 7); i++) d[i] = s[i];

    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    unsigned char v = (unsigned char)c;

    /* Byte fill until 8-byte aligned */
    while (n && ((uint64_t)p & 7)) { *p++ = v; n--; }

    /* Bulk 8-byte fill */
    uint64_t v8 = v;
    v8 |= v8 << 8;  v8 |= v8 << 16;  v8 |= v8 << 32;
    uint64_t *p8 = (uint64_t *)p;
    size_t words = n / 8;
    for (size_t i = 0; i < words; i++) p8[i] = v8;

    /* Remainder */
    p = (unsigned char *)(p8 + words);
    for (size_t i = 0; i < (n & 7); i++) p[i] = v;

    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s || d >= s + n) {
        memcpy(dest, src, n);
    } else {
        /* Backward copy — bulk 8 bytes at a time from end */
        d += n; s += n;
        while (n && ((uint64_t)d & 7)) { *--d = *--s; n--; }
        uint64_t *d8 = (uint64_t *)d;
        const uint64_t *s8 = (const uint64_t *)s;
        size_t words = n / 8;
        for (size_t i = 1; i <= words; i++) d8[-(long)i] = s8[-(long)i];
        d = (unsigned char *)(d8 - words);
        s = (const unsigned char *)(s8 - words);
        for (size_t i = 1; i <= (n & 7); i++) d[-(long)i] = s[-(long)i];
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == uc)
            return (void *)(p + i);
    }
    return (void *)0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0)
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char *)s;
    return (char *)last;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && ((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) ==
                 ((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b)) { a++; b++; }
    int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    return ca - cb;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i];
        if (ca != cb || ca == 0) return ca - cb;
    }
    return 0;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

/* Forward declaration for strdup */
void *malloc(size_t size);

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* ========================================================================
   ctype.h functions
   ======================================================================== */

int isalpha(int c)  { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isprint(int c)  { return c >= 0x20 && c <= 0x7E; }
int ispunct(int c)  { return isprint(c) && !isalnum(c) && c != ' '; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7F; }
int isgraph(int c)  { return c > 0x20 && c <= 0x7E; }
int toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ========================================================================
   Heap allocator (free-list, backed by SYS_ALLOC)
   ======================================================================== */

#define HEAP_MAGIC  0x5A484541ULL  /* "ZHEA" */
#define FREED_MAGIC 0xDEADFEEEULL

struct HeapHeader {
    uint64_t magic;
    uint64_t size;
} __attribute__((packed));

struct FreeNode {
    uint64_t       size;
    struct FreeNode *next;
};

/* Segregated free lists: power-of-2 size classes for blocks <= 4096 bytes */
#define NUM_BUCKETS 8
static const uint64_t BUCKET_SIZES[NUM_BUCKETS] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

static struct FreeNode *g_buckets[NUM_BUCKETS] = {};
static struct FreeNode g_overflow = { 0, NULL };
static int g_heapInit = 0;

static int heap_bucket_index(uint64_t blockSize) {
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

/* Insert into overflow list (sorted by address, with coalescing) */
static void heap_insert_overflow(void *ptr, uint64_t size) {
    struct FreeNode *node = (struct FreeNode *)ptr;
    node->size = size;

    struct FreeNode *prev = &g_overflow;
    struct FreeNode *cur  = g_overflow.next;
    while (cur != NULL && cur < node) {
        prev = cur;
        cur  = cur->next;
    }

    int merged_prev = 0;
    if (prev != &g_overflow &&
        (uint8_t *)prev + prev->size == (uint8_t *)node) {
        prev->size += size;
        node = prev;
        merged_prev = 1;
    }

    if (cur != NULL &&
        (uint8_t *)node + node->size == (uint8_t *)cur) {
        node->size += cur->size;
        node->next  = cur->next;
        if (!merged_prev) prev->next = node;
    } else if (!merged_prev) {
        node->next = cur;
        prev->next = node;
    }
}

/* Take a block >= needed from overflow. Splits remainder back. */
static void *heap_take_overflow(uint64_t needed) {
    struct FreeNode *prev = &g_overflow;
    struct FreeNode *cur  = g_overflow.next;

    while (cur != NULL) {
        if (cur->size >= needed) {
            uint64_t blockSize = cur->size;
            prev->next = cur->next;

            if (blockSize > needed + sizeof(struct FreeNode) + 16) {
                heap_insert_overflow((uint8_t *)cur + needed, blockSize - needed);
            }
            return (void *)cur;
        }
        prev = cur;
        cur  = cur->next;
    }
    return NULL;
}

static void heap_grow(uint64_t bytes) {
    uint64_t pages = (bytes + 0xFFF) / 0x1000;
    if (pages < 4) pages = 4;
    void *mem = (void *)_zos_syscall1(SYS_ALLOC, (long)(pages * 0x1000));
    if (mem != NULL)
        heap_insert_overflow(mem, pages * 0x1000);
}

/* Refill a small-block bucket from overflow */
static int heap_refill_bucket(int idx) {
    uint64_t bsize = BUCKET_SIZES[idx];
    uint64_t chunk = (bsize < 4096) ? 4096 : bsize;

    void *block = heap_take_overflow(chunk);
    if (block == NULL) {
        heap_grow(chunk);
        block = heap_take_overflow(chunk);
        if (block == NULL) return 0;
    }

    uint64_t count = chunk / bsize;
    for (uint64_t i = 0; i < count; i++) {
        struct FreeNode *node = (struct FreeNode *)((uint8_t *)block + i * bsize);
        node->size = bsize;
        node->next = g_buckets[idx];
        g_buckets[idx] = node;
    }
    return 1;
}

void *malloc(size_t size) {
    if (!g_heapInit) {
        heap_grow(16 * 0x1000);
        g_heapInit = 1;
    }

    /* Guard against overflow: size + Header must not wrap */
    if (size > (uint64_t)-1 - sizeof(struct HeapHeader) - 15)
        return NULL;

    uint64_t needed = size + sizeof(struct HeapHeader);
    needed = (needed + 15) & ~15ULL;

    int idx = heap_bucket_index(needed);

    if (idx >= 0) {
        /* Small allocation — use segregated bucket (O(1)) */
        if (g_buckets[idx] == NULL && !heap_refill_bucket(idx))
            return NULL;

        struct FreeNode *node = g_buckets[idx];
        g_buckets[idx] = node->next;

        struct HeapHeader *hdr = (struct HeapHeader *)node;
        hdr->magic = HEAP_MAGIC;
        hdr->size  = size;
        return (void *)((uint8_t *)hdr + sizeof(struct HeapHeader));
    }

    /* Large allocation — search overflow list */
    void *block = heap_take_overflow(needed);
    if (block == NULL) {
        heap_grow(needed);
        block = heap_take_overflow(needed);
        if (block == NULL) return NULL;
    }

    struct HeapHeader *hdr = (struct HeapHeader *)block;
    hdr->magic = HEAP_MAGIC;
    hdr->size  = size;
    return (void *)((uint8_t *)hdr + sizeof(struct HeapHeader));
}

void free(void *ptr) {
    if (ptr == NULL) return;

    struct HeapHeader *hdr = (struct HeapHeader *)((uint8_t *)ptr - sizeof(struct HeapHeader));

    if (hdr->magic == FREED_MAGIC) return;    /* double-free */
    if (hdr->magic != HEAP_MAGIC) return;     /* corrupt */
    hdr->magic = FREED_MAGIC;

    uint64_t blockSize = hdr->size + sizeof(struct HeapHeader);
    blockSize = (blockSize + 15) & ~15ULL;

    int idx = heap_bucket_index(blockSize);

    if (idx >= 0) {
        /* Small block — push onto bucket (O(1)) */
        struct FreeNode *node = (struct FreeNode *)hdr;
        node->size = BUCKET_SIZES[idx];
        node->next = g_buckets[idx];
        g_buckets[idx] = node;
    } else {
        /* Large block — sorted insert with coalescing */
        heap_insert_overflow((void *)hdr, blockSize);
    }
}

void *calloc(size_t nmemb, size_t size) {
    /* Check for multiplication overflow */
    if (nmemb != 0 && size > (size_t)-1 / nmemb)
        return NULL;
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    struct HeapHeader *hdr = (struct HeapHeader *)((uint8_t *)ptr - sizeof(struct HeapHeader));
    uint64_t old = hdr->size;

    /* Compute actual block size (accounting for bucket rounding) */
    uint64_t oldBlock = (old + sizeof(struct HeapHeader) + 15) & ~15ULL;
    int idx = heap_bucket_index(oldBlock);
    if (idx >= 0) oldBlock = BUCKET_SIZES[idx];

    uint64_t newNeed = (size + sizeof(struct HeapHeader) + 15) & ~15ULL;
    if (newNeed <= oldBlock) {
        hdr->size = size;
        return ptr;
    }

    void *newp = malloc(size);
    if (newp == NULL) return NULL;

    size_t copySize = old < size ? old : size;
    memcpy(newp, ptr, copySize);
    free(ptr);
    return newp;
}

/* ========================================================================
   stdlib.h functions
   ======================================================================== */

int abs(int x)   { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

int atoi(const char *s) {
    int neg = 0, val = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (isdigit((unsigned char)*s)) {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long val = 0;
    int neg = 0;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long val = 0;

    while (isspace((unsigned char)*s)) s++;
    /* strtoul allows an optional leading + or - (result is negated for -) */
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return neg ? (unsigned long)(-(long)val) : val;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

void exit(int status) {
    _zos_syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

void abort(void) {
    _zos_syscall1(SYS_PRINT, (long)"abort() called\n");
    _zos_syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}

int system(const char *command) {
    (void)command;
    return -1;
}

/* ========================================================================
   printf family — vsnprintf core
   ======================================================================== */

struct _pf_state {
    char *buf;
    size_t pos;
    size_t max;
};

static void _pf_putc(struct _pf_state *st, char c) {
    if (st->pos < st->max)
        st->buf[st->pos] = c;
    st->pos++;
}

static void _pf_puts(struct _pf_state *st, const char *s) {
    while (*s) _pf_putc(st, *s++);
}

static void _pf_putnum(struct _pf_state *st, unsigned long val, int base, int upper, int width, char pad, int neg, int precision) {
    char tmp[24];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % base];
            val /= base;
        }
    }

    int digitCount = i;
    int precPad = 0;
    if (precision > digitCount)
        precPad = precision - digitCount;

    int total = (neg ? 1 : 0) + precPad + digitCount;

    if (neg && pad == '0' && precision < 0) {
        _pf_putc(st, '-');
    }
    if (precision < 0 && pad == '0') {
        for (int w = total; w < width; w++)
            _pf_putc(st, '0');
    } else {
        for (int w = total; w < width; w++)
            _pf_putc(st, ' ');
    }
    if (neg && !(pad == '0' && precision < 0)) {
        _pf_putc(st, '-');
    }

    for (int p = 0; p < precPad; p++)
        _pf_putc(st, '0');

    while (i > 0)
        _pf_putc(st, tmp[--i]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct _pf_state st;
    st.buf = buf;
    st.pos = 0;
    st.max = size > 0 ? size - 1 : 0;

    while (*fmt) {
        if (*fmt != '%') {
            _pf_putc(&st, *fmt++);
            continue;
        }
        fmt++;

        char pad = ' ';
        int left_align = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '0') pad = '0';
            if (*fmt == '-') { left_align = 1; pad = ' '; }
            fmt++;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            long val;
            if (is_long >= 1) val = va_arg(ap, long);
            else val = va_arg(ap, int);
            int neg = 0;
            unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); }
            else uval = (unsigned long)val;
            if (left_align) {
                size_t before = st.pos;
                _pf_putnum(&st, uval, 10, 0, 0, pad, neg, precision);
                size_t len = st.pos - before;
                for (size_t w = len; (int)w < width; w++) _pf_putc(&st, ' ');
            } else {
                _pf_putnum(&st, uval, 10, 0, width, pad, neg, precision);
            }
            break;
        }
        case 'u': {
            unsigned long val;
            if (is_long >= 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            if (left_align) {
                size_t before = st.pos;
                _pf_putnum(&st, val, 10, 0, 0, ' ', 0, precision);
                size_t len = st.pos - before;
                for (size_t w = len; (int)w < width; w++) _pf_putc(&st, ' ');
            } else {
                _pf_putnum(&st, val, 10, 0, width, pad, 0, precision);
            }
            break;
        }
        case 'x': case 'X': {
            unsigned long val;
            if (is_long >= 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            int upper = (*fmt == 'X');
            if (left_align) {
                size_t before = st.pos;
                _pf_putnum(&st, val, 16, upper, 0, pad, 0, precision);
                size_t len = st.pos - before;
                for (size_t w = len; (int)w < width; w++) _pf_putc(&st, ' ');
            } else {
                _pf_putnum(&st, val, 16, upper, width, pad, 0, precision);
            }
            break;
        }
        case 'p': {
            void *val = va_arg(ap, void *);
            _pf_puts(&st, "0x");
            _pf_putnum(&st, (unsigned long)val, 16, 0, 0, '0', 0, -1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) s = "(null)";
            int slen = (int)strlen(s);
            if (precision >= 0 && precision < slen) slen = precision;
            if (!left_align) {
                for (int w = slen; w < width; w++) _pf_putc(&st, ' ');
            }
            for (int i = 0; i < slen; i++) _pf_putc(&st, s[i]);
            if (left_align) {
                for (int w = slen; w < width; w++) _pf_putc(&st, ' ');
            }
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            _pf_putc(&st, c);
            break;
        }
        case '%':
            _pf_putc(&st, '%');
            break;
        default:
            _pf_putc(&st, '%');
            _pf_putc(&st, *fmt);
            break;
        }
        if (*fmt) fmt++;
    }

    if (size > 0) {
        if (st.pos < size)
            st.buf[st.pos] = '\0';
        else
            st.buf[size - 1] = '\0';
    }

    return (int)st.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}

static char _printbuf[4096];

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(_printbuf, sizeof(_printbuf), fmt, ap);
    va_end(ap);
    _zos_syscall1(SYS_PRINT, (long)_printbuf);
    return ret;
}

int puts(const char *s) {
    _zos_syscall1(SYS_PRINT, (long)s);
    _zos_syscall1(SYS_PUTCHAR, (long)'\n');
    return 0;
}

int putchar(int c) {
    _zos_syscall1(SYS_PUTCHAR, (long)c);
    return c;
}

/* ========================================================================
   assert.h support
   ======================================================================== */

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    _zos_syscall1(SYS_PRINT, (long)"Assertion failed: ");
    _zos_syscall1(SYS_PRINT, (long)expr);
    _zos_syscall1(SYS_PRINT, (long)" at ");
    _zos_syscall1(SYS_PRINT, (long)file);
    _zos_syscall1(SYS_PRINT, (long)"\n");
    (void)line; (void)func;
    abort();
}
