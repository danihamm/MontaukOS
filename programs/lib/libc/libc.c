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
#include <dirent.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

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

static inline long _zos_syscall2(long nr, long a1, long a2) {
    long ret;
    __asm__ volatile(
        "mov %[a1], %%rdi\n\t"
        "mov %[a2], %%rsi\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), [a1] "r"(a1), [a2] "r"(a2)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long _zos_syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "mov %[a1], %%rdi\n\t"
        "mov %[a2], %%rsi\n\t"
        "mov %[a3], %%rdx\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), [a1] "r"(a1), [a2] "r"(a2), [a3] "r"(a3)
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
#define SYS_SLEEP_MS 2
#define SYS_PRINT   4
#define SYS_PUTCHAR 5
#define SYS_OPEN    6
#define SYS_READ    7
#define SYS_GETSIZE 8
#define SYS_CLOSE   9
#define SYS_READDIR 10
#define SYS_ALLOC   11
#define SYS_FREE    12
#define SYS_GETMILLISECONDS 14
#define SYS_GETCHAR 18
#define SYS_SPAWN   20
#define SYS_WAITPID 23
#define SYS_GETARGS 25
#define SYS_GETTIME 28
#define SYS_FWRITE  41
#define SYS_FCREATE 42
#define SYS_FDELETE 77
#define SYS_FMKDIR  78
#define SYS_GETTZ   91
#define SYS_FRENAME 94
#define SYS_GETCWD  95
#define SYS_CHDIR   96

/* ========================================================================
   errno
   ======================================================================== */

int errno = 0;

/* ========================================================================
   Internal helpers
   ======================================================================== */

struct _mtk_datetime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

struct _DIR {
    int count;
    int index;
    struct dirent entry;
    char names[256][NAME_MAX + 1];
};

struct _env_entry {
    char *name;
    char *value;
};

static struct _env_entry _env_entries[64];
static sighandler_t _signal_handlers[32];

static const char *_weekday_short[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *_weekday_long[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static const char *_month_short[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *_month_long[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int _is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int _days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 1 && _is_leap_year(year)) return 29;
    return days[month];
}

static int _day_of_year(int year, int month, int day) {
    int yday = 0;
    for (int i = 0; i < month; i++) {
        yday += _days_in_month(year, i);
    }
    return yday + day - 1;
}

static time_t _epoch_from_datetime(int year, int month, int day, int hour, int minute, int second) {
    time_t days = 0;

    for (int y = 1970; y < year; y++) {
        days += _is_leap_year(y) ? 366 : 365;
    }

    for (int m = 1; m < month; m++) {
        days += _days_in_month(year, m - 1);
    }

    days += day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static void _tm_from_epoch(time_t epoch, struct tm *out) {
    time_t days = epoch / 86400;
    time_t rem = epoch % 86400;
    int year = 1970;

    if (rem < 0) {
        rem += 86400;
        days--;
    }

    while (days < 0) {
        year--;
        days += _is_leap_year(year) ? 366 : 365;
    }

    while (1) {
        int diy = _is_leap_year(year) ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        year++;
    }

    int month = 0;
    while (month < 11) {
        int dim = _days_in_month(year, month);
        if (days < dim) break;
        days -= dim;
        month++;
    }

    out->tm_year = year - 1900;
    out->tm_mon = month;
    out->tm_mday = (int)days + 1;
    out->tm_hour = (int)(rem / 3600);
    rem %= 3600;
    out->tm_min = (int)(rem / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_yday = _day_of_year(year, month, out->tm_mday);
    out->tm_wday = (int)(((epoch / 86400) + 4) % 7);
    if (out->tm_wday < 0) out->tm_wday += 7;
    out->tm_isdst = 0;
}

static int _get_tz_offset_minutes(void) {
    return (int)_zos_syscall0(SYS_GETTZ);
}

static time_t _current_time_epoch(void) {
    struct _mtk_datetime dt = {};
    _zos_syscall1(SYS_GETTIME, (long)&dt);
    return _epoch_from_datetime((int)dt.year, (int)dt.month, (int)dt.day,
                                (int)dt.hour, (int)dt.minute, (int)dt.second)
         - (time_t)_get_tz_offset_minutes() * 60;
}

static int _append_char(char *buf, size_t max, size_t *pos, char c) {
    if (*pos + 1 >= max) return 0;
    buf[(*pos)++] = c;
    return 1;
}

static int _append_str(char *buf, size_t max, size_t *pos, const char *s) {
    while (*s) {
        if (!_append_char(buf, max, pos, *s++)) return 0;
    }
    return 1;
}

static int _append_num(char *buf, size_t max, size_t *pos, int value, int width, char pad) {
    char tmp[16];
    int i = 0;

    if (value < 0) value = -value;
    do {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0 && i < (int)sizeof(tmp));

    while (i < width) {
        tmp[i++] = pad;
    }

    while (i > 0) {
        if (!_append_char(buf, max, pos, tmp[--i])) return 0;
    }
    return 1;
}

static int _path_is_directory(const char *path) {
    const char *names[1];
    return (int)_zos_syscall3(SYS_READDIR, (long)path, (long)names, 1L) >= 0;
}

static int _path_local_prefix(const char *path, char *buf, size_t size) {
    if (buf == NULL || size == 0) return 0;
    buf[0] = '\0';
    if (path == NULL) return 0;

    const char *local = path;
    for (int i = 0; local[i]; i++) {
        if (local[i] == ':') {
            local += i + 1;
            if (local[0] == '/') local++;
            break;
        }
    }

    size_t len = strlen(local);
    while (len > 0 && local[len - 1] == '/') len--;
    if (len == 0) return 0;

    if (len >= size - 1) len = size - 2;
    memcpy(buf, local, len);
    buf[len++] = '/';
    buf[len] = '\0';
    return (int)len;
}

static int _find_env_slot(const char *name) {
    for (int i = 0; i < (int)(sizeof(_env_entries) / sizeof(_env_entries[0])); i++) {
        if (_env_entries[i].name != NULL && strcmp(_env_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int _alloc_env_slot(void) {
    for (int i = 0; i < (int)(sizeof(_env_entries) / sizeof(_env_entries[0])); i++) {
        if (_env_entries[i].name == NULL)
            return i;
    }
    return -1;
}

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

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n]) {
        if (strchr(accept, s[n]) == NULL) break;
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n]) {
        if (strchr(reject, s[n]) != NULL) break;
        n++;
    }
    return n;
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

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        if (strchr(accept, *s) != NULL)
            return (char *)s;
        s++;
    }
    return NULL;
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

int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);
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

const char *strerror(int errnum) {
    switch (errnum) {
    case 0:      return "success";
    case EPERM:  return "operation not permitted";
    case ENOENT: return "no such file or directory";
    case EIO:    return "input/output error";
    case EBADF:  return "bad file descriptor";
    case ENOMEM: return "not enough memory";
    case EACCES: return "permission denied";
    case EEXIST: return "file exists";
    case ENOTDIR:return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case ERANGE: return "result out of range";
    case ENOSYS: return "function not implemented";
    default:     return "error";
    }
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
    if (name == NULL || name[0] == '\0') return NULL;

    int slot = _find_env_slot(name);
    if (slot < 0) return NULL;
    return _env_entries[slot].value;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }

    int slot = _find_env_slot(name);
    if (slot >= 0 && !overwrite) {
        return 0;
    }

    if (slot < 0) {
        slot = _alloc_env_slot();
        if (slot < 0) {
            errno = ENOMEM;
            return -1;
        }
    } else {
        free(_env_entries[slot].name);
        free(_env_entries[slot].value);
        _env_entries[slot].name = NULL;
        _env_entries[slot].value = NULL;
    }

    _env_entries[slot].name = strdup(name);
    _env_entries[slot].value = strdup(value);
    if (_env_entries[slot].name == NULL || _env_entries[slot].value == NULL) {
        free(_env_entries[slot].name);
        free(_env_entries[slot].value);
        _env_entries[slot].name = NULL;
        _env_entries[slot].value = NULL;
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

int unsetenv(const char *name) {
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    int slot = _find_env_slot(name);
    if (slot < 0) return 0;

    free(_env_entries[slot].name);
    free(_env_entries[slot].value);
    _env_entries[slot].name = NULL;
    _env_entries[slot].value = NULL;
    return 0;
}

int putenv(char *string) {
    if (string == NULL) {
        errno = EINVAL;
        return -1;
    }

    char *eq = strchr(string, '=');
    if (eq == NULL || eq == string) {
        errno = EINVAL;
        return -1;
    }

    size_t name_len = (size_t)(eq - string);
    char *name = (char *)malloc(name_len + 1);
    if (name == NULL) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(name, string, name_len);
    name[name_len] = '\0';

    int rc = setenv(name, eq + 1, 1);
    free(name);
    return rc;
}

static void (*_atexit_funcs[32])(void);
static int _atexit_count = 0;

int atexit(void (*func)(void)) {
    if (_atexit_count >= 32 || func == NULL) return -1;
    _atexit_funcs[_atexit_count++] = func;
    return 0;
}

void exit(int status) {
    for (int i = _atexit_count - 1; i >= 0; i--)
        _atexit_funcs[i]();
    _zos_syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

void abort(void) {
    _zos_syscall1(SYS_PRINT, (long)"abort() called\n");
    _zos_syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}

static int _system_parse_drive_prefix(const char *s) {
    if (s == NULL || s[0] < '0' || s[0] > '9') return -1;
    int drive = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        drive = drive * 10 + (s[i] - '0');
        i++;
    }
    return (s[i] == ':') ? drive : -1;
}

static void _system_build_drive_path(int drive, const char *leaf, char *out, size_t out_size) {
    snprintf(out, out_size, "%d:/%s", drive, (leaf != NULL) ? leaf : "");
}

static int _system_try_spawn(const char *path, const char *args) {
    int pid = (int)_zos_syscall2(SYS_SPAWN, (long)path, (long)args);
    if (pid < 0) return -1;
    _zos_syscall1(SYS_WAITPID, (long)pid);
    return 0;
}

int system(const char *command) {
    char command_buf[256];
    char cwd[128] = "";
    char path[256];
    char leaf[128];
    char *args = NULL;
    int current_drive = 0;
    int has_slash = 0;

    if (command == NULL) return -1;

    while (*command == ' ' || *command == '\t' || *command == '\n') command++;
    if (*command == '\0') return 0;

    size_t command_len = strlen(command);
    if (command_len >= sizeof(command_buf)) {
        errno = EINVAL;
        return -1;
    }

    memcpy(command_buf, command, command_len + 1);

    char *prog = command_buf;
    while (*prog == ' ' || *prog == '\t' || *prog == '\n') prog++;
    if (*prog == '\0') return 0;

    char *split = prog;
    while (*split != '\0' && *split != ' ' && *split != '\t' && *split != '\n') {
        if (*split == '/') has_slash = 1;
        split++;
    }
    if (*split != '\0') {
        *split++ = '\0';
        while (*split == ' ' || *split == '\t' || *split == '\n') split++;
        if (*split != '\0') args = split;
    }

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        int drive = _system_parse_drive_prefix(cwd);
        if (drive >= 0) current_drive = drive;
    }

    if (_system_parse_drive_prefix(prog) >= 0 || prog[0] == '/' || prog[0] == '.' || has_slash) {
        if (_system_try_spawn(prog, args) == 0) return 0;
        snprintf(path, sizeof(path), "%s.elf", prog);
        if (_system_try_spawn(path, args) == 0) return 0;
        errno = ENOENT;
        return -1;
    }

    snprintf(leaf, sizeof(leaf), "%s", prog);
    if (cwd[0] != '\0') {
        const char *sep = cwd[strlen(cwd) - 1] == '/' ? "" : "/";
        snprintf(path, sizeof(path), "%s%s%s", cwd, sep, leaf);
        if (_system_try_spawn(path, args) == 0) return 0;
        snprintf(path, sizeof(path), "%s%s%s.elf", cwd, sep, leaf);
        if (_system_try_spawn(path, args) == 0) return 0;
    }

    snprintf(path, sizeof(path), "0:/os/%s.elf", leaf);
    if (_system_try_spawn(path, args) == 0) return 0;
    snprintf(path, sizeof(path), "0:/os/%s", leaf);
    if (_system_try_spawn(path, args) == 0) return 0;
    snprintf(path, sizeof(path), "0:/games/%s.elf", leaf);
    if (_system_try_spawn(path, args) == 0) return 0;

    if (current_drive != 0) {
        _system_build_drive_path(current_drive, leaf, path, sizeof(path));
        if (_system_try_spawn(path, args) == 0) return 0;
        snprintf(leaf, sizeof(leaf), "%s.elf", prog);
        _system_build_drive_path(current_drive, leaf, path, sizeof(path));
        if (_system_try_spawn(path, args) == 0) return 0;
    }

    errno = ENOENT;
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

/* ========================================================================
   stdio.h FILE-based I/O
   ======================================================================== */

/* Static FILE objects for standard streams */
static FILE _stdin_file  = { .handle = -1, .pos = 0, .size = 0, .eof = 0,
                             .error = 0, .is_std = 1, .ungetc_buf = -1 };
static FILE _stdout_file = { .handle = -1, .pos = 0, .size = 0, .eof = 0,
                             .error = 0, .is_std = 2, .ungetc_buf = -1 };
static FILE _stderr_file = { .handle = -1, .pos = 0, .size = 0, .eof = 0,
                             .error = 0, .is_std = 3, .ungetc_buf = -1 };

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

static char _stdin_linebuf[512];
static size_t _stdin_line_len = 0;
static size_t _stdin_line_pos = 0;

static void _stdin_echo_char(char c) {
    if (c == '\b') {
        _zos_syscall1(SYS_PUTCHAR, '\b');
        _zos_syscall1(SYS_PUTCHAR, ' ');
        _zos_syscall1(SYS_PUTCHAR, '\b');
        return;
    }
    _zos_syscall1(SYS_PUTCHAR, (unsigned char)c);
}

static int _stdin_fill_line(void) {
    _stdin_line_len = 0;
    _stdin_line_pos = 0;

    while (_stdin_line_len + 1 < sizeof(_stdin_linebuf)) {
        int c = (int)_zos_syscall0(SYS_GETCHAR);
        if (c <= 0) continue;

        if (c == '\r') c = '\n';

        if (c == '\b' || c == 127) {
            if (_stdin_line_len > 0) {
                _stdin_line_len--;
                _stdin_echo_char('\b');
            }
            continue;
        }

        if (c == '\n') {
            _stdin_linebuf[_stdin_line_len++] = (char)c;
            _stdin_echo_char((char)c);
            return 1;
        }

        if (c >= ' ' || c == '\t') {
            _stdin_linebuf[_stdin_line_len++] = (char)c;
            _stdin_echo_char((char)c);
        }
    }

    if (_stdin_line_len > 0) {
        _stdin_linebuf[_stdin_line_len++] = '\n';
        _stdin_echo_char('\n');
        return 1;
    }

    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    if (path == NULL || mode == NULL) return NULL;

    int handle = -1;
    int want_read  = 0;
    int want_write = 0;
    int want_append = 0;

    if (mode[0] == 'r') {
        want_read = 1;
        if (mode[1] == '+') want_write = 1;
    } else if (mode[0] == 'w') {
        want_write = 1;
        if (mode[1] == '+') want_read = 1;
        /* Truncate: delete then create */
        _zos_syscall1(SYS_FDELETE, (long)path);
        handle = (int)_zos_syscall1(SYS_FCREATE, (long)path);
        if (handle < 0) return NULL;
    } else if (mode[0] == 'a') {
        want_write = 1;
        want_append = 1;
        if (mode[1] == '+') want_read = 1;
        /* Try open existing, create if not found */
        handle = (int)_zos_syscall1(SYS_OPEN, (long)path);
        if (handle < 0) {
            handle = (int)_zos_syscall1(SYS_FCREATE, (long)path);
            if (handle < 0) return NULL;
        }
    } else {
        return NULL;
    }

    /* For read and read+ modes, just open existing */
    if (mode[0] == 'r') {
        handle = (int)_zos_syscall1(SYS_OPEN, (long)path);
        if (handle < 0) return NULL;
    }

    unsigned long fileSize = (unsigned long)_zos_syscall1(SYS_GETSIZE, (long)handle);

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (f == NULL) {
        _zos_syscall1(SYS_CLOSE, (long)handle);
        return NULL;
    }

    f->handle = handle;
    f->size = fileSize;
    f->pos = want_append ? fileSize : 0;
    f->eof = 0;
    f->error = 0;
    f->is_std = 0;
    f->ungetc_buf = -1;

    (void)want_read;
    (void)want_write;

    return f;
}

int fclose(FILE *stream) {
    if (stream == NULL) return EOF;
    if (stream->is_std) return 0;

    _zos_syscall1(SYS_CLOSE, (long)stream->handle);
    free(stream);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (stream == NULL || ptr == NULL || size == 0 || nmemb == 0) return 0;

    if (stream->is_std == 1) {
        size_t total = size * nmemb;
        char *dst = (char *)ptr;
        size_t i = 0;
        for (; i < total; i++) {
            int c = fgetc(stream);
            if (c == EOF) {
                stream->eof = 1;
                break;
            }
            dst[i] = (char)c;
        }
        return i / size;
    }

    size_t total = size * nmemb;
    size_t remaining = (stream->pos < stream->size) ? stream->size - stream->pos : 0;
    if (total > remaining) {
        total = remaining;
        stream->eof = 1;
    }
    if (total == 0) return 0;

    int ret = (int)_zos_syscall4(SYS_READ, (long)stream->handle,
                                  (long)ptr, (long)stream->pos, (long)total);
    if (ret < 0) {
        stream->error = 1;
        return 0;
    }

    stream->pos += (unsigned long)ret;
    return (size_t)ret / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (stream == NULL || ptr == NULL || size == 0 || nmemb == 0) return 0;

    if (stream->is_std) {
        /* stdout/stderr: print via SYS_PRINT/SYS_PUTCHAR */
        size_t total = size * nmemb;
        const char *src = (const char *)ptr;
        for (size_t i = 0; i < total; i++)
            _zos_syscall1(SYS_PUTCHAR, (long)(unsigned char)src[i]);
        return nmemb;
    }

    size_t total = size * nmemb;
    int ret = (int)_zos_syscall4(SYS_FWRITE, (long)stream->handle,
                                  (long)ptr, (long)stream->pos, (long)total);
    if (ret < 0) {
        stream->error = 1;
        return 0;
    }

    stream->pos += (unsigned long)ret;
    /* Update size if we wrote past the old end */
    if (stream->pos > stream->size)
        stream->size = stream->pos;
    return (size_t)ret / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (stream == NULL || stream->is_std) return -1;

    unsigned long newpos;
    switch (whence) {
    case SEEK_SET: newpos = (unsigned long)offset; break;
    case SEEK_CUR: newpos = stream->pos + (unsigned long)offset; break;
    case SEEK_END: newpos = stream->size + (unsigned long)offset; break;
    default: return -1;
    }

    stream->pos = newpos;
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (stream == NULL) return -1;
    return (long)stream->pos;
}

int fflush(FILE *stream) {
    /* No buffering in this implementation */
    (void)stream;
    return 0;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (stream == NULL) return NULL;
    if (path == NULL) return stream;

    FILE *fresh = fopen(path, mode);
    if (fresh == NULL) return NULL;

    if (!stream->is_std) {
        _zos_syscall1(SYS_CLOSE, (long)stream->handle);
    }

    *stream = *fresh;
    free(fresh);
    return stream;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

int feof(FILE *stream) {
    if (stream == NULL) return 0;
    return stream->eof;
}

int ferror(FILE *stream) {
    if (stream == NULL) return 0;
    return stream->error;
}

void clearerr(FILE *stream) {
    if (stream == NULL) return;
    stream->eof = 0;
    stream->error = 0;
}

int fgetc(FILE *stream) {
    if (stream == NULL) return EOF;

    /* Check ungetc buffer first */
    if (stream->ungetc_buf >= 0) {
        int c = stream->ungetc_buf;
        stream->ungetc_buf = -1;
        return c;
    }

    if (stream->is_std == 1) {
        if (_stdin_line_pos >= _stdin_line_len) {
            if (!_stdin_fill_line()) {
                stream->eof = 1;
                return EOF;
            }
        }
        return (unsigned char)_stdin_linebuf[_stdin_line_pos++];
    }

    if (stream->pos >= stream->size) {
        stream->eof = 1;
        return EOF;
    }

    unsigned char c;
    int ret = (int)_zos_syscall4(SYS_READ, (long)stream->handle,
                                  (long)&c, (long)stream->pos, 1L);
    if (ret <= 0) {
        stream->eof = 1;
        return EOF;
    }

    stream->pos++;
    return (int)c;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    size_t n = fwrite(&ch, 1, 1, stream);
    return (n == 1) ? c : EOF;
}

int ungetc(int c, FILE *stream) {
    if (stream == NULL || c == EOF) return EOF;
    stream->ungetc_buf = c;
    stream->eof = 0;
    return c;
}

char *fgets(char *s, int size, FILE *stream) {
    if (s == NULL || size <= 0 || stream == NULL) return NULL;

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (s == NULL || stream == NULL) return EOF;

    size_t len = strlen(s);
    size_t written = fwrite(s, 1, len, stream);
    return (written == len) ? 0 : EOF;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (stream == NULL || stream->is_std) {
        _zos_syscall1(SYS_PRINT, (long)buf);
    } else {
        fwrite(buf, 1, (size_t)(n > 0 ? n : 0), stream);
    }
    return n;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (stream == NULL || stream->is_std) {
        _zos_syscall1(SYS_PRINT, (long)buf);
    } else {
        fwrite(buf, 1, (size_t)(n > 0 ? n : 0), stream);
    }
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int vsprintf(char *str, const char *fmt, va_list ap) {
    return vsnprintf(str, (size_t)-1, fmt, ap);
}

int remove(const char *path) {
    if (path == NULL) return -1;
    return (int)_zos_syscall1(SYS_FDELETE, (long)path);
}

int rename(const char *oldpath, const char *newpath) {
    if (oldpath == NULL || newpath == NULL) return -1;
    return (int)_zos_syscall2(SYS_FRENAME, (long)oldpath, (long)newpath);
}

void perror(const char *s) {
    if (s != NULL && s[0] != '\0') {
        _zos_syscall1(SYS_PRINT, (long)s);
        _zos_syscall1(SYS_PRINT, (long)": ");
    }
    _zos_syscall1(SYS_PRINT, (long)"error\n");
}

FILE *tmpfile(void) {
    char path[L_tmpnam];
    if (tmpnam(path) == NULL) return NULL;
    return fopen(path, "w+");
}

char *tmpnam(char *s) {
    static char internal[L_tmpnam];
    static unsigned long counter = 0;
    char *out = (s != NULL) ? s : internal;

    _zos_syscall1(SYS_FMKDIR, (long)"0:/tmp");
    snprintf(out, L_tmpnam, "0:/tmp/tmp%lu.tmp",
             (unsigned long)_zos_syscall0(SYS_GETMILLISECONDS) + counter++);
    return out;
}

/* ========================================================================
   sscanf (minimal: %d, %x, %s, %c, %u, %ld, %lu)
   ======================================================================== */

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int matched = 0;
    const char *s = str;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            int is_long = 0;
            if (*fmt == 'l') { is_long = 1; fmt++; }

            if (*fmt == 'd' || *fmt == 'i') {
                while (isspace((unsigned char)*s)) s++;
                int neg = 0;
                if (*s == '-') { neg = 1; s++; }
                else if (*s == '+') s++;
                if (!isdigit((unsigned char)*s)) goto done;
                long val = 0;
                while (isdigit((unsigned char)*s))
                    val = val * 10 + (*s++ - '0');
                if (neg) val = -val;
                if (is_long) *va_arg(ap, long *) = val;
                else *va_arg(ap, int *) = (int)val;
                matched++;
            } else if (*fmt == 'u') {
                while (isspace((unsigned char)*s)) s++;
                if (!isdigit((unsigned char)*s)) goto done;
                unsigned long val = 0;
                while (isdigit((unsigned char)*s))
                    val = val * 10 + (unsigned long)(*s++ - '0');
                if (is_long) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            } else if (*fmt == 'x' || *fmt == 'X') {
                while (isspace((unsigned char)*s)) s++;
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
                if (!isxdigit((unsigned char)*s)) goto done;
                unsigned long val = 0;
                while (isxdigit((unsigned char)*s)) {
                    int d;
                    if (*s >= '0' && *s <= '9') d = *s - '0';
                    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                    else d = *s - 'A' + 10;
                    val = val * 16 + (unsigned long)d;
                    s++;
                }
                if (is_long) *va_arg(ap, unsigned long *) = val;
                else *va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            } else if (*fmt == 's') {
                while (isspace((unsigned char)*s)) s++;
                char *dst = va_arg(ap, char *);
                if (*s == '\0') goto done;
                while (*s && !isspace((unsigned char)*s))
                    *dst++ = *s++;
                *dst = '\0';
                matched++;
            } else if (*fmt == 'c') {
                if (*s == '\0') goto done;
                *va_arg(ap, char *) = *s++;
                matched++;
            } else if (*fmt == '%') {
                if (*s != '%') goto done;
                s++;
            }
            fmt++;
        } else if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            fmt++;
        } else {
            if (*s != *fmt) goto done;
            s++;
            fmt++;
        }
    }

done:
    va_end(ap);
    return matched;
}

/* ========================================================================
   fcntl.h / unistd.h POSIX-style file I/O
   ======================================================================== */

/* fd position tracking for POSIX read/write (MontaukOS uses explicit offsets) */
#define _FD_POS_MAX 64
static unsigned long _fd_pos[_FD_POS_MAX];

int access(const char *path, int mode) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    (void)mode;

    int h = (int)_zos_syscall1(SYS_OPEN, (long)path);
    if (h >= 0) {
        _zos_syscall1(SYS_CLOSE, (long)h);
        return 0;
    }

    if (_path_is_directory(path)) {
        return 0;
    }

    errno = ENOENT;
    return -1;
}

int open(const char *path, int flags, ...) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    int h = (int)_zos_syscall1(SYS_OPEN, (long)path);
    int wants_create = (flags & O_CREAT) != 0;
    int wants_trunc = (flags & O_TRUNC) != 0;

    if (h < 0) {
        if (wants_create) {
            h = (int)_zos_syscall1(SYS_FCREATE, (long)path);
        }
    } else if (wants_trunc) {
        _zos_syscall1(SYS_CLOSE, (long)h);
        _zos_syscall1(SYS_FDELETE, (long)path);
        h = (int)_zos_syscall1(SYS_FCREATE, (long)path);
    }

    if (h >= 0 && h < _FD_POS_MAX) {
        _fd_pos[h] = 0;
    }
    if (h < 0) {
        errno = ENOENT;
    }
    return h;
}

int read(int fd, void *buf, size_t count) {
    if (buf == NULL) return -1;
    unsigned long pos = (fd >= 0 && fd < _FD_POS_MAX) ? _fd_pos[fd] : 0;
    int ret = (int)_zos_syscall4(SYS_READ, (long)fd, (long)buf, (long)pos, (long)count);
    if (ret > 0 && fd >= 0 && fd < _FD_POS_MAX)
        _fd_pos[fd] += (unsigned long)ret;
    return ret;
}

int write(int fd, const void *buf, size_t count) {
    if (buf == NULL) return -1;
    unsigned long pos = (fd >= 0 && fd < _FD_POS_MAX) ? _fd_pos[fd] : 0;
    int ret = (int)_zos_syscall4(SYS_FWRITE, (long)fd, (long)buf, (long)pos, (long)count);
    if (ret > 0 && fd >= 0 && fd < _FD_POS_MAX)
        _fd_pos[fd] += (unsigned long)ret;
    return ret;
}

int close(int fd) {
    if (fd >= 0 && fd < _FD_POS_MAX)
        _fd_pos[fd] = 0;
    if ((int)_zos_syscall1(SYS_CLOSE, (long)fd) < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

long lseek(int fd, long offset, int whence) {
    unsigned long pos = (fd >= 0 && fd < _FD_POS_MAX) ? _fd_pos[fd] : 0;
    unsigned long newpos;
    switch (whence) {
    case 0: /* SEEK_SET */
        newpos = (unsigned long)offset;
        break;
    case 1: /* SEEK_CUR */
        newpos = pos + (unsigned long)offset;
        break;
    case 2: /* SEEK_END */
        newpos = (unsigned long)_zos_syscall1(SYS_GETSIZE, (long)fd) + (unsigned long)offset;
        break;
    default:
        return -1;
    }
    if (fd >= 0 && fd < _FD_POS_MAX)
        _fd_pos[fd] = newpos;
    return (long)newpos;
}

int chdir(const char *path) {
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if ((int)_zos_syscall1(SYS_CHDIR, (long)path) < 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

char *getcwd(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if ((int)_zos_syscall2(SYS_GETCWD, (long)buf, (long)size) < 0) {
        errno = ERANGE;
        return NULL;
    }
    return buf;
}

int isatty(int fd) {
    return fd >= 0 && fd <= 2;
}

/* ========================================================================
   sys/stat.h functions
   ======================================================================== */

int mkdir(const char *path, unsigned int mode) {
    (void)mode;
    if (path == NULL) return -1;
    return (int)_zos_syscall1(SYS_FMKDIR, (long)path);
}

int stat(const char *path, struct stat *buf) {
    if (path == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));

    int h = (int)_zos_syscall1(SYS_OPEN, (long)path);
    if (h >= 0) {
        buf->st_mode = S_IFREG;
        buf->st_size = (unsigned long)_zos_syscall1(SYS_GETSIZE, (long)h);
        _zos_syscall1(SYS_CLOSE, (long)h);
        return 0;
    }

    if (_path_is_directory(path)) {
        buf->st_mode = S_IFDIR;
        buf->st_size = 0;
        return 0;
    }

    errno = ENOENT;
    return -1;
}

int fstat(int fd, struct stat *buf) {
    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = S_IFREG;
    buf->st_size = (unsigned long)_zos_syscall1(SYS_GETSIZE, (long)fd);
    return 0;
}

/* ========================================================================
   dirent.h functions
   ======================================================================== */

DIR *opendir(const char *name) {
    if (name == NULL) {
        errno = EINVAL;
        return NULL;
    }

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (dir == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    const char *raw_names[256];
    int count = (int)_zos_syscall3(SYS_READDIR, (long)name, (long)raw_names, 256L);
    if (count < 0) {
        free(dir);
        errno = ENOTDIR;
        return NULL;
    }

    memset(dir, 0, sizeof(*dir));
    dir->count = count;
    dir->index = 0;

    char local_prefix[256];
    int prefix_len = _path_local_prefix(name, local_prefix, sizeof(local_prefix));
    for (int i = 0; i < count && i < 256; i++) {
        const char *entry = raw_names[i];
        if (prefix_len > 0 &&
            strncmp(entry, local_prefix, (size_t)prefix_len) == 0) {
            entry += prefix_len;
        }
        strncpy(dir->names[i], entry, NAME_MAX);
        dir->names[i][NAME_MAX] = '\0';
    }

    return dir;
}

struct dirent *readdir(DIR *dirp) {
    if (dirp == NULL || dirp->index >= dirp->count) {
        return NULL;
    }

    memset(&dirp->entry, 0, sizeof(dirp->entry));
    dirp->entry.d_type = DT_UNKNOWN;
    strncpy(dirp->entry.d_name, dirp->names[dirp->index], NAME_MAX);
    dirp->entry.d_name[NAME_MAX] = '\0';
    dirp->index++;
    return &dirp->entry;
}

int closedir(DIR *dirp) {
    if (dirp == NULL) {
        errno = EINVAL;
        return -1;
    }
    free(dirp);
    return 0;
}

void rewinddir(DIR *dirp) {
    if (dirp == NULL) return;
    dirp->index = 0;
}

/* ========================================================================
   stdlib.h: qsort
   ======================================================================== */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || base == NULL || compar == NULL) return;

    /* Simple insertion sort for small arrays, good enough for libc */
    unsigned char *b = (unsigned char *)base;
    unsigned char tmp[256];
    int use_heap = (size > sizeof(tmp));
    unsigned char *t = use_heap ? (unsigned char *)malloc(size) : tmp;
    if (t == NULL) return;

    for (size_t i = 1; i < nmemb; i++) {
        unsigned char *cur = b + i * size;
        size_t j = i;
        while (j > 0) {
            unsigned char *prev = b + (j - 1) * size;
            if (compar(prev, cur) <= 0) break;
            j--;
            cur = prev;
        }
        if (j != i) {
            unsigned char *dst = b + j * size;
            unsigned char *src = b + i * size;
            memcpy(t, src, size);
            memmove(dst + size, dst, (i - j) * size);
            memcpy(dst, t, size);
        }
    }

    if (use_heap) free(t);
}

/* ========================================================================
   stdlib.h: rand / srand
   ======================================================================== */

static unsigned int _rand_seed = 1;

int rand(void) {
    _rand_seed = _rand_seed * 1103515245 + 12345;
    return (int)((_rand_seed >> 16) & 0x7fff);
}

void srand(unsigned int seed) {
    _rand_seed = seed;
}

div_t div(int numer, int denom) {
    div_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

double strtod(const char *nptr, char **endptr) {
    double result = 0.0;
    double sign = 1.0;
    const char *s = nptr;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;

    while (*s >= '0' && *s <= '9')
        result = result * 10.0 + (*s++ - '0');

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s++ - '0') * frac;
            frac *= 0.1;
        }
    }

    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        int exp_val = 0;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9')
            exp_val = exp_val * 10 + (*s++ - '0');
        double exp_mult = 1.0;
        for (int i = 0; i < exp_val; i++)
            exp_mult *= 10.0;
        if (exp_sign > 0) result *= exp_mult;
        else result /= exp_mult;
    }

    if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        s = nptr + 2;
        result = 0.0;
        while (1) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            result = result * 16.0 + d;
            s++;
        }
        if (*s == '.') {
            s++;
            double frac = 1.0 / 16.0;
            while (1) {
                int d;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                else break;
                result += d * frac;
                frac /= 16.0;
                s++;
            }
        }
        if (*s == 'p' || *s == 'P') {
            s++;
            int exp_sign = 1;
            int exp_val = 0;
            if (*s == '-') { exp_sign = -1; s++; }
            else if (*s == '+') s++;
            while (*s >= '0' && *s <= '9')
                exp_val = exp_val * 10 + (*s++ - '0');
            result = ldexp(result, exp_sign * exp_val);
        }
    }

    if (endptr) *endptr = (char *)s;
    return sign * result;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

