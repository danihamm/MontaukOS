/*
    * montauk.h
    * MontaukOS C API for user programs (TCC-compatible)
    * Copyright (c) 2026 Daniel Hammer
    *
    * Usage: #include <montauk.h>
    *
    * Provides direct access to all MontaukOS system calls from C.
    * This header is self-contained and does not depend on C++ headers.
*/

#ifndef _MONTAUK_H
#define _MONTAUK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
   Syscall numbers
   ==================================================================== */

#define MTK_SYS_EXIT            0
#define MTK_SYS_YIELD           1
#define MTK_SYS_SLEEP_MS        2
#define MTK_SYS_GETPID          3
#define MTK_SYS_PRINT           4
#define MTK_SYS_PUTCHAR         5
#define MTK_SYS_OPEN            6
#define MTK_SYS_READ            7
#define MTK_SYS_GETSIZE         8
#define MTK_SYS_CLOSE           9
#define MTK_SYS_READDIR         10
#define MTK_SYS_ALLOC           11
#define MTK_SYS_FREE            12
#define MTK_SYS_GETTICKS        13
#define MTK_SYS_GETMILLISECONDS 14
#define MTK_SYS_GETINFO         15
#define MTK_SYS_ISKEYAVAILABLE  16
#define MTK_SYS_GETKEY          17
#define MTK_SYS_GETCHAR         18
#define MTK_SYS_PING            19
#define MTK_SYS_SPAWN           20
#define MTK_SYS_WAITPID         23
#define MTK_SYS_TERMSIZE        24
#define MTK_SYS_GETARGS         25
#define MTK_SYS_RESET           26
#define MTK_SYS_SHUTDOWN        27
#define MTK_SYS_GETTIME         28
#define MTK_SYS_SOCKET          29
#define MTK_SYS_CONNECT         30
#define MTK_SYS_BIND            31
#define MTK_SYS_LISTEN          32
#define MTK_SYS_ACCEPT          33
#define MTK_SYS_SEND            34
#define MTK_SYS_RECV            35
#define MTK_SYS_CLOSESOCK       36
#define MTK_SYS_GETNETCFG       37
#define MTK_SYS_SETNETCFG       38
#define MTK_SYS_SENDTO          39
#define MTK_SYS_RECVFROM        40
#define MTK_SYS_FWRITE          41
#define MTK_SYS_FCREATE         42
#define MTK_SYS_TERMSCALE       43
#define MTK_SYS_RESOLVE         44
#define MTK_SYS_GETRANDOM       45
#define MTK_SYS_KLOG            46
#define MTK_SYS_MOUSESTATE      47
#define MTK_SYS_SETMOUSEBOUNDS  48
#define MTK_SYS_SPAWN_REDIR     49
#define MTK_SYS_CHILDIO_READ    50
#define MTK_SYS_CHILDIO_WRITE   51
#define MTK_SYS_WINCREATE       54
#define MTK_SYS_WINDESTROY      55
#define MTK_SYS_WINPRESENT      56
#define MTK_SYS_WINPOLL         57
#define MTK_SYS_WINENUM         58
#define MTK_SYS_WINMAP          59
#define MTK_SYS_WINUNMAP        97
#define MTK_SYS_WINSENDEVENT    60
#define MTK_SYS_PROCLIST        61
#define MTK_SYS_KILL            62
#define MTK_SYS_WINRESIZE       64
#define MTK_SYS_WINSETSCALE     65
#define MTK_SYS_WINGETSCALE     66
#define MTK_SYS_MEMSTATS        67
#define MTK_SYS_WINSETCURSOR    68
#define MTK_SYS_FDELETE         77
#define MTK_SYS_FMKDIR          78
#define MTK_SYS_FRENAME         94
#define MTK_SYS_DRIVELIST       79
#define MTK_SYS_AUDIOOPEN       80
#define MTK_SYS_AUDIOCLOSE      81
#define MTK_SYS_AUDIOWRITE      82
#define MTK_SYS_AUDIOCTL        83
#define MTK_SYS_SETTZ           90
#define MTK_SYS_GETTZ           91
#define MTK_SYS_GETCWD          95
#define MTK_SYS_CHDIR           96
#define MTK_SYS_DUPHANDLE       98
#define MTK_SYS_WAIT_HANDLE     99
#define MTK_SYS_STREAM_CREATE   100
#define MTK_SYS_STREAM_READ     101
#define MTK_SYS_STREAM_WRITE    102
#define MTK_SYS_MAILBOX_CREATE  103
#define MTK_SYS_MAILBOX_SEND    104
#define MTK_SYS_MAILBOX_RECV    105
#define MTK_SYS_WAITSET_CREATE  106
#define MTK_SYS_WAITSET_ADD     107
#define MTK_SYS_WAITSET_REMOVE  108
#define MTK_SYS_WAITSET_WAIT    109
#define MTK_SYS_PROC_OPEN       110
#define MTK_SYS_SURFACE_CREATE  111
#define MTK_SYS_SURFACE_MAP     112
#define MTK_SYS_SURFACE_RESIZE  113

