/*
    * syscall.h
    * MontaukOS program-side syscall wrappers using SYSCALL instruction
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <Api/Syscall.hpp>

namespace montauk {

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
        syscall1(Montauk::SYS_EXIT, (uint64_t)code);
        __builtin_unreachable();
    }

    inline void yield() { syscall0(Montauk::SYS_YIELD); }
    inline void sleep_ms(uint64_t ms) { syscall1(Montauk::SYS_SLEEP_MS, ms); }
    inline int getpid() { return (int)syscall0(Montauk::SYS_GETPID); }
    inline int spawn(const char* path, const char* args = nullptr) {
        return (int)syscall2(Montauk::SYS_SPAWN, (uint64_t)path, (uint64_t)args);
    }
    inline int chdir(const char* path) {
        return (int)syscall1(Montauk::SYS_CHDIR, (uint64_t)path);
    }
    inline int getcwd(char* buf, uint64_t maxLen) {
        return (int)syscall2(Montauk::SYS_GETCWD, (uint64_t)buf, maxLen);
    }

    // Console
    inline void print(const char* text) { syscall1(Montauk::SYS_PRINT, (uint64_t)text); }
    inline void putchar(char c) { syscall1(Montauk::SYS_PUTCHAR, (uint64_t)c); }

    // File I/O
    inline int open(const char* path) { return (int)syscall1(Montauk::SYS_OPEN, (uint64_t)path); }
    inline int read(int handle, uint8_t* buf, uint64_t off, uint64_t size) {
        return (int)syscall4(Montauk::SYS_READ, (uint64_t)handle, (uint64_t)buf, off, size);
    }
    inline uint64_t getsize(int handle) { return (uint64_t)syscall1(Montauk::SYS_GETSIZE, (uint64_t)handle); }
    inline void close(int handle) { syscall1(Montauk::SYS_CLOSE, (uint64_t)handle); }
    inline int readdir(const char* path, const char** names, int max) {
        return (int)syscall3(Montauk::SYS_READDIR, (uint64_t)path, (uint64_t)names, (uint64_t)max);
    }

    // File write/create
    inline int fwrite(int handle, const uint8_t* buf, uint64_t off, uint64_t size) {
        return (int)syscall4(Montauk::SYS_FWRITE, (uint64_t)handle, (uint64_t)buf, off, size);
    }
    inline int fcreate(const char* path) {
        return (int)syscall1(Montauk::SYS_FCREATE, (uint64_t)path);
    }
    inline int fdelete(const char* path) {
        return (int)syscall1(Montauk::SYS_FDELETE, (uint64_t)path);
    }
    inline int fmkdir(const char* path) {
        return (int)syscall1(Montauk::SYS_FMKDIR, (uint64_t)path);
    }
    inline int frename(const char* oldPath, const char* newPath) {
        return (int)syscall2(Montauk::SYS_FRENAME, (uint64_t)oldPath, (uint64_t)newPath);
    }
    inline int drivelist(int* outDrives, int max) {
        return (int)syscall2(Montauk::SYS_DRIVELIST, (uint64_t)outDrives, (uint64_t)max);
    }

    // Memory
    inline void* alloc(uint64_t size) { return (void*)syscall1(Montauk::SYS_ALLOC, size); }
    inline void free(void* ptr) { syscall1(Montauk::SYS_FREE, (uint64_t)ptr); }

    // Timekeeping
    inline uint64_t get_ticks() { return (uint64_t)syscall0(Montauk::SYS_GETTICKS); }
    inline uint64_t get_milliseconds() { return (uint64_t)syscall0(Montauk::SYS_GETMILLISECONDS); }

    // System
    inline void get_info(Montauk::SysInfo* info) { syscall1(Montauk::SYS_GETINFO, (uint64_t)info); }

    // Keyboard
    inline bool is_key_available() { return (bool)syscall0(Montauk::SYS_ISKEYAVAILABLE); }
    inline void getkey(Montauk::KeyEvent* out) { syscall1(Montauk::SYS_GETKEY, (uint64_t)out); }
    inline char getchar() { return (char)syscall0(Montauk::SYS_GETCHAR); }

    // Networking
    inline int32_t ping(uint32_t ip, uint32_t timeoutMs = 3000) {
        return (int32_t)syscall2(Montauk::SYS_PING, (uint64_t)ip, (uint64_t)timeoutMs);
    }

    // DNS resolve: returns IP in network byte order, or 0 on failure
    inline uint32_t resolve(const char* hostname) {
        return (uint32_t)syscall1(Montauk::SYS_RESOLVE, (uint64_t)hostname);
    }

    // Network configuration
    inline void get_netcfg(Montauk::NetCfg* out) { syscall1(Montauk::SYS_GETNETCFG, (uint64_t)out); }
    inline int set_netcfg(const Montauk::NetCfg* cfg) { return (int)syscall1(Montauk::SYS_SETNETCFG, (uint64_t)cfg); }

    // Sockets
    inline int socket(int type) {
        return (int)syscall1(Montauk::SYS_SOCKET, (uint64_t)type);
    }
    inline int connect(int fd, uint32_t ip, uint16_t port) {
        return (int)syscall3(Montauk::SYS_CONNECT, (uint64_t)fd, (uint64_t)ip, (uint64_t)port);
    }
    inline int bind(int fd, uint16_t port) {
        return (int)syscall2(Montauk::SYS_BIND, (uint64_t)fd, (uint64_t)port);
    }
    inline int listen(int fd) {
        return (int)syscall1(Montauk::SYS_LISTEN, (uint64_t)fd);
    }
    inline int accept(int fd) {
        return (int)syscall1(Montauk::SYS_ACCEPT, (uint64_t)fd);
    }
    inline int send(int fd, const void* data, uint32_t len) {
        return (int)syscall3(Montauk::SYS_SEND, (uint64_t)fd, (uint64_t)data, (uint64_t)len);
    }
    inline int recv(int fd, void* buf, uint32_t maxLen) {
        return (int)syscall3(Montauk::SYS_RECV, (uint64_t)fd, (uint64_t)buf, (uint64_t)maxLen);
    }
    inline int closesocket(int fd) {
        return (int)syscall1(Montauk::SYS_CLOSESOCK, (uint64_t)fd);
    }
    inline int sendto(int fd, const void* data, uint32_t len, uint32_t destIp, uint16_t destPort) {
        return (int)syscall5(Montauk::SYS_SENDTO, (uint64_t)fd, (uint64_t)data,
                             (uint64_t)len, (uint64_t)destIp, (uint64_t)destPort);
    }
    inline int recvfrom(int fd, void* buf, uint32_t maxLen, uint32_t* srcIp, uint16_t* srcPort) {
        return (int)syscall5(Montauk::SYS_RECVFROM, (uint64_t)fd, (uint64_t)buf,
                             (uint64_t)maxLen, (uint64_t)srcIp, (uint64_t)srcPort);
    }

    // Generic IPC
    inline int dup_handle(int handle) {
        return (int)syscall1(Montauk::SYS_DUPHANDLE, (uint64_t)handle);
    }
    inline uint32_t wait_handle(int handle, uint32_t wantedSignals, uint64_t timeoutMs = ~0ULL) {
        return (uint32_t)syscall3(Montauk::SYS_WAIT_HANDLE, (uint64_t)handle,
                                  (uint64_t)wantedSignals, timeoutMs);
    }
    inline int stream_create(int* outReadHandle, int* outWriteHandle, uint32_t capacity = 0) {
        return (int)syscall3(Montauk::SYS_STREAM_CREATE, (uint64_t)outReadHandle,
                             (uint64_t)outWriteHandle, (uint64_t)capacity);
    }
    inline int stream_read(int handle, void* buf, int maxLen) {
        return (int)syscall3(Montauk::SYS_STREAM_READ, (uint64_t)handle, (uint64_t)buf, (uint64_t)maxLen);
    }
    inline int stream_write(int handle, const void* data, int len) {
        return (int)syscall3(Montauk::SYS_STREAM_WRITE, (uint64_t)handle, (uint64_t)data, (uint64_t)len);
    }
    inline int mailbox_create(int* outSendHandle, int* outRecvHandle) {
        return (int)syscall2(Montauk::SYS_MAILBOX_CREATE, (uint64_t)outSendHandle, (uint64_t)outRecvHandle);
    }
    inline int mailbox_send(int handle, uint32_t msgType, const void* data, uint16_t len, int attachHandle = -1) {
        return (int)syscall5(Montauk::SYS_MAILBOX_SEND, (uint64_t)handle, (uint64_t)msgType,
                             (uint64_t)data, (uint64_t)len, (uint64_t)(int64_t)attachHandle);
    }
    inline int mailbox_recv(int handle, uint32_t* outMsgType, void* data,
                            uint16_t* inOutLen, int* outAttachHandle = nullptr) {
        return (int)syscall5(Montauk::SYS_MAILBOX_RECV, (uint64_t)handle, (uint64_t)outMsgType,
                             (uint64_t)data, (uint64_t)inOutLen, (uint64_t)outAttachHandle);
    }
    inline int waitset_create() {
        return (int)syscall0(Montauk::SYS_WAITSET_CREATE);
    }
    inline int waitset_add(int waitsetHandle, int targetHandle, uint32_t signals) {
        return (int)syscall3(Montauk::SYS_WAITSET_ADD, (uint64_t)waitsetHandle,
                             (uint64_t)targetHandle, (uint64_t)signals);
    }
    inline int waitset_remove(int waitsetHandle, int index) {
        return (int)syscall2(Montauk::SYS_WAITSET_REMOVE, (uint64_t)waitsetHandle, (uint64_t)index);
    }
    inline int waitset_wait(int waitsetHandle, Montauk::IpcWaitResult* outReady, uint64_t timeoutMs = ~0ULL) {
        return (int)syscall3(Montauk::SYS_WAITSET_WAIT, (uint64_t)waitsetHandle, (uint64_t)outReady, timeoutMs);
    }
    inline int proc_open(int pid) {
        return (int)syscall1(Montauk::SYS_PROC_OPEN, (uint64_t)pid);
    }
    inline int surface_create(uint64_t byteSize) {
        return (int)syscall1(Montauk::SYS_SURFACE_CREATE, byteSize);
    }
    inline void* surface_map(int handle) {
        return (void*)syscall1(Montauk::SYS_SURFACE_MAP, (uint64_t)handle);
    }
    inline int surface_resize(int handle, uint64_t newSize) {
        return (int)syscall2(Montauk::SYS_SURFACE_RESIZE, (uint64_t)handle, newSize);
    }

    // Shared library support
    inline int load_lib(const char* path) {
        return (int)syscall1(Montauk::SYS_LOAD_LIB, (uint64_t)path);
    }
    inline int unload_lib(int handle) {
        return (int)syscall1(Montauk::SYS_UNLOAD_LIB, (uint64_t)handle);
    }
    inline void* dlsym(int handle, uint64_t symbolOffset) {
        return (void*)syscall2(Montauk::SYS_DLSYM, (uint64_t)handle, symbolOffset);
    }
    inline uint64_t get_libbase(int handle) {
        return (uint64_t)syscall1(Montauk::SYS_GETLIBBASE, (uint64_t)handle);
    }

    // Process management
    inline void waitpid(int pid) { syscall1(Montauk::SYS_WAITPID, (uint64_t)pid); }

    // Framebuffer
    inline void fb_info(Montauk::FbInfo* info) { syscall1(Montauk::SYS_FBINFO, (uint64_t)info); }
    inline void* fb_map() { return (void*)syscall0(Montauk::SYS_FBMAP); }

    // Arguments
    inline int getargs(char* buf, uint64_t maxLen) {
        return (int)syscall2(Montauk::SYS_GETARGS, (uint64_t)buf, maxLen);
    }

    // Terminal
    inline void termsize(int* cols, int* rows) {
        uint64_t r = (uint64_t)syscall0(Montauk::SYS_TERMSIZE);
        if (cols) *cols = (int)(r & 0xFFFFFFFF);
        if (rows) *rows = (int)(r >> 32);
    }

    inline void termscale(int scale_x, int scale_y) {
        syscall2(Montauk::SYS_TERMSCALE, (uint64_t)scale_x, (uint64_t)scale_y);
    }

    inline void get_termscale(int* scale_x, int* scale_y) {
        uint64_t r = (uint64_t)syscall2(Montauk::SYS_TERMSCALE, 0, 0);
        if (scale_x) *scale_x = (int)(r & 0xFFFFFFFF);
        if (scale_y) *scale_y = (int)(r >> 32);
    }

    // Timekeeping (wall-clock)
    inline void gettime(Montauk::DateTime* out) { syscall1(Montauk::SYS_GETTIME, (uint64_t)out); }

    // Timezone offset (total minutes from UTC)
    inline void settz(int offset_minutes) { syscall1(Montauk::SYS_SETTZ, (uint64_t)(int64_t)offset_minutes); }
    inline int gettz() { return (int)syscall0(Montauk::SYS_GETTZ); }

    // Random number generation
    inline int64_t getrandom(void* buf, uint32_t len) {
        return syscall2(Montauk::SYS_GETRANDOM, (uint64_t)buf, (uint64_t)len);
    }

    // Power management
    [[noreturn]] inline void reset() {
        syscall0(Montauk::SYS_RESET);
        __builtin_unreachable();
    }

    [[noreturn]] inline void shutdown() {
        syscall0(Montauk::SYS_SHUTDOWN);
        __builtin_unreachable();
    }

    inline int suspend() {
        return (int)syscall0(Montauk::SYS_SUSPEND);
    }

    // Mouse
    inline void mouse_state(Montauk::MouseState* out) { syscall1(Montauk::SYS_MOUSESTATE, (uint64_t)out); }
    inline void set_mouse_bounds(int32_t maxX, int32_t maxY) {
        syscall2(Montauk::SYS_SETMOUSEBOUNDS, (uint64_t)maxX, (uint64_t)maxY);
    }

    // Kernel log
    inline int64_t read_klog(char* buf, uint64_t size) {
        return syscall2(Montauk::SYS_KLOG, (uint64_t)buf, size);
    }

    // I/O redirection
    inline int spawn_redir(const char* path, const char* args = nullptr) {
        return (int)syscall2(Montauk::SYS_SPAWN_REDIR, (uint64_t)path, (uint64_t)args);
    }
    inline int childio_read(int childPid, char* buf, int maxLen) {
        return (int)syscall3(Montauk::SYS_CHILDIO_READ, (uint64_t)childPid, (uint64_t)buf, (uint64_t)maxLen);
    }
    inline int childio_write(int childPid, const char* data, int len) {
        return (int)syscall3(Montauk::SYS_CHILDIO_WRITE, (uint64_t)childPid, (uint64_t)data, (uint64_t)len);
    }
    inline int childio_writekey(int childPid, const Montauk::KeyEvent* key) {
        return (int)syscall2(Montauk::SYS_CHILDIO_WRITEKEY, (uint64_t)childPid, (uint64_t)key);
    }
    inline int childio_settermsz(int childPid, int cols, int rows) {
        return (int)syscall3(Montauk::SYS_CHILDIO_SETTERMSZ, (uint64_t)childPid, (uint64_t)cols, (uint64_t)rows);
    }

    // Process listing / kill
    inline int proclist(Montauk::ProcInfo* buf, int max) {
        return (int)syscall2(Montauk::SYS_PROCLIST, (uint64_t)buf, (uint64_t)max);
    }
    inline int kill(int pid) {
        return (int)syscall1(Montauk::SYS_KILL, (uint64_t)pid);
    }
    inline int devlist(Montauk::DevInfo* buf, int max) {
        return (int)syscall2(Montauk::SYS_DEVLIST, (uint64_t)buf, (uint64_t)max);
    }
    inline int diskinfo(Montauk::DiskInfo* buf, int port) {
        return (int)syscall2(Montauk::SYS_DISKINFO, (uint64_t)buf, (uint64_t)port);
    }

    // Partition table
    inline int partlist(Montauk::PartInfo* buf, int max) {
        return (int)syscall2(Montauk::SYS_PARTLIST, (uint64_t)buf, (uint64_t)max);
    }

    // Raw block device I/O (driver-agnostic)
    inline int64_t disk_read(int blockDev, uint64_t lba, uint32_t sectorCount, void* buf) {
        return syscall4(Montauk::SYS_DISKREAD, (uint64_t)blockDev, lba,
                        (uint64_t)sectorCount, (uint64_t)buf);
    }
    inline int64_t disk_write(int blockDev, uint64_t lba, uint32_t sectorCount, const void* buf) {
        return syscall4(Montauk::SYS_DISKWRITE, (uint64_t)blockDev, lba,
                        (uint64_t)sectorCount, (uint64_t)buf);
    }

    // GPT management
    inline int gpt_init(int blockDev) {
        return (int)syscall1(Montauk::SYS_GPTINIT, (uint64_t)blockDev);
    }
    inline int gpt_add(const Montauk::GptAddParams* params) {
        return (int)syscall1(Montauk::SYS_GPTADD, (uint64_t)params);
    }
    inline int fs_mount(int partIndex, int driveNum) {
        return (int)syscall2(Montauk::SYS_FSMOUNT, (uint64_t)partIndex, (uint64_t)driveNum);
    }
    inline int fs_format(const Montauk::FsFormatParams* params) {
        return (int)syscall1(Montauk::SYS_FSFORMAT, (uint64_t)params);
    }

    // Audio
    inline int audio_open(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
        return (int)syscall3(Montauk::SYS_AUDIOOPEN, (uint64_t)sampleRate,
                             (uint64_t)channels, (uint64_t)bitsPerSample);
    }
    inline void audio_close(int handle) {
        syscall1(Montauk::SYS_AUDIOCLOSE, (uint64_t)handle);
    }
    inline int audio_write(int handle, const void* data, uint32_t size) {
        return (int)syscall3(Montauk::SYS_AUDIOWRITE, (uint64_t)handle,
                             (uint64_t)data, (uint64_t)size);
    }
    inline int audio_ctl(int handle, int cmd, int value) {
        return (int)syscall3(Montauk::SYS_AUDIOCTL, (uint64_t)handle,
                             (uint64_t)cmd, (uint64_t)value);
    }
    inline int audio_set_volume(int handle, int percent) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_SET_VOLUME, percent);
    }
    inline int audio_get_volume(int handle) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_GET_VOLUME, 0);
    }
    inline int audio_get_pos(int handle) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_GET_POS, 0);
    }
    inline int audio_pause(int handle) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_PAUSE, 1);
    }
    inline int audio_resume(int handle) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_PAUSE, 0);
    }
    inline int audio_get_output(int handle) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_GET_OUTPUT, 0);
    }
    inline int audio_bt_status(int handle) {
        return audio_ctl(handle, Montauk::AUDIO_CTL_BT_STATUS, 0);
    }

    // Bluetooth
    inline int bt_scan(Montauk::BtScanResult* buf, int maxCount, uint32_t timeoutMs) {
        return (int)syscall3(Montauk::SYS_BTSCAN, (uint64_t)buf, (uint64_t)maxCount, (uint64_t)timeoutMs);
    }
    inline int bt_connect(const uint8_t* bdAddr) {
        return (int)syscall1(Montauk::SYS_BTCONNECT, (uint64_t)bdAddr);
    }
    inline int bt_disconnect(const uint8_t* bdAddr) {
        return (int)syscall1(Montauk::SYS_BTDISCONNECT, (uint64_t)bdAddr);
    }
    inline int bt_list(Montauk::BtDevInfo* buf, int maxCount) {
        return (int)syscall2(Montauk::SYS_BTLIST, (uint64_t)buf, (uint64_t)maxCount);
    }
    inline int bt_info(Montauk::BtAdapterInfo* buf) {
        return (int)syscall1(Montauk::SYS_BTINFO, (uint64_t)buf);
    }

    // Kernel introspection
    inline void memstats(Montauk::MemStats* out) { syscall1(Montauk::SYS_MEMSTATS, (uint64_t)out); }

    // User management
    inline int setuser(int pid, const char* name) {
        return (int)syscall2(Montauk::SYS_SETUSER, (uint64_t)pid, (uint64_t)name);
    }
    inline int getuser(char* buf, uint64_t maxLen) {
        return (int)syscall2(Montauk::SYS_GETUSER, (uint64_t)buf, maxLen);
    }

    // Window server
    inline int win_create(const char* title, int w, int h, Montauk::WinCreateResult* result) {
        return (int)syscall4(Montauk::SYS_WINCREATE, (uint64_t)title, (uint64_t)w, (uint64_t)h, (uint64_t)result);
    }
    inline int win_destroy(int id) {
        return (int)syscall1(Montauk::SYS_WINDESTROY, (uint64_t)id);
    }
    inline uint64_t win_present(int id) {
        return (uint64_t)syscall1(Montauk::SYS_WINPRESENT, (uint64_t)id);
    }
    inline int win_poll(int id, Montauk::WinEvent* event) {
        return (int)syscall2(Montauk::SYS_WINPOLL, (uint64_t)id, (uint64_t)event);
    }
    inline int win_enumerate(Montauk::WinInfo* info, int max) {
        return (int)syscall2(Montauk::SYS_WINENUM, (uint64_t)info, (uint64_t)max);
    }
    inline uint64_t win_map(int id) {
        return (uint64_t)syscall1(Montauk::SYS_WINMAP, (uint64_t)id);
    }
    inline int win_unmap(int id) {
        return (int)syscall1(Montauk::SYS_WINUNMAP, (uint64_t)id);
    }
    inline int win_sendevent(int id, const Montauk::WinEvent* event) {
        return (int)syscall2(Montauk::SYS_WINSENDEVENT, (uint64_t)id, (uint64_t)event);
    }
    inline uint64_t win_resize(int id, int w, int h) {
        return (uint64_t)syscall3(Montauk::SYS_WINRESIZE, (uint64_t)id, (uint64_t)w, (uint64_t)h);
    }
    inline int win_setscale(int scale) {
        return (int)syscall1(Montauk::SYS_WINSETSCALE, (uint64_t)scale);
    }
    inline int win_getscale() {
        return (int)syscall0(Montauk::SYS_WINGETSCALE);
    }
    inline int win_setcursor(int id, int cursor) {
        return (int)syscall2(Montauk::SYS_WINSETCURSOR, (uint64_t)id, (uint64_t)cursor);
    }

}