/* ========================================================================
   math.h functions
   ======================================================================== */

/* Constants */
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_LN2       0.69314718055994530942
#define M_LOG2E     1.44269504088896340736

double fabs(double x) { return x < 0 ? -x : x; }

double frexp(double x, int *exp) {
    int e = 0;
    double ax = fabs(x);

    if (ax == 0.0) {
        if (exp) *exp = 0;
        return 0.0;
    }

    while (ax >= 1.0) {
        ax *= 0.5;
        e++;
    }
    while (ax < 0.5) {
        ax *= 2.0;
        e--;
    }

    if (exp) *exp = e;
    return x < 0.0 ? -ax : ax;
}

double ldexp(double x, int n) {
    if (x == 0.0 || n == 0) return x;
    while (n > 0) { x *= 2.0; n--; }
    while (n < 0) { x *= 0.5; n++; }
    return x;
}

long double ldexpl(long double x, int n) {
    return (long double)ldexp((double)x, n);
}

double floor(double x) {
    double t = (double)(long long)x;
    return (x < t) ? t - 1.0 : t;
}

double ceil(double x) {
    double f = floor(x);
    return (x > f) ? f + 1.0 : f;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - (double)((long long)(x / y)) * y;
}

double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double guess = x;
    for (int i = 0; i < 20; i++)
        guess = (guess + x / guess) * 0.5;
    return guess;
}

