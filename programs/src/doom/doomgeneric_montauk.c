/*
    * doomgeneric_montauk.c
    * DOOM platform implementation for MontaukOS (standalone window server client)
    * Copyright (c) 2025 Daniel Hammer
*/

#include "doomgeneric.h"
#include "doomkeys.h"

#include <string.h>
#include <stdio.h>

/* ---- Raw syscall interface (C versions) ---- */

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

/* Syscall numbers (must match kernel/src/Api/Syscall.hpp) */
#define SYS_EXIT            0
#define SYS_SLEEP_MS        2
#define SYS_PRINT           4
#define SYS_GETMILLISECONDS 14
#define SYS_WINCREATE       54
#define SYS_WINDESTROY      55
#define SYS_WINPRESENT      56
#define SYS_WINPOLL         57

/* Window server structs (must match Montauk::WinCreateResult and Montauk::WinEvent) */
struct WinCreateResult {
    int      id;        /* -1 on failure */
    unsigned _pad;
    unsigned long pixelVa;  /* VA of pixel buffer in caller's address space */
};

struct WinEvent {
    unsigned char type;     /* 0=key, 1=mouse, 2=resize, 3=close */
    unsigned char _pad[3];
    union {
        struct {
            unsigned char scancode;
            char          ascii;
            unsigned char pressed;
            unsigned char shift;
            unsigned char ctrl;
            unsigned char alt;
        } key;
        struct { int x, y, scroll; unsigned char buttons, prev_buttons; } mouse;
        struct { int w, h; } resize;
    };
};

/* ---- Window state ---- */

static int       g_winId   = -1;
static uint32_t* g_pixBuf  = 0;

/* ---- Circular key queue ---- */

#define KEY_QUEUE_SIZE 64

struct KeyQueueEntry {
    int pressed;
    unsigned char doomkey;
};

static struct KeyQueueEntry g_keyQueue[KEY_QUEUE_SIZE];
static int g_keyQueueRead  = 0;
static int g_keyQueueWrite = 0;

static void key_queue_push(int pressed, unsigned char doomkey) {
    int next = (g_keyQueueWrite + 1) % KEY_QUEUE_SIZE;
    if (next == g_keyQueueRead) return; /* full, drop */
    g_keyQueue[g_keyQueueWrite].pressed = pressed;
    g_keyQueue[g_keyQueueWrite].doomkey = doomkey;
    g_keyQueueWrite = next;
}

static int key_queue_pop(int* pressed, unsigned char* doomkey) {
    if (g_keyQueueRead == g_keyQueueWrite) return 0;
    *pressed = g_keyQueue[g_keyQueueRead].pressed;
    *doomkey = g_keyQueue[g_keyQueueRead].doomkey;
    g_keyQueueRead = (g_keyQueueRead + 1) % KEY_QUEUE_SIZE;
    return 1;
}

/* ---- PS/2 scancode to ASCII table (set 1, unshifted) ---- */

static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' '
};

/* ---- PS/2 scancode to DOOM key mapping ---- */

static unsigned char scancode_to_doomkey(unsigned char scancode, char ascii) {
    switch (scancode) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1C: return KEY_ENTER;
        case 0x01: return KEY_ESCAPE;
        case 0x39: return KEY_USE;      /* Space = use */
        case 0x1D: return KEY_FIRE;     /* LCtrl = fire */
        case 0x2A: return KEY_RSHIFT;   /* LShift = run */
        case 0x36: return KEY_RSHIFT;   /* RShift = run */
        case 0x38: return KEY_RALT;     /* Alt = strafe */
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        /* F1-F10 */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        /* Equals and minus for screen size */
        case 0x0D: return KEY_EQUALS;
        case 0x0C: return KEY_MINUS;
        default:
            /* Pass through printable ASCII as lowercase */
            if (ascii >= 'a' && ascii <= 'z') return (unsigned char)ascii;
            if (ascii >= '0' && ascii <= '9') return (unsigned char)ascii;
            return 0;
    }
}

/* ---- Poll window events and enqueue key events ---- */

static void poll_keyboard(void) {
    struct WinEvent evt;
    while (_zos_syscall2(SYS_WINPOLL, (long)g_winId, (long)&evt) > 0) {
        if (evt.type == 0) {
            /* Key event */
            unsigned char baseSc = evt.key.scancode & 0x7F;

            char ascii = 0;
            if (baseSc < 128)
                ascii = scancode_to_ascii[baseSc];

            unsigned char dk = scancode_to_doomkey(baseSc, ascii);
            if (dk != 0) {
                key_queue_push(evt.key.pressed ? 1 : 0, dk);
            }
        } else if (evt.type == 3) {
            /* Close event — exit the process */
            _zos_syscall1(SYS_EXIT, 0);
        }
    }
}

/* ---- DG platform functions ---- */

void DG_Init(void) {
    struct WinCreateResult result;
    _zos_syscall4(SYS_WINCREATE, (long)"DOOM", (long)DOOMGENERIC_RESX,
                  (long)DOOMGENERIC_RESY, (long)&result);

    if (result.id < 0) {
        _zos_syscall1(SYS_EXIT, 1);
    }

    g_winId  = result.id;
    g_pixBuf = (uint32_t*)result.pixelVa;
}

void DG_DrawFrame(void) {
    /* Poll keyboard first */
    poll_keyboard();

    /* Copy DG_ScreenBuffer into the shared pixel buffer */
    if (g_pixBuf == 0 || DG_ScreenBuffer == 0) return;

    memcpy(g_pixBuf, DG_ScreenBuffer,
           DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t));

    /* Mark window as dirty so the compositor picks it up */
    _zos_syscall1(SYS_WINPRESENT, (long)g_winId);
}

void DG_SleepMs(uint32_t ms) {
    _zos_syscall1(SYS_SLEEP_MS, (long)ms);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)_zos_syscall0(SYS_GETMILLISECONDS);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    return key_queue_pop(pressed, doomKey);
}

void DG_SetWindowTitle(const char* title) {
    (void)title;
}

/* ---- Entry point ---- */

void _start(void) {
    char *argv[] = { "doom", "-iwad", "0:/games/doom1.wad", 0 };
    doomgeneric_Create(3, argv);
    for (;;) {
        doomgeneric_Tick();
    }
}
