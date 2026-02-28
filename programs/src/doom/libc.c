/*
    * libc.c
    * Minimal C standard library for MontaukOS userspace (DOOM port)
    * Copyright (c) 2025 Daniel Hammer
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
#define SYS_OPEN    6
#define SYS_READ    7
#define SYS_GETSIZE 8
#define SYS_CLOSE   9
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
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)c;
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (s < d && d < s + n) {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    } else {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
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

static int _tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && _tolower((unsigned char)*a) == _tolower((unsigned char)*b)) { a++; b++; }
    return _tolower((unsigned char)*a) - _tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = _tolower((unsigned char)a[i]);
        int cb = _tolower((unsigned char)b[i]);
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

#define HEAP_MAGIC 0x5A484541ULL  /* "ZHEA" */

struct HeapHeader {
    uint64_t magic;
    uint64_t size;
} __attribute__((packed));

struct FreeNode {
    uint64_t       size;
    struct FreeNode *next;
};

static struct FreeNode g_heapHead = { 0, NULL };
static int g_heapInit = 0;

static void heap_insert_free(void *ptr, uint64_t size) {
    struct FreeNode *node = (struct FreeNode *)ptr;
    node->size = size;
    node->next = g_heapHead.next;
    g_heapHead.next = node;
}

static void heap_grow(uint64_t bytes) {
    uint64_t pages = (bytes + 0xFFF) / 0x1000;
    if (pages < 4) pages = 4;
    void *mem = (void *)_zos_syscall1(SYS_ALLOC, (long)(pages * 0x1000));
    if (mem != NULL)
        heap_insert_free(mem, pages * 0x1000);
}

void *malloc(size_t size) {
    if (!g_heapInit) {
        heap_grow(16 * 0x1000);
        g_heapInit = 1;
    }

    uint64_t needed = size + sizeof(struct HeapHeader);
    needed = (needed + 15) & ~15ULL;

    struct FreeNode *prev = &g_heapHead;
    struct FreeNode *cur  = g_heapHead.next;

    while (cur != NULL) {
        if (cur->size >= needed) {
            uint64_t blockSize = cur->size;
            prev->next = cur->next;

            if (blockSize > needed + sizeof(struct FreeNode) + 16) {
                void *rest = (void *)((uint8_t *)cur + needed);
                heap_insert_free(rest, blockSize - needed);
            }

            struct HeapHeader *hdr = (struct HeapHeader *)cur;
            hdr->magic = HEAP_MAGIC;
            hdr->size  = size;
            return (void *)((uint8_t *)hdr + sizeof(struct HeapHeader));
        }
        prev = cur;
        cur  = cur->next;
    }

    heap_grow(needed);
    return malloc(size);
}

void free(void *ptr) {
    if (ptr == NULL) return;

    struct HeapHeader *hdr = (struct HeapHeader *)((uint8_t *)ptr - sizeof(struct HeapHeader));
    uint64_t blockSize = hdr->size + sizeof(struct HeapHeader);
    blockSize = (blockSize + 15) & ~15ULL;
    heap_insert_free((void *)hdr, blockSize);
}

void *calloc(size_t nmemb, size_t size) {
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

    void *newp = malloc(size);
    if (newp == NULL) return NULL;

    size_t copySize = old < size ? old : size;
    memcpy(newp, ptr, copySize);
    free(ptr);
    return newp;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
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

long atol(const char *s) {
    long neg = 0, val = 0;
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
    return (unsigned long)strtol(nptr, endptr, base);
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    /* Simple insertion sort — sufficient for doom's usage */
    char *arr = (char *)base;
    char tmp[256]; /* doom's qsort elements are small */
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, arr + i * size, size);
        size_t j = i;
        while (j > 0 && compar(arr + (j - 1) * size, tmp) > 0) {
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            j--;
        }
        memcpy(arr + j * size, tmp, size);
    }
}

static unsigned int _rand_seed = 1;

int rand(void) {
    _rand_seed = _rand_seed * 1103515245 + 12345;
    return (int)((_rand_seed >> 16) & 0x7FFF);
}