/* ---- sin / cos via range reduction + minimax polynomial ---- */

/* Reduce x to [-pi, pi] */
static double _reduce_angle(double x) {
    /* Bring into [-2pi, 2pi] via fmod, then into [-pi, pi] */
    x = fmod(x, 2.0 * M_PI);
    if (x > M_PI) x -= 2.0 * M_PI;
    else if (x < -M_PI) x += 2.0 * M_PI;
    return x;
}

/* Core sin approximation for x in [-pi/2, pi/2].
   Taylor series to degree 17 for < 1e-11 accuracy at the boundary. */
static double _sin_core(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0
           + x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0
           + x2 * (1.0/6227020800.0 + x2 * (-1.0/1307674368000.0))))))));
}

double sin(double x) {
    x = _reduce_angle(x);
    /* Reduce to [-pi/2, pi/2] using sin(pi - x) = sin(x) */
    if (x > M_PI_2) x = M_PI - x;
    else if (x < -M_PI_2) x = -M_PI - x;
    return _sin_core(x);
}

double cos(double x) {
    return sin(x + M_PI_2);
}

/* ---- log via exponent extraction + polynomial on [1, 2) ---- */

/* Union for double bit manipulation */
typedef union { double d; uint64_t u; } _dbl_bits;

double log(double x) {
    if (x <= 0.0) return -HUGE_VAL;
    if (x == 1.0) return 0.0;

    /* Extract exponent and mantissa: x = m * 2^e, where m in [1, 2) */
    _dbl_bits bits;
    bits.d = x;
    int e = (int)((bits.u >> 52) & 0x7FF) - 1023;
    bits.u = (bits.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = bits.d;

    /* log(x) = e * ln(2) + log(m), where m in [1, 2)
       Use log(m) = log((1+f)/(1-f)) = 2*(f + f^3/3 + f^5/5 + ...) where f = (m-1)/(m+1) */
    double f = (m - 1.0) / (m + 1.0);
    double f2 = f * f;
    double ln_m = 2.0 * f * (1.0 + f2 * (1.0/3.0 + f2 * (1.0/5.0 + f2 * (1.0/7.0
                 + f2 * (1.0/9.0 + f2 * (1.0/11.0 + f2 * (1.0/13.0
                 + f2 * (1.0/15.0 + f2 * (1.0/17.0)))))))));

    return (double)e * M_LN2 + ln_m;
}

/* ---- exp via range reduction to [0, ln2) + polynomial ---- */

double exp(double x) {
    if (x == 0.0) return 1.0;
    if (x < -708.0) return 0.0;
    if (x > 709.0) return HUGE_VAL;

    /* Range reduction: exp(x) = 2^k * exp(r), where x = k*ln(2) + r, |r| <= ln(2)/2 */
    double k_real = floor(x * M_LOG2E + 0.5);
    int k = (int)k_real;
    double r = x - k_real * M_LN2;

    /* Pade-like polynomial for exp(r), |r| <= ~0.347:
       1 + r + r^2/2 + r^3/6 + r^4/24 + r^5/120 + r^6/720 + r^7/5040 */
    double exp_r = 1.0 + r * (1.0 + r * (1.0/2.0 + r * (1.0/6.0
                 + r * (1.0/24.0 + r * (1.0/120.0 + r * (1.0/720.0 + r * (1.0/5040.0)))))));

    /* Multiply by 2^k via bit manipulation */
    _dbl_bits bits;
    bits.d = exp_r;
    bits.u += (uint64_t)k << 52;
    return bits.d;
}

/* ---- pow via exp(exp * log(base)) ---- */

double pow(double base, double e) {
    if (e == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;
    if (e == 1.0) return base;

    /* Integer exponent fast path */
    if (e == (double)(long long)e && fabs(e) < 64) {
        long long ei = (long long)e;
        int neg = 0;
        if (ei < 0) { neg = 1; ei = -ei; }
        double r = 1.0;
        double b = base;
        while (ei > 0) {
            if (ei & 1) r *= b;
            b *= b;
            ei >>= 1;
        }
        return neg ? 1.0 / r : r;
    }

    /* General case */
    if (base < 0.0) return 0.0; /* negative base with fractional exp is undefined (in reals) */
    return exp(e * log(base));
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return (sin(x) > 0.0) ? HUGE_VAL : -HUGE_VAL;
    return sin(x) / c;
}

double asin(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return atan2(sqrt(1.0 - x * x), x);
}

double log2(double x)  { return log(x) * M_LOG2E; }
double log10(double x) { return log(x) * 0.43429448190325182765; /* 1/ln(10) */ }

double sinh(double x) {
    return 0.5 * (exp(x) - exp(-x));
}

double cosh(double x) {
    return 0.5 * (exp(x) + exp(-x));
}

double tanh(double x) {
    double ex = exp(x);
    double enx = exp(-x);
    return (ex - enx) / (ex + enx);
}

/* ---- atan / atan2 via polynomial approximation ---- */

/* Core atan for |x| <= ~0.414 (= tan(pi/8)).
   Taylor series converges well in this small range. */
static double _atan_small(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/3.0 + x2 * (1.0/5.0 + x2 * (-1.0/7.0
           + x2 * (1.0/9.0 + x2 * (-1.0/11.0 + x2 * (1.0/13.0
           + x2 * (-1.0/15.0 + x2 * (1.0/17.0)))))))));
}

