/*
    * apps_common.hpp
    * Shared inline utilities and forward declarations for desktop apps
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/framebuffer.hpp>
#include <gui/font.hpp>
#include <gui/draw.hpp>
#include <gui/svg.hpp>
#include <gui/widgets.hpp>
#include <gui/window.hpp>
#include <gui/canvas.hpp>
#include <gui/terminal.hpp>
#include <gui/desktop.hpp>

// Placement new for freestanding environment
inline void* operator new(unsigned long, void* p) { return p; }

using namespace gui;

// ============================================================================
// Minimal snprintf
// ============================================================================

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState { char* buf; int pos; int max; };

inline void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

inline void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24]; int i = 0;
    const char* digits = "0123456789abcdef";
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    int total = (neg ? 1 : 0) + i;
    if (neg && pad == '0') pf_putc(st, '-');
    for (int w = total; w < width; w++) pf_putc(st, pad);
    if (neg && pad != '0') pf_putc(st, '-');
    while (i > 0) pf_putc(st, tmp[--i]);
}

inline int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    PfState st; st.buf = buf; st.pos = 0; st.max = size > 0 ? size - 1 : 0;
    while (*fmt) {
        if (*fmt != '%') { pf_putc(&st, *fmt++); continue; }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            long val = va_arg(ap, int);
            int neg = 0; unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); } else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg); break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            for (int w = slen; w < width; w++) pf_putc(&st, ' ');
            for (int j = 0; j < slen; j++) pf_putc(&st, s[j]);
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); pf_putc(&st, c); break; }
        case '%': pf_putc(&st, '%'); break;
        default: pf_putc(&st, '%'); pf_putc(&st, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (size > 0) { if (st.pos < size) st.buf[st.pos] = '\0'; else st.buf[size - 1] = '\0'; }
    va_end(ap); return st.pos;
}

// ============================================================================
// String helpers
// ============================================================================

inline void str_append(char* dst, const char* src, int max) {
    int len = montauk::slen(dst);
    int i = 0;
    while (src[i] && len < max - 1) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

inline int str_compare_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

// ============================================================================
// Network formatting helpers
// ============================================================================

inline void format_ip(char* buf, uint32_t ip) {
    snprintf(buf, 20, "%d.%d.%d.%d",
        (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
        (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
}

inline void format_mac(char* buf, const uint8_t* mac) {
    snprintf(buf, 20, "%02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
        (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

// ============================================================================
// File size formatting
// ============================================================================

inline void format_size(char* buf, int size) {
    if (size < 1024) {
        snprintf(buf, 16, "%d B", size);
    } else if (size < 1024 * 1024) {
        int kb = size / 1024;
        int frac = ((size % 1024) * 10) / 1024;
        if (kb < 10) {
            snprintf(buf, 16, "%d.%d KB", kb, frac);
        } else {
            snprintf(buf, 16, "%d KB", kb);
        }
    } else {
        int mb = size / (1024 * 1024);
        int frac = ((size % (1024 * 1024)) * 10) / (1024 * 1024);
        if (mb < 10) {
            snprintf(buf, 16, "%d.%d MB", mb, frac);
        } else {
            snprintf(buf, 16, "%d MB", mb);
        }
    }
}

// ============================================================================
// Forward declarations for app launchers
// ============================================================================

void open_terminal(DesktopState* ds);
void open_filemanager(DesktopState* ds);
void open_sysinfo(DesktopState* ds);
void open_calculator(DesktopState* ds);
void open_texteditor(DesktopState* ds);
void open_texteditor_with_file(DesktopState* ds, const char* path);
void open_klog(DesktopState* ds);
void open_wiki(DesktopState* ds);
void open_weather(DesktopState* ds);
void open_procmgr(DesktopState* ds);
void open_mandelbrot(DesktopState* ds);
void open_devexplorer(DesktopState* ds);
void open_settings(DesktopState* ds);
void open_doom(DesktopState* ds);
void open_reboot_dialog(DesktopState* ds);
void open_shutdown_dialog(DesktopState* ds);
void desktop_poll_external_windows(DesktopState* ds);
