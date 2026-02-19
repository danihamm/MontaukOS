/*
    * syscall.h
    * ZenithOS program-side syscall wrappers using SYSCALL instruction
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <Api/Syscall.hpp>

namespace zenith {

    // ---- Raw SYSCALL wrappers ----

    // The SYSCALL handler does not restore RDI, RSI, RDX, R10, R8, R9
    // (they are skipped on the return path).  We move arguments into the
    // correct registers inside the asm block and list ALL argument
    // registers in the clobber list.  This guarantees the compiler
    // reloads every argument on each call — GCC cannot optimise away
    // clobbers, unlike "+r" outputs whose dead values it may discard.

    inline int64_t syscall0(uint64_t nr) {
        int64_t ret;
        asm volatile("syscall" : "=a"(ret) : "a"(nr)
            : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
        return ret;
    }

    inline int64_t syscall1(uint64_t nr, uint64_t a1) {
        int64_t ret;
        asm volatile(
            "mov %[a1], %%rdi\n\t"
            "syscall"
            : "=a"(ret)
            : "a"(nr), [a1] "r"(a1)
            : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
        return ret;
    }

    inline int64_t syscall2(uint64_t nr, uint64_t a1, uint64_t a2) {
        int64_t ret;
        asm volatile(
            "mov %[a1], %%rdi\n\t"
            "mov %[a2], %%rsi\n\t"
            "syscall"
            : "=a"(ret)
            : "a"(nr), [a1] "r"(a1), [a2] "r"(a2)
            : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
        return ret;
    }

    inline int64_t syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
        int64_t ret;
        asm volatile(
            "mov %[a1], %%rdi\n\t"
            "mov %[a2], %%rsi\n\t"
            "mov %[a3], %%rdx\n\t"
            "syscall"
            : "=a"(ret)
            : "a"(nr), [a1] "r"(a1), [a2] "r"(a2), [a3] "r"(a3)
            : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
        return ret;
    }

    inline int64_t syscall4(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
        int64_t ret;
        asm volatile(
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

    inline int64_t syscall5(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
        int64_t ret;
        asm volatile(
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

    inline int64_t syscall6(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
        int64_t ret;
        asm volatile(
            "mov %[a1], %%rdi\n\t"
            "mov %[a2], %%rsi\n\t"
            "mov %[a3], %%rdx\n\t"
            "mov %[a4], %%r10\n\t"
            "mov %[a5], %%r8\n\t"
            "mov %[a6], %%r9\n\t"
            "syscall"
            : "=a"(ret)
            : "a"(nr), [a1] "r"(a1), [a2] "r"(a2), [a3] "r"(a3), [a4] "r"(a4), [a5] "r"(a5), [a6] "r"(a6)
            : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
        return ret;
    }

    // ---- Typed wrappers ----

    // Process
    [[noreturn]] inline void exit(int code = 0) {
        syscall1(Zenith::SYS_EXIT, (uint64_t)code);
        __builtin_unreachable();
    }

    inline void yield() { syscall0(Zenith::SYS_YIELD); }
    inline void sleep_ms(uint64_t ms) { syscall1(Zenith::SYS_SLEEP_MS, ms); }
    inline int getpid() { return (int)syscall0(Zenith::SYS_GETPID); }
    inline int spawn(const char* path, const char* args = nullptr) {
        return (int)syscall2(Zenith::SYS_SPAWN, (uint64_t)path, (uint64_t)args);
    }

    // Console
    inline void print(const char* text) { syscall1(Zenith::SYS_PRINT, (uint64_t)text); }
    inline void putchar(char c) { syscall1(Zenith::SYS_PUTCHAR, (uint64_t)c); }

    // File I/O
    inline int open(const char* path) { return (int)syscall1(Zenith::SYS_OPEN, (uint64_t)path); }
    inline int read(int handle, uint8_t* buf, uint64_t off, uint64_t size) {
        return (int)syscall4(Zenith::SYS_READ, (uint64_t)handle, (uint64_t)buf, off, size);
    }
    inline uint64_t getsize(int handle) { return (uint64_t)syscall1(Zenith::SYS_GETSIZE, (uint64_t)handle); }
    inline void close(int handle) { syscall1(Zenith::SYS_CLOSE, (uint64_t)handle); }
    inline int readdir(const char* path, const char** names, int max) {
        return (int)syscall3(Zenith::SYS_READDIR, (uint64_t)path, (uint64_t)names, (uint64_t)max);
    }

    // File write/create
    inline int fwrite(int handle, const uint8_t* buf, uint64_t off, uint64_t size) {
        return (int)syscall4(Zenith::SYS_FWRITE, (uint64_t)handle, (uint64_t)buf, off, size);
    }
    inline int fcreate(const char* path) {
        return (int)syscall1(Zenith::SYS_FCREATE, (uint64_t)path);
    }

    // Memory
    inline void* alloc(uint64_t size) { return (void*)syscall1(Zenith::SYS_ALLOC, size); }
    inline void free(void* ptr) { syscall1(Zenith::SYS_FREE, (uint64_t)ptr); }

    // Timekeeping
    inline uint64_t get_ticks() { return (uint64_t)syscall0(Zenith::SYS_GETTICKS); }
    inline uint64_t get_milliseconds() { return (uint64_t)syscall0(Zenith::SYS_GETMILLISECONDS); }

    // System
    inline void get_info(Zenith::SysInfo* info) { syscall1(Zenith::SYS_GETINFO, (uint64_t)info); }

    // Keyboard
    inline bool is_key_available() { return (bool)syscall0(Zenith::SYS_ISKEYAVAILABLE); }
    inline void getkey(Zenith::KeyEvent* out) { syscall1(Zenith::SYS_GETKEY, (uint64_t)out); }
    inline char getchar() { return (char)syscall0(Zenith::SYS_GETCHAR); }

    // Networking
    inline int32_t ping(uint32_t ip, uint32_t timeoutMs = 3000) {
        return (int32_t)syscall2(Zenith::SYS_PING, (uint64_t)ip, (uint64_t)timeoutMs);
    }

    // Network configuration
    inline void get_netcfg(Zenith::NetCfg* out) { syscall1(Zenith::SYS_GETNETCFG, (uint64_t)out); }
    inline int set_netcfg(const Zenith::NetCfg* cfg) { return (int)syscall1(Zenith::SYS_SETNETCFG, (uint64_t)cfg); }

    // Sockets
    inline int socket(int type) {
        return (int)syscall1(Zenith::SYS_SOCKET, (uint64_t)type);
    }
    inline int connect(int fd, uint32_t ip, uint16_t port) {
        return (int)syscall3(Zenith::SYS_CONNECT, (uint64_t)fd, (uint64_t)ip, (uint64_t)port);
    }
    inline int bind(int fd, uint16_t port) {
        return (int)syscall2(Zenith::SYS_BIND, (uint64_t)fd, (uint64_t)port);
    }
    inline int listen(int fd) {
        return (int)syscall1(Zenith::SYS_LISTEN, (uint64_t)fd);
    }
    inline int accept(int fd) {
        return (int)syscall1(Zenith::SYS_ACCEPT, (uint64_t)fd);
    }
    inline int send(int fd, const void* data, uint32_t len) {
        return (int)syscall3(Zenith::SYS_SEND, (uint64_t)fd, (uint64_t)data, (uint64_t)len);
    }
    inline int recv(int fd, void* buf, uint32_t maxLen) {
        return (int)syscall3(Zenith::SYS_RECV, (uint64_t)fd, (uint64_t)buf, (uint64_t)maxLen);
    }
    inline int closesocket(int fd) {
        return (int)syscall1(Zenith::SYS_CLOSESOCK, (uint64_t)fd);
    }
    inline int sendto(int fd, const void* data, uint32_t len, uint32_t destIp, uint16_t destPort) {
        return (int)syscall5(Zenith::SYS_SENDTO, (uint64_t)fd, (uint64_t)data,
                             (uint64_t)len, (uint64_t)destIp, (uint64_t)destPort);
    }
    inline int recvfrom(int fd, void* buf, uint32_t maxLen, uint32_t* srcIp, uint16_t* srcPort) {
        return (int)syscall5(Zenith::SYS_RECVFROM, (uint64_t)fd, (uint64_t)buf,
                             (uint64_t)maxLen, (uint64_t)srcIp, (uint64_t)srcPort);
    }

    // Process management
    inline void waitpid(int pid) { syscall1(Zenith::SYS_WAITPID, (uint64_t)pid); }

    // Framebuffer
    inline void fb_info(Zenith::FbInfo* info) { syscall1(Zenith::SYS_FBINFO, (uint64_t)info); }
    inline void* fb_map() { return (void*)syscall0(Zenith::SYS_FBMAP); }

    // Arguments
    inline int getargs(char* buf, uint64_t maxLen) {
        return (int)syscall2(Zenith::SYS_GETARGS, (uint64_t)buf, maxLen);
    }

    // Terminal
    inline void termsize(int* cols, int* rows) {
        uint64_t r = (uint64_t)syscall0(Zenith::SYS_TERMSIZE);
        if (cols) *cols = (int)(r & 0xFFFFFFFF);
        if (rows) *rows = (int)(r >> 32);
    }

    // Timekeeping (wall-clock)
    inline void gettime(Zenith::DateTime* out) { syscall1(Zenith::SYS_GETTIME, (uint64_t)out); }

    // Power management
    [[noreturn]] inline void reset() {
        syscall0(Zenith::SYS_RESET);
        __builtin_unreachable();
    }

    [[noreturn]] inline void shutdown() {
        syscall0(Zenith::SYS_SHUTDOWN);
        __builtin_unreachable();
    }

}