#define M_PI_4 0.78539816339744830962

/* atan for x >= 0, using range reduction:
   - |x| <= tan(pi/8) ~ 0.4142: polynomial directly
   - 0.4142 < |x| <= 1: atan(x) = pi/4 + atan((x-1)/(x+1))
   - |x| > 1: atan(x) = pi/2 - atan(1/x) */
static double _atan_positive(double x) {
    if (x <= 0.41421356237309504) {
        return _atan_small(x);
    } else if (x <= 1.0) {
        return M_PI_4 + _atan_small((x - 1.0) / (x + 1.0));
    } else {
        return M_PI_2 - _atan_positive(1.0 / x);
    }
}

double atan2(double y, double x) {
    if (x == 0.0 && y == 0.0) return 0.0;
    if (x == 0.0) return (y > 0.0) ? M_PI_2 : -M_PI_2;
    if (y == 0.0) return (x > 0.0) ? 0.0 : M_PI;

    double a = _atan_positive(fabs(y) / fabs(x));

    /* Map to correct quadrant */
    if (x < 0.0) a = M_PI - a;
    if (y < 0.0) a = -a;
    return a;
}

double atan(double x) {
    if (x >= 0.0) return _atan_positive(x);
    return -_atan_positive(-x);
}

double round(double x) { return floor(x + 0.5); }

