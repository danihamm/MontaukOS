/*
    * doomgeneric_zenith.c
    * DOOM platform implementation for ZenithOS
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

/* Syscall numbers (must match kernel/src/Api/Syscall.hpp) */
#define SYS_EXIT            0
#define SYS_SLEEP_MS        2
#define SYS_PRINT           4
#define SYS_GETMILLISECONDS 14
#define SYS_ISKEYAVAILABLE  16
#define SYS_GETKEY          17
#define SYS_FBINFO          21
#define SYS_FBMAP           22

/* FbInfo struct (must match kernel definition) */
struct FbInfo {
    unsigned long width;
    unsigned long height;
    unsigned long pitch;
    unsigned long bpp;
    unsigned long userAddr;
};

/* KeyEvent struct (must match kernel definition) */
struct KeyEvent {
    unsigned char scancode;
    char          ascii;
    unsigned char pressed;
    unsigned char shift;
    unsigned char ctrl;
    unsigned char alt;
};

/* ---- Framebuffer state ---- */

static uint32_t* g_fbPtr    = 0;
static uint32_t  g_fbWidth  = 0;
static uint32_t  g_fbHeight = 0;
static uint32_t  g_fbPitch  = 0; /* bytes per scanline */

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
        case 0x39: return ' ';          /* Space = use */
        case 0x1D: return KEY_RCTRL;    /* LCtrl = fire */
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

/* ---- Poll keyboard and enqueue events ---- */

static void poll_keyboard(void) {
    while (_zos_syscall0(SYS_ISKEYAVAILABLE)) {
        struct KeyEvent evt;
        _zos_syscall1(SYS_GETKEY, (long)&evt);

        unsigned char baseSc = evt.scancode & 0x7F; /* strip break bit */

        char ascii = 0;
        if (baseSc < 128)
            ascii = scancode_to_ascii[baseSc];

        unsigned char dk = scancode_to_doomkey(baseSc, ascii);
        if (dk != 0) {
            key_queue_push(evt.pressed ? 1 : 0, dk);
        }
    }
}

/* ---- DG platform functions ---- */

void DG_Init(void) {
    struct FbInfo info;
    _zos_syscall1(SYS_FBINFO, (long)&info);

    g_fbWidth  = (uint32_t)info.width;
    g_fbHeight = (uint32_t)info.height;
    g_fbPitch  = (uint32_t)info.pitch;

    g_fbPtr = (uint32_t*)(unsigned long)_zos_syscall0(SYS_FBMAP);

    printf("DOOM: framebuffer %ux%u pitch=%u mapped at %p\n",
           g_fbWidth, g_fbHeight, g_fbPitch, (void*)g_fbPtr);
}

void DG_DrawFrame(void) {
    /* Poll keyboard first */
    poll_keyboard();

    /* Copy DG_ScreenBuffer (DOOMGENERIC_RESX x DOOMGENERIC_RESY) to framebuffer */
    if (g_fbPtr == 0 || DG_ScreenBuffer == 0) return;

    uint32_t copyW = DOOMGENERIC_RESX;
    uint32_t copyH = DOOMGENERIC_RESY;
    if (copyW > g_fbWidth)  copyW = g_fbWidth;
    if (copyH > g_fbHeight) copyH = g_fbHeight;

    uint32_t fbStride = g_fbPitch / 4; /* pixels per scanline */

    for (uint32_t y = 0; y < copyH; y++) {
        uint32_t* dst = g_fbPtr + y * fbStride;
        uint32_t* src = DG_ScreenBuffer + y * DOOMGENERIC_RESX;
        memcpy(dst, src, copyW * sizeof(uint32_t));
    }
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