void srand(unsigned int seed) {
    _rand_seed = seed;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static void (*_atexit_funcs[32])(void);
static int _atexit_count = 0;

int atexit(void (*func)(void)) {
    if (_atexit_count >= 32) return -1;
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
   printf family — vsnprintf core
   ======================================================================== */

/* Internal: write a char to buffer if space remains */
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

    /* Precision: minimum number of digits (zero-pad the digits themselves) */
    int digitCount = i;
    int precPad = 0;
    if (precision > digitCount)
        precPad = precision - digitCount;

    /* Total output length: sign + precPad + digits */
    int total = (neg ? 1 : 0) + precPad + digitCount;

    /* Width padding (if pad is '0' and no precision, treat as digit padding) */
    if (neg && pad == '0' && precision < 0) {
        _pf_putc(st, '-');
    }
    if (precision < 0 && pad == '0') {
        /* Old behavior: '0' flag pads digits */
        for (int w = total; w < width; w++)
            _pf_putc(st, '0');
    } else {
        /* Space padding on the left */
        for (int w = total; w < width; w++)
            _pf_putc(st, ' ');
    }
    if (neg && !(pad == '0' && precision < 0)) {
        _pf_putc(st, '-');
    }

    /* Precision zero-padding */
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
        fmt++; /* skip '%' */

        /* Flags */
        char pad = ' ';
        int left_align = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '0') pad = '0';
            if (*fmt == '-') { left_align = 1; pad = ' '; }
            fmt++;
        }

        /* Width */
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

        /* Precision (consumed but mostly ignored) */
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

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { is_long = 1; fmt++; }

        /* Conversion */
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
        case 'o': {
            unsigned long val;
            if (is_long >= 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            _pf_putnum(&st, val, 8, 0, width, pad, 0, precision);
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

    /* Null terminate */
    if (size > 0) {
        if (st.pos < size)
            st.buf[st.pos] = '\0';
        else
            st.buf[size - 1] = '\0';
    }

    return (int)st.pos;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
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

/* Print buffer for printf/fprintf */
static char _printbuf[4096];

int vprintf(const char *fmt, va_list ap) {
    (void)fmt; (void)ap;
    return 0;
}

int printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

int puts(const char *s) {
    (void)s;
    return 0;
}

int putchar(int c) {
    (void)c;
    return c;
}

/* ========================================================================
   FILE I/O
   ======================================================================== */

/* Simple FILE struct */
struct _FILE {
    int      handle;
    uint64_t pos;
    uint64_t size;
    int      eof;
    int      error;
    int      is_std;   /* 1 = stdout, 2 = stderr */
    int      ungetc_buf; /* -1 if empty */
};

typedef struct _FILE FILE;

/* Up to 16 open files */
#define MAX_FILES 16
static FILE _file_pool[MAX_FILES];
static int _file_pool_init = 0;

/* Standard streams */
static FILE _stdout_file = { -1, 0, 0, 0, 0, 1, -1 };
static FILE _stderr_file = { -1, 0, 0, 0, 0, 2, -1 };
static FILE _stdin_file  = { -1, 0, 0, 0, 0, 3, -1 };

FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;
FILE *stdin  = &_stdin_file;

static FILE *alloc_file(void) {
    if (!_file_pool_init) {
        memset(_file_pool, 0, sizeof(_file_pool));
        for (int i = 0; i < MAX_FILES; i++) {
            _file_pool[i].handle = -1;
            _file_pool[i].ungetc_buf = -1;
        }
        _file_pool_init = 1;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (_file_pool[i].handle == -1 && _file_pool[i].is_std == 0) {
            return &_file_pool[i];
        }
    }
    return NULL;
}

FILE *fopen(const char *path, const char *mode) {
    (void)mode;

    /* Build VFS path: prepend "0:/" if not already prefixed */
    char vfspath[256];
    if (path[0] == '0' && path[1] == ':') {
        /* Already has VFS prefix */
        size_t len = strlen(path);
        if (len >= sizeof(vfspath)) len = sizeof(vfspath) - 1;
        memcpy(vfspath, path, len);
        vfspath[len] = '\0';
    } else {
        /* Prepend "0:/" */
        const char *prefix = "0:/";
        int i = 0;
        while (prefix[i]) { vfspath[i] = prefix[i]; i++; }
        int j = 0;
        while (path[j] && i < 254) { vfspath[i++] = path[j++]; }
        vfspath[i] = '\0';
    }

    int handle = (int)_zos_syscall1(SYS_OPEN, (long)vfspath);
    if (handle < 0) {
        errno = 2; /* ENOENT */
        return NULL;
    }

    FILE *fp = alloc_file();
    if (fp == NULL) {
        _zos_syscall1(SYS_CLOSE, (long)handle);
        errno = 12; /* ENOMEM */
        return NULL;
    }

    fp->handle = handle;
    fp->pos    = 0;
    fp->size   = (uint64_t)_zos_syscall1(SYS_GETSIZE, (long)handle);
    fp->eof    = 0;
    fp->error  = 0;
    fp->is_std = 0;
    fp->ungetc_buf = -1;

    return fp;
}

int fclose(FILE *fp) {
    if (fp == NULL || fp->is_std) return -1;
    _zos_syscall1(SYS_CLOSE, (long)fp->handle);
    fp->handle = -1;
    fp->is_std = 0;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (fp == NULL || fp->is_std || size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;
    size_t remaining = 0;
    if (fp->pos < fp->size)
        remaining = (size_t)(fp->size - fp->pos);

    if (total > remaining) total = remaining;
    if (total == 0) { fp->eof = 1; return 0; }

    int bytes = (int)_zos_syscall4(SYS_READ,
        (long)fp->handle, (long)ptr, (long)fp->pos, (long)total);

    if (bytes <= 0) { fp->eof = 1; return 0; }

    fp->pos += (uint64_t)bytes;
    if (fp->pos >= fp->size) fp->eof = 1;

    return (size_t)bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (fp == NULL) return 0;

    /* stdout/stderr: discard (no terminal in windowed mode) */
    if (fp->is_std == 1 || fp->is_std == 2) {
        return nmemb;
    }

    /* Read-only filesystem, no-op */
    return 0;
}

int fseek(FILE *fp, long offset, int whence) {
    if (fp == NULL || fp->is_std) return -1;

    uint64_t newpos;
    switch (whence) {
    case 0: /* SEEK_SET */
        newpos = (uint64_t)offset;
        break;
    case 1: /* SEEK_CUR */
        newpos = fp->pos + (uint64_t)offset;
        break;
    case 2: /* SEEK_END */
        newpos = fp->size + (uint64_t)offset;
        break;
    default:
        return -1;
    }

    fp->pos = newpos;
    fp->eof = 0;
    return 0;
}

long ftell(FILE *fp) {
    if (fp == NULL || fp->is_std) return -1;
    return (long)fp->pos;
}

int fflush(FILE *fp) {
    (void)fp;
    return 0;
}

int feof(FILE *fp) {
    if (fp == NULL) return 1;
    return fp->eof;
}

int ferror(FILE *fp) {
    if (fp == NULL) return 1;
    return fp->error;
}

void clearerr(FILE *fp) {
    if (fp == NULL) return;
    fp->eof = 0;
    fp->error = 0;
}

int fgetc(FILE *fp) {
    if (fp == NULL) return -1;
    if (fp->ungetc_buf >= 0) {
        int c = fp->ungetc_buf;
        fp->ungetc_buf = -1;
        return c;
    }
    unsigned char c;
    size_t n = fread(&c, 1, 1, fp);
    return n == 1 ? (int)c : -1;
}

int getc(FILE *fp) {
    return fgetc(fp);
}

int ungetc(int c, FILE *fp) {
    if (fp == NULL || c == -1) return -1;
    fp->ungetc_buf = c;
    fp->eof = 0;
    return c;
}

char *fgets(char *s, int size, FILE *fp) {
    if (size <= 0) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == -1) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp) {
    (void)s; (void)fp;
    return 0;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    (void)fp; (void)fmt;
    return 0;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    (void)fp; (void)fmt; (void)ap;
    return 0;
}

int sscanf(const char *str, const char *fmt, ...) {
    /* Minimal sscanf: supports %d, %s, %x, %u only */
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    const char *s = str;

    while (*fmt && *s) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd' || *fmt == 'i') {
                int *out = va_arg(ap, int *);
                int neg = 0;
                int val = 0;
                while (isspace((unsigned char)*s)) s++;
                if (*s == '-') { neg = 1; s++; }
                else if (*s == '+') s++;
                if (!isdigit((unsigned char)*s)) break;
                while (isdigit((unsigned char)*s)) { val = val * 10 + (*s - '0'); s++; }
                *out = neg ? -val : val;
                count++;
                fmt++;
            } else if (*fmt == 'u') {
                unsigned int *out = va_arg(ap, unsigned int *);
                unsigned int val = 0;
                while (isspace((unsigned char)*s)) s++;
                if (!isdigit((unsigned char)*s)) break;
                while (isdigit((unsigned char)*s)) { val = val * 10 + (*s - '0'); s++; }
                *out = val;
                count++;
                fmt++;
            } else if (*fmt == 'x' || *fmt == 'X') {
                unsigned int *out = va_arg(ap, unsigned int *);
                unsigned int val = 0;
                while (isspace((unsigned char)*s)) s++;
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
                while (isxdigit((unsigned char)*s)) {
                    int d;
                    if (*s >= '0' && *s <= '9') d = *s - '0';
                    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                    else d = *s - 'A' + 10;
                    val = val * 16 + d;
                    s++;
                }
                *out = val;
                count++;
                fmt++;
            } else if (*fmt == 's') {
                char *out = va_arg(ap, char *);
                while (isspace((unsigned char)*s)) s++;
                while (*s && !isspace((unsigned char)*s)) *out++ = *s++;
                *out = '\0';
                count++;
                fmt++;
            } else {
                break;
            }
        } else if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            while (isspace((unsigned char)*fmt)) fmt++;
        } else {
            if (*s != *fmt) break;
            s++;
            fmt++;
        }
    }

    va_end(ap);
    return count;
}