/* ---- atof: basic floating-point string parser ---- */

double atof(const char *s) {
    if (s == NULL) return 0.0;

    while (isspace((unsigned char)*s)) s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    /* Integer part */
    double val = 0.0;
    while (isdigit((unsigned char)*s)) {
        val = val * 10.0 + (*s - '0');
        s++;
    }

    /* Fractional part */
    if (*s == '.') {
        s++;
        double place = 0.1;
        while (isdigit((unsigned char)*s)) {
            val += (*s - '0') * place;
            place *= 0.1;
            s++;
        }
    }

    /* Exponent part */
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') s++;
        int ev = 0;
        while (isdigit((unsigned char)*s)) {
            ev = ev * 10 + (*s - '0');
            s++;
        }
        double mul = 1.0;
        while (ev-- > 0) mul *= 10.0;
        if (eneg) val /= mul;
        else val *= mul;
    }

    return neg ? -val : val;
}

float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float fabsf(float x)  { return x < 0.0f ? -x : x; }
float sqrtf(float x)  { return (float)sqrt((double)x); }
float sinf(float x)   { return (float)sin((double)x); }
float cosf(float x)   { return (float)cos((double)x); }

/* ========================================================================
   locale.h / signal.h functions
   ======================================================================== */

char *setlocale(int category, const char *locale) {
    static char current[] = "C";

    (void)category;

    if (locale == NULL) return current;
    if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0 || locale[0] == '\0')
        return current;
    return NULL;
}