#define MTK_SOCK_TCP 1
#define MTK_SOCK_UDP 2
#define MTK_IPC_SIGNAL_READABLE    (1u << 0)
#define MTK_IPC_SIGNAL_WRITABLE    (1u << 1)
#define MTK_IPC_SIGNAL_PEER_CLOSED (1u << 2)
#define MTK_IPC_SIGNAL_EXITED      (1u << 3)
#define MTK_IPC_SIGNAL_READY       (1u << 4)

/* Window event types */
#define MTK_EVENT_KEY    0
#define MTK_EVENT_MOUSE  1
#define MTK_EVENT_RESIZE 2
#define MTK_EVENT_CLOSE  3
#define MTK_EVENT_SCALE  4

/* Audio control commands */
#define MTK_AUDIO_SET_VOLUME 0
#define MTK_AUDIO_GET_VOLUME 1
#define MTK_AUDIO_GET_POS    2
#define MTK_AUDIO_PAUSE      3

/* ====================================================================
   Structures
   ==================================================================== */

typedef struct {
    uint8_t scancode;
    char    ascii;
    uint8_t pressed;
    uint8_t shift;
    uint8_t ctrl;
    uint8_t alt;
} mtk_key_event;

typedef struct {
    int32_t  x;
    int32_t  y;
    int32_t  scroll_delta;
    uint8_t  buttons;
} mtk_mouse_state;

typedef struct {
    int32_t  index;
    uint32_t signals;
} mtk_ipc_wait_result;

typedef struct {
    uint8_t type;
    uint8_t _pad[3];
    union {
        mtk_key_event key;
        struct { int32_t x, y, scroll; uint8_t buttons, prev_buttons; } mouse;
        struct { int32_t w, h; } resize;
        struct { int32_t scale; } scale;
    };
} mtk_win_event;

typedef struct {
    int32_t  id;
    uint32_t _pad;
    uint64_t pixels;    /* virtual address of pixel buffer (uint32_t*) */
} mtk_win_result;

typedef struct {
    int32_t  id;
    int32_t  owner_pid;
    char     title[64];
    int32_t  width, height;
    uint8_t  dirty;
    uint8_t  cursor;
    uint8_t  _pad2[2];
} mtk_win_info;

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} mtk_datetime;

typedef struct {
    char     os_name[32];
    char     os_version[32];
    uint32_t api_version;
    uint32_t max_processes;
} mtk_sysinfo;

typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint64_t page_size;
} mtk_memstats;

typedef struct {
    uint32_t ip_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint8_t  mac_address[6];
    uint8_t  _pad[2];
    uint32_t dns_server;
} mtk_netcfg;

typedef struct {
    int32_t  pid;
    int32_t  parent_pid;
    uint8_t  state;     /* 0=Free, 1=Ready, 2=Running, 3=Blocked, 4=Terminated */
    uint8_t  _pad[3];
    char     name[64];
    uint64_t heap_used;
} mtk_procinfo;

/* ====================================================================
   Raw syscall wrappers
   ==================================================================== */