void perror(const char *s) {
    (void)s;
}

int rename(const char *old, const char *new_) {
    (void)old; (void)new_;
    return -1;
}

int remove(const char *path) {
    (void)path;
    return -1;
}

FILE *tmpfile(void) {
    return NULL;
}

char *tmpnam(char *s) {
    (void)s;
    return NULL;
}

/* ========================================================================
   Stubs for unneeded functions
   ======================================================================== */

int mkdir(const char *path, unsigned int mode) {
    (void)path; (void)mode;
    return -1;
}

/* math.h stubs — doom's fixed-point code doesn't use libm heavily,
   but z_zone.c references some functions. Provide no-ops. */
double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) { return (double)(long)x - (x < (double)(long)x ? 1.0 : 0.0); }
double ceil(double x)  { double f = floor(x); return (x > f) ? f + 1.0 : f; }
double fmod(double x, double y) { if (y == 0.0) return 0.0; return x - (double)((long)(x/y)) * y; }
double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double guess = x;
    for (int i = 0; i < 20; i++)
        guess = (guess + x / guess) * 0.5;
    return guess;
}
double pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (exp == 1.0) return base;
    /* Integer exponent fast path */
    if (exp == (double)(long)exp && exp > 0) {
        double r = 1.0;
        long e = (long)exp;
        while (e-- > 0) r *= base;
        return r;
    }
    return 0.0;
}
double sin(double x)  { (void)x; return 0.0; }
double cos(double x)  { (void)x; return 1.0; }
double atan2(double y, double x) { (void)y; (void)x; return 0.0; }
double log(double x)  { (void)x; return 0.0; }
double exp(double x)  { (void)x; return 1.0; }
double round(double x) { return floor(x + 0.5); }
double atof(const char *s) { (void)s; return 0.0; }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float fabsf(float x)  { return x < 0.0f ? -x : x; }

/* ========================================================================
   Misc stubs that doom might reference
   ======================================================================== */

/* Some doom code calls I_Error which ends up doing fprintf + exit */
/* The signal/system stuff is behind ifdefs, but just in case: */

unsigned int sleep(unsigned int seconds) {
    (void)seconds;
    return 0;
}

int system(const char *command) {
    (void)command;
    return -1;
}