struct lconv *localeconv(void) {
    static struct lconv conv = { (char *)"." };
    return &conv;
}

sighandler_t signal(int sig, sighandler_t handler) {
    if (sig <= 0 || sig >= (int)(sizeof(_signal_handlers) / sizeof(_signal_handlers[0]))) {
        errno = EINVAL;
        return SIG_ERR;
    }

    sighandler_t prev = _signal_handlers[sig];
    _signal_handlers[sig] = handler;
    return prev ? prev : SIG_DFL;
}

int raise(int sig) {
    if (sig <= 0 || sig >= (int)(sizeof(_signal_handlers) / sizeof(_signal_handlers[0]))) {
        errno = EINVAL;
        return -1;
    }

    sighandler_t handler = _signal_handlers[sig];
    if (handler == NULL || handler == SIG_DFL || handler == SIG_IGN) {
        return 0;
    }

    handler(sig);
    return 0;
}

/* ========================================================================
   time.h / sys/time.h functions
   ======================================================================== */

clock_t clock(void) {
    return (clock_t)_zos_syscall0(SYS_GETMILLISECONDS);
}

time_t time(time_t *tloc) {
    time_t now = _current_time_epoch();
    if (tloc != NULL) *tloc = now;
    return now;
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

struct tm *gmtime(const time_t *timer) {
    static struct tm out;
    if (timer == NULL) return NULL;
    _tm_from_epoch(*timer, &out);
    return &out;
}

struct tm *localtime(const time_t *timer) {
    static struct tm out;
    if (timer == NULL) return NULL;
    _tm_from_epoch(*timer + (time_t)_get_tz_offset_minutes() * 60, &out);
    return &out;
}

time_t mktime(struct tm *tm) {
    if (tm == NULL) return (time_t)-1;

    while (tm->tm_sec < 0) { tm->tm_sec += 60; tm->tm_min--; }
    while (tm->tm_sec >= 60) { tm->tm_sec -= 60; tm->tm_min++; }
    while (tm->tm_min < 0) { tm->tm_min += 60; tm->tm_hour--; }
    while (tm->tm_min >= 60) { tm->tm_min -= 60; tm->tm_hour++; }
    while (tm->tm_hour < 0) { tm->tm_hour += 24; tm->tm_mday--; }
    while (tm->tm_hour >= 24) { tm->tm_hour -= 24; tm->tm_mday++; }
    while (tm->tm_mon < 0) { tm->tm_mon += 12; tm->tm_year--; }
    while (tm->tm_mon >= 12) { tm->tm_mon -= 12; tm->tm_year++; }

    for (;;) {
        int dim = _days_in_month(tm->tm_year + 1900, tm->tm_mon);
        if (tm->tm_mday >= 1 && tm->tm_mday <= dim) break;
        if (tm->tm_mday <= 0) {
            tm->tm_mon--;
            if (tm->tm_mon < 0) {
                tm->tm_mon = 11;
                tm->tm_year--;
            }
            tm->tm_mday += _days_in_month(tm->tm_year + 1900, tm->tm_mon);
        } else {
            tm->tm_mday -= dim;
            tm->tm_mon++;
            if (tm->tm_mon >= 12) {
                tm->tm_mon = 0;
                tm->tm_year++;
            }
        }
    }

    time_t epoch = _epoch_from_datetime(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                                        tm->tm_hour, tm->tm_min, tm->tm_sec)
                 - (time_t)_get_tz_offset_minutes() * 60;
    _tm_from_epoch(epoch + (time_t)_get_tz_offset_minutes() * 60, tm);
    return epoch;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (s == NULL || max == 0 || format == NULL || tm == NULL) return 0;

    size_t pos = 0;
    int tz = _get_tz_offset_minutes();

    while (*format) {
        if (*format != '%') {
            if (!_append_char(s, max, &pos, *format++)) return 0;
            continue;
        }

        format++;
        if (*format == '\0') break;

        switch (*format) {
        case '%':
            if (!_append_char(s, max, &pos, '%')) return 0;
            break;
        case 'a':
            if (!_append_str(s, max, &pos, _weekday_short[tm->tm_wday])) return 0;
            break;
        case 'A':
            if (!_append_str(s, max, &pos, _weekday_long[tm->tm_wday])) return 0;
            break;
        case 'b':
            if (!_append_str(s, max, &pos, _month_short[tm->tm_mon])) return 0;
            break;
        case 'B':
            if (!_append_str(s, max, &pos, _month_long[tm->tm_mon])) return 0;
            break;
        case 'c':
            if (!_append_str(s, max, &pos, _weekday_short[tm->tm_wday])) return 0;
            if (!_append_char(s, max, &pos, ' ')) return 0;
            if (!_append_str(s, max, &pos, _month_short[tm->tm_mon])) return 0;
            if (!_append_char(s, max, &pos, ' ')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_mday, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ' ')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_hour, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ':')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_min, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ':')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_sec, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ' ')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_year + 1900, 4, '0')) return 0;
            break;
        case 'd':
            if (!_append_num(s, max, &pos, tm->tm_mday, 2, '0')) return 0;
            break;
        case 'e':
            if (!_append_num(s, max, &pos, tm->tm_mday, 2, ' ')) return 0;
            break;
        case 'F':
            if (!_append_num(s, max, &pos, tm->tm_year + 1900, 4, '0')) return 0;
            if (!_append_char(s, max, &pos, '-')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_mon + 1, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, '-')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_mday, 2, '0')) return 0;
            break;
        case 'H':
            if (!_append_num(s, max, &pos, tm->tm_hour, 2, '0')) return 0;
            break;
        case 'I': {
            int hour = tm->tm_hour % 12;
            if (hour == 0) hour = 12;
            if (!_append_num(s, max, &pos, hour, 2, '0')) return 0;
            break;
        }
        case 'j':
            if (!_append_num(s, max, &pos, tm->tm_yday + 1, 3, '0')) return 0;
            break;
        case 'm':
            if (!_append_num(s, max, &pos, tm->tm_mon + 1, 2, '0')) return 0;
            break;
        case 'M':
            if (!_append_num(s, max, &pos, tm->tm_min, 2, '0')) return 0;
            break;
        case 'p':
            if (!_append_str(s, max, &pos, tm->tm_hour < 12 ? "AM" : "PM")) return 0;
            break;
        case 'R':
            if (!_append_num(s, max, &pos, tm->tm_hour, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ':')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_min, 2, '0')) return 0;
            break;
        case 'S':
            if (!_append_num(s, max, &pos, tm->tm_sec, 2, '0')) return 0;
            break;
        case 'T':
        case 'X':
            if (!_append_num(s, max, &pos, tm->tm_hour, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ':')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_min, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, ':')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_sec, 2, '0')) return 0;
            break;
        case 'u': {
            int wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
            if (!_append_num(s, max, &pos, wday, 1, '0')) return 0;
            break;
        }
        case 'w':
            if (!_append_num(s, max, &pos, tm->tm_wday, 1, '0')) return 0;
            break;
        case 'x':
            if (!_append_num(s, max, &pos, tm->tm_mon + 1, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, '/')) return 0;
            if (!_append_num(s, max, &pos, tm->tm_mday, 2, '0')) return 0;
            if (!_append_char(s, max, &pos, '/')) return 0;
            if (!_append_num(s, max, &pos, (tm->tm_year + 1900) % 100, 2, '0')) return 0;
            break;
        case 'y':
            if (!_append_num(s, max, &pos, (tm->tm_year + 1900) % 100, 2, '0')) return 0;
            break;
        case 'Y':
            if (!_append_num(s, max, &pos, tm->tm_year + 1900, 4, '0')) return 0;
            break;
        case 'z': {
            int sign = tz >= 0 ? '+' : '-';
            int abs_tz = tz >= 0 ? tz : -tz;
            if (!_append_char(s, max, &pos, (char)sign)) return 0;
            if (!_append_num(s, max, &pos, abs_tz / 60, 2, '0')) return 0;
            if (!_append_num(s, max, &pos, abs_tz % 60, 2, '0')) return 0;
            break;
        }
        case 'Z':
            if (!_append_str(s, max, &pos, "UTC")) return 0;
            break;
        default:
            if (!_append_char(s, max, &pos, '%')) return 0;
            if (!_append_char(s, max, &pos, *format)) return 0;
            break;
        }
        format++;
    }

    s[pos] = '\0';
    return pos;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    long ms = (long)_zos_syscall0(SYS_GETMILLISECONDS);

    if (tv != NULL) {
        tv->tv_sec = (long)time(NULL);
        tv->tv_usec = (ms % 1000) * 1000;
    }

    if (tz != NULL) {
        int offset = _get_tz_offset_minutes();
        tz->tz_minuteswest = -offset;
        tz->tz_dsttime = 0;
    }

    return 0;
}

/* ========================================================================
   unistd.h: sleep
   ======================================================================== */

unsigned int sleep(unsigned int seconds) {
    _zos_syscall1(SYS_SLEEP_MS, (long)seconds * 1000L);
    return 0;
}