static inline long _mtk_syscall0(long nr) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long _mtk_syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile(
        "mov %[a1], %%rdi\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), [a1] "r"(a1)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

static inline long _mtk_syscall2(long nr, long a1, long a2) {
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

static inline long _mtk_syscall3(long nr, long a1, long a2, long a3) {
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

static inline long _mtk_syscall4(long nr, long a1, long a2, long a3, long a4) {
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

static inline long _mtk_syscall5(long nr, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    __asm__ volatile(
        "mov %[a1], %%rdi\n\t"
        "mov %[a2], %%rsi\n\t"
        "mov %[a3], %%rdx\n\t"
        "mov %[a4], %%r10\n\t"
        "mov %[a5], %%r8\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), [a1] "r"(a1), [a2] "r"(a2), [a3] "r"(a3), [a4] "r"(a4), [a5] "r"(a5)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
    return ret;
}

/* ====================================================================
   Process management
   ==================================================================== */

static inline void mtk_exit(int code) {
    _mtk_syscall1(MTK_SYS_EXIT, (long)code);
}

static inline void mtk_yield(void) {
    _mtk_syscall0(MTK_SYS_YIELD);
}

static inline void mtk_sleep_ms(unsigned long ms) {
    _mtk_syscall1(MTK_SYS_SLEEP_MS, (long)ms);
}

static inline int mtk_getpid(void) {
    return (int)_mtk_syscall0(MTK_SYS_GETPID);
}

static inline int mtk_spawn(const char *path, const char *args) {
    return (int)_mtk_syscall2(MTK_SYS_SPAWN, (long)path, (long)args);
}

static inline void mtk_waitpid(int pid) {
    _mtk_syscall1(MTK_SYS_WAITPID, (long)pid);
}

static inline int mtk_kill(int pid) {
    return (int)_mtk_syscall1(MTK_SYS_KILL, (long)pid);
}

static inline int mtk_proclist(mtk_procinfo *buf, int max) {
    return (int)_mtk_syscall2(MTK_SYS_PROCLIST, (long)buf, (long)max);
}

static inline int mtk_getargs(char *buf, unsigned long max_len) {
    return (int)_mtk_syscall2(MTK_SYS_GETARGS, (long)buf, (long)max_len);
}

static inline int mtk_chdir(const char *path) {
    return (int)_mtk_syscall1(MTK_SYS_CHDIR, (long)path);
}

static inline int mtk_getcwd(char *buf, unsigned long max_len) {
    return (int)_mtk_syscall2(MTK_SYS_GETCWD, (long)buf, (long)max_len);
}

/* ====================================================================
   Console I/O
   ==================================================================== */

static inline void mtk_print(const char *text) {
    _mtk_syscall1(MTK_SYS_PRINT, (long)text);
}

static inline void mtk_putchar(char c) {
    _mtk_syscall1(MTK_SYS_PUTCHAR, (long)c);
}

static inline char mtk_getchar(void) {
    return (char)_mtk_syscall0(MTK_SYS_GETCHAR);
}

static inline int mtk_is_key_available(void) {
    return (int)_mtk_syscall0(MTK_SYS_ISKEYAVAILABLE);
}

static inline void mtk_getkey(mtk_key_event *out) {
    _mtk_syscall1(MTK_SYS_GETKEY, (long)out);
}

/* ====================================================================
   File I/O
   ==================================================================== */

static inline int mtk_open(const char *path) {
    return (int)_mtk_syscall1(MTK_SYS_OPEN, (long)path);
}

static inline int mtk_read(int handle, uint8_t *buf, unsigned long offset, unsigned long size) {
    return (int)_mtk_syscall4(MTK_SYS_READ, (long)handle, (long)buf, (long)offset, (long)size);
}

static inline int mtk_write(int handle, const uint8_t *buf, unsigned long offset, unsigned long size) {
    return (int)_mtk_syscall4(MTK_SYS_FWRITE, (long)handle, (long)buf, (long)offset, (long)size);
}

static inline unsigned long mtk_getsize(int handle) {
    return (unsigned long)_mtk_syscall1(MTK_SYS_GETSIZE, (long)handle);
}

static inline void mtk_close(int handle) {
    _mtk_syscall1(MTK_SYS_CLOSE, (long)handle);
}

static inline int mtk_create(const char *path) {
    return (int)_mtk_syscall1(MTK_SYS_FCREATE, (long)path);
}

static inline int mtk_delete(const char *path) {
    return (int)_mtk_syscall1(MTK_SYS_FDELETE, (long)path);
}

static inline int mtk_mkdir(const char *path) {
    return (int)_mtk_syscall1(MTK_SYS_FMKDIR, (long)path);
}

static inline int mtk_rename(const char *oldpath, const char *newpath) {
    return (int)_mtk_syscall2(MTK_SYS_FRENAME, (long)oldpath, (long)newpath);
}

static inline int mtk_readdir(const char *path, const char **names, int max) {
    return (int)_mtk_syscall3(MTK_SYS_READDIR, (long)path, (long)names, (long)max);
}

/* ====================================================================
   Generic IPC
   ==================================================================== */

static inline int mtk_dup_handle(int handle) {
    return (int)_mtk_syscall1(MTK_SYS_DUPHANDLE, (long)handle);
}

static inline uint32_t mtk_wait_handle(int handle, uint32_t wanted_signals, unsigned long timeout_ms) {
    return (uint32_t)_mtk_syscall3(MTK_SYS_WAIT_HANDLE, (long)handle, (long)wanted_signals, (long)timeout_ms);
}

static inline int mtk_stream_create(int *out_read_handle, int *out_write_handle, unsigned long capacity) {
    return (int)_mtk_syscall3(MTK_SYS_STREAM_CREATE, (long)out_read_handle, (long)out_write_handle, (long)capacity);
}

static inline int mtk_stream_read(int handle, void *buf, int max_len) {
    return (int)_mtk_syscall3(MTK_SYS_STREAM_READ, (long)handle, (long)buf, (long)max_len);
}

static inline int mtk_stream_write(int handle, const void *data, int len) {
    return (int)_mtk_syscall3(MTK_SYS_STREAM_WRITE, (long)handle, (long)data, (long)len);
}

static inline int mtk_mailbox_create(int *out_send_handle, int *out_recv_handle) {
    return (int)_mtk_syscall2(MTK_SYS_MAILBOX_CREATE, (long)out_send_handle, (long)out_recv_handle);
}

static inline int mtk_mailbox_send(int handle, uint32_t msg_type, const void *data,
                                   uint16_t len, int attach_handle) {
    return (int)_mtk_syscall5(MTK_SYS_MAILBOX_SEND, (long)handle, (long)msg_type,
                              (long)data, (long)len, (long)attach_handle);
}

static inline int mtk_mailbox_recv(int handle, uint32_t *out_msg_type, void *data,
                                   uint16_t *in_out_len, int *out_attach_handle) {
    return (int)_mtk_syscall5(MTK_SYS_MAILBOX_RECV, (long)handle, (long)out_msg_type,
                              (long)data, (long)in_out_len, (long)out_attach_handle);
}

static inline int mtk_waitset_create(void) {
    return (int)_mtk_syscall0(MTK_SYS_WAITSET_CREATE);
}

static inline int mtk_waitset_add(int waitset_handle, int target_handle, uint32_t signals) {
    return (int)_mtk_syscall3(MTK_SYS_WAITSET_ADD, (long)waitset_handle, (long)target_handle, (long)signals);
}

static inline int mtk_waitset_remove(int waitset_handle, int index) {
    return (int)_mtk_syscall2(MTK_SYS_WAITSET_REMOVE, (long)waitset_handle, (long)index);
}

static inline int mtk_waitset_wait(int waitset_handle, mtk_ipc_wait_result *out_ready, unsigned long timeout_ms) {
    return (int)_mtk_syscall3(MTK_SYS_WAITSET_WAIT, (long)waitset_handle, (long)out_ready, (long)timeout_ms);
}

static inline int mtk_proc_open(int pid) {
    return (int)_mtk_syscall1(MTK_SYS_PROC_OPEN, (long)pid);
}

static inline int mtk_surface_create(unsigned long byte_size) {
    return (int)_mtk_syscall1(MTK_SYS_SURFACE_CREATE, (long)byte_size);
}

static inline void *mtk_surface_map(int handle) {
    return (void *)_mtk_syscall1(MTK_SYS_SURFACE_MAP, (long)handle);
}

static inline int mtk_surface_resize(int handle, unsigned long new_size) {
    return (int)_mtk_syscall2(MTK_SYS_SURFACE_RESIZE, (long)handle, (long)new_size);
}

/* ====================================================================
   Memory
   ==================================================================== */

static inline void *mtk_alloc_pages(unsigned long bytes) {
    return (void *)_mtk_syscall1(MTK_SYS_ALLOC, (long)bytes);
}

static inline void mtk_free_pages(void *ptr) {
    _mtk_syscall1(MTK_SYS_FREE, (long)ptr);
}

static inline void mtk_get_memstats(mtk_memstats *out) {
    _mtk_syscall1(MTK_SYS_MEMSTATS, (long)out);
}

/* ====================================================================
   Timekeeping
   ==================================================================== */

static inline unsigned long mtk_get_ticks(void) {
    return (unsigned long)_mtk_syscall0(MTK_SYS_GETTICKS);
}

static inline unsigned long mtk_get_ms(void) {
    return (unsigned long)_mtk_syscall0(MTK_SYS_GETMILLISECONDS);
}

static inline void mtk_gettime(mtk_datetime *out) {
    _mtk_syscall1(MTK_SYS_GETTIME, (long)out);
}

static inline void mtk_settz(int offset_minutes) {
    _mtk_syscall1(MTK_SYS_SETTZ, (long)offset_minutes);
}

static inline int mtk_gettz(void) {
    return (int)_mtk_syscall0(MTK_SYS_GETTZ);
}

/* ====================================================================
   System info
   ==================================================================== */

static inline void mtk_get_info(mtk_sysinfo *info) {
    _mtk_syscall1(MTK_SYS_GETINFO, (long)info);
}

static inline long mtk_getrandom(void *buf, unsigned int len) {
    return _mtk_syscall2(MTK_SYS_GETRANDOM, (long)buf, (long)len);
}

/* ====================================================================
   Terminal
   ==================================================================== */

static inline void mtk_termsize(int *cols, int *rows) {
    unsigned long r = (unsigned long)_mtk_syscall0(MTK_SYS_TERMSIZE);
    if (cols) *cols = (int)(r & 0xFFFFFFFF);
    if (rows) *rows = (int)(r >> 32);
}

/* ====================================================================
   Window server
   ==================================================================== */

static inline int mtk_win_create(const char *title, int w, int h, mtk_win_result *result) {
    return (int)_mtk_syscall4(MTK_SYS_WINCREATE, (long)title, (long)w, (long)h, (long)result);
}

static inline int mtk_win_destroy(int id) {
    return (int)_mtk_syscall1(MTK_SYS_WINDESTROY, (long)id);
}

static inline unsigned long mtk_win_present(int id) {
    return (unsigned long)_mtk_syscall1(MTK_SYS_WINPRESENT, (long)id);
}

static inline int mtk_win_poll(int id, mtk_win_event *event) {
    return (int)_mtk_syscall2(MTK_SYS_WINPOLL, (long)id, (long)event);
}

static inline int mtk_win_unmap(int id) {
    return (int)_mtk_syscall1(MTK_SYS_WINUNMAP, (long)id);
}

static inline unsigned long mtk_win_resize(int id, int w, int h) {
    return (unsigned long)_mtk_syscall3(MTK_SYS_WINRESIZE, (long)id, (long)w, (long)h);
}

static inline int mtk_win_enumerate(mtk_win_info *info, int max) {
    return (int)_mtk_syscall2(MTK_SYS_WINENUM, (long)info, (long)max);
}

static inline unsigned long mtk_win_map(int id) {
    return (unsigned long)_mtk_syscall1(MTK_SYS_WINMAP, (long)id);
}

static inline int mtk_win_setcursor(int id, int cursor) {
    return (int)_mtk_syscall2(MTK_SYS_WINSETCURSOR, (long)id, (long)cursor);
}

static inline int mtk_win_setscale(int scale) {
    return (int)_mtk_syscall1(MTK_SYS_WINSETSCALE, (long)scale);
}

static inline int mtk_win_getscale(void) {
    return (int)_mtk_syscall0(MTK_SYS_WINGETSCALE);
}

/* ====================================================================
   Mouse
   ==================================================================== */

static inline void mtk_get_mouse(mtk_mouse_state *out) {
    _mtk_syscall1(MTK_SYS_MOUSESTATE, (long)out);
}

/* ====================================================================
   Networking
   ==================================================================== */

static inline int mtk_socket(int type) {
    return (int)_mtk_syscall1(MTK_SYS_SOCKET, (long)type);
}

static inline int mtk_connect(int fd, uint32_t ip, uint16_t port) {
    return (int)_mtk_syscall3(MTK_SYS_CONNECT, (long)fd, (long)ip, (long)port);
}

static inline int mtk_bind(int fd, uint16_t port) {
    return (int)_mtk_syscall2(MTK_SYS_BIND, (long)fd, (long)port);
}

static inline int mtk_listen(int fd) {
    return (int)_mtk_syscall1(MTK_SYS_LISTEN, (long)fd);
}

static inline int mtk_accept(int fd) {
    return (int)_mtk_syscall1(MTK_SYS_ACCEPT, (long)fd);
}

static inline int mtk_send(int fd, const void *data, uint32_t len) {
    return (int)_mtk_syscall3(MTK_SYS_SEND, (long)fd, (long)data, (long)len);
}

static inline int mtk_recv(int fd, void *buf, uint32_t max_len) {
    return (int)_mtk_syscall3(MTK_SYS_RECV, (long)fd, (long)buf, (long)max_len);
}

static inline int mtk_closesocket(int fd) {
    return (int)_mtk_syscall1(MTK_SYS_CLOSESOCK, (long)fd);
}

static inline uint32_t mtk_resolve(const char *hostname) {
    return (uint32_t)_mtk_syscall1(MTK_SYS_RESOLVE, (long)hostname);
}

static inline int mtk_ping(uint32_t ip, uint32_t timeout_ms) {
    return (int)_mtk_syscall2(MTK_SYS_PING, (long)ip, (long)timeout_ms);
}

static inline void mtk_get_netcfg(mtk_netcfg *out) {
    _mtk_syscall1(MTK_SYS_GETNETCFG, (long)out);
}

/* ====================================================================
   Audio
   ==================================================================== */

static inline int mtk_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    return (int)_mtk_syscall3(MTK_SYS_AUDIOOPEN, (long)sample_rate, (long)channels, (long)bits);
}

static inline void mtk_audio_close(int handle) {
    _mtk_syscall1(MTK_SYS_AUDIOCLOSE, (long)handle);
}

static inline int mtk_audio_write(int handle, const void *data, uint32_t size) {
    return (int)_mtk_syscall3(MTK_SYS_AUDIOWRITE, (long)handle, (long)data, (long)size);
}

static inline int mtk_audio_ctl(int handle, int cmd, int value) {
    return (int)_mtk_syscall3(MTK_SYS_AUDIOCTL, (long)handle, (long)cmd, (long)value);
}

/* ====================================================================
   Power management
   ==================================================================== */

static inline void mtk_reset(void) {
    _mtk_syscall0(MTK_SYS_RESET);
}

static inline void mtk_shutdown(void) {
    _mtk_syscall0(MTK_SYS_SHUTDOWN);
}

/* ====================================================================
   Pixel helpers (for window server apps)
   ==================================================================== */

static inline uint32_t mtk_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t mtk_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

#ifdef __cplusplus
}
#endif

#endif /* _MONTAUK_H */
