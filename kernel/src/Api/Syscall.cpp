/*
    * Syscall.cpp
    * SYSCALL/SYSRET setup and number-based dispatch
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Syscall.hpp"
#include <Timekeeping/Time.hpp>
#include <Terminal/Terminal.hpp>
#include <Fs/Vfs.hpp>
#include <Memory/Heap.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Sched/Scheduler.hpp>
#include <Libraries/Memory.hpp>
#include <Libraries/String.hpp>
#include <Drivers/PS2/Keyboard.hpp>
#include <Net/Icmp.hpp>
#include <Net/Dns.hpp>
#include <Net/Socket.hpp>
#include <Net/ByteOrder.hpp>
#include <Net/NetConfig.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Hal/MSR.hpp>
#include <Hal/GDT.hpp>
#include <Graphics/Cursor.hpp>
#include "../Libraries/flanterm/src/flanterm.h"

// Assembly entry point
extern "C" void SyscallEntry();

namespace Zenith {

    // ---- Syscall implementations ----

    static void Sys_Exit(int exitCode) {
        (void)exitCode;
        Sched::ExitProcess();
    }

    static void Sys_Yield() {
        Sched::Schedule();
    }

    static void Sys_SleepMs(uint64_t ms) {
        Timekeeping::Sleep(ms);
    }

    static int Sys_GetPid() {
        return Sched::GetCurrentPid();
    }

    static void Sys_Print(const char* text) {
        Kt::Print(text);
    }

    static void Sys_Putchar(char c) {
        Kt::Putchar(c);
    }

    static int Sys_Open(const char* path) {
        return Fs::Vfs::VfsOpen(path);
    }

    static int Sys_Read(int handle, uint8_t* buffer, uint64_t offset, uint64_t size) {
        return Fs::Vfs::VfsRead(handle, buffer, offset, size);
    }

    static uint64_t Sys_GetSize(int handle) {
        return Fs::Vfs::VfsGetSize(handle);
    }

    static void Sys_Close(int handle) {
        Fs::Vfs::VfsClose(handle);
    }

    static int Sys_ReadDir(const char* path, const char** outNames, int maxEntries) {
        // Get entries from VFS into a kernel-local array
        const char* kernelNames[64];
        int max = maxEntries;
        if (max > 64) max = 64;
        int count = Fs::Vfs::VfsReadDir(path, kernelNames, max);
        if (count <= 0) return count;

        // Allocate a user-accessible page for string data via process heap
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return -1;

        void* page = Memory::g_pfa->AllocateZeroed();
        if (page == nullptr) return -1;
        uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
        uint64_t userVa = proc->heapNext;
        proc->heapNext += 0x1000;
        Memory::VMM::Paging::MapUserIn(proc->pml4Phys, physAddr, userVa);

        // Copy strings into the user page and write pointers to outNames
        uint64_t offset = 0;
        uint8_t* pageBuf = (uint8_t*)Memory::HHDM(physAddr);
        int copied = 0;
        for (int i = 0; i < count; i++) {
            int len = Lib::strlen(kernelNames[i]) + 1;
            if (offset + len > 0x1000) break;
            memcpy(pageBuf + offset, kernelNames[i], len);
            outNames[i] = (const char*)(userVa + offset);
            offset += len;
            copied++;
        }

        return copied;
    }

    static uint64_t Sys_Alloc(uint64_t size) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;

        // Round up to page boundary
        size = (size + 0xFFF) & ~0xFFFULL;
        if (size == 0) size = 0x1000;

        uint64_t userVa = proc->heapNext;
        uint64_t numPages = size / 0x1000;

        for (uint64_t i = 0; i < numPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) return 0;
            uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
            Memory::VMM::Paging::MapUserIn(proc->pml4Phys, physAddr, userVa + i * 0x1000);
        }

        proc->heapNext += size;
        return userVa;
    }

    static void Sys_Free(uint64_t) {
        // No-op for now (pages leak). Proper freeing can come later.
    }

    static uint64_t Sys_GetTicks() {
        return Timekeeping::GetTicks();
    }

    static uint64_t Sys_GetMilliseconds() {
        return Timekeeping::GetMilliseconds();
    }

    static void Sys_GetInfo(SysInfo* outInfo) {
        if (outInfo == nullptr) return;

        // Copy strings into fixed-size arrays (user-accessible)
        const char* name = "ZenithOS";
        const char* ver = "0.1.0";
        for (int i = 0; name[i]; i++) outInfo->osName[i] = name[i];
        outInfo->osName[8] = '\0';
        for (int i = 0; ver[i]; i++) outInfo->osVersion[i] = ver[i];
        outInfo->osVersion[5] = '\0';

        outInfo->apiVersion = 2;
        outInfo->maxProcesses = Sched::MaxProcesses;
    }

    static bool Sys_IsKeyAvailable() {
        return Drivers::PS2::Keyboard::IsKeyAvailable();
    }

    static void Sys_GetKey(KeyEvent* outEvent) {
        if (outEvent == nullptr) return;
        auto k = Drivers::PS2::Keyboard::GetKey();
        outEvent->scancode = k.Scancode;
        outEvent->ascii    = k.Ascii;
        outEvent->pressed  = k.Pressed;
        outEvent->shift    = k.Shift;
        outEvent->ctrl     = k.Ctrl;
        outEvent->alt      = k.Alt;
    }

    static char Sys_GetChar() {
        return Drivers::PS2::Keyboard::GetChar();
    }

    static uint16_t g_pingSeq = 0;
    static constexpr uint16_t PING_ID = 0x2E01; // "ZE"

    static void Sys_FbInfo(FbInfo* out) {
        if (out == nullptr) return;
        out->width    = Graphics::Cursor::GetFramebufferWidth();
        out->height   = Graphics::Cursor::GetFramebufferHeight();
        out->pitch    = Graphics::Cursor::GetFramebufferPitch();
        out->bpp      = 32;
        out->userAddr = 0;
    }

    static void Sys_WaitPid(int pid) {
        while (Sched::IsAlive(pid)) {
            Sched::Schedule();  // yield until the process exits
        }
    }

    static uint64_t Sys_FbMap() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;

        uint32_t* fbBase = Graphics::Cursor::GetFramebufferBase();
        if (fbBase == nullptr) return 0;

        uint64_t fbPhys = Memory::SubHHDM((uint64_t)fbBase);
        uint64_t fbSize = Graphics::Cursor::GetFramebufferHeight()
                        * Graphics::Cursor::GetFramebufferPitch();
        uint64_t numPages = (fbSize + 0xFFF) / 0x1000;

        Kt::KernelLogStream(Kt::INFO, "FbMap") << "fbPhys=" << kcp::hex << fbPhys
            << " size=" << kcp::dec << fbSize
            << " pages=" << numPages
            << " (" << Graphics::Cursor::GetFramebufferWidth()
            << "x" << Graphics::Cursor::GetFramebufferHeight()
            << " pitch=" << Graphics::Cursor::GetFramebufferPitch() << ")";

        // Map at a fixed user VA
        constexpr uint64_t userVa = 0x50000000ULL;

        for (uint64_t i = 0; i < numPages; i++) {
            Memory::VMM::Paging::MapUserInWC(
                proc->pml4Phys,
                fbPhys + i * 0x1000,
                userVa + i * 0x1000
            );
        }

        return userVa;
    }

    static int32_t Sys_Ping(uint32_t ipAddr, uint32_t timeoutMs) {
        uint16_t seq = g_pingSeq++;

        Net::Icmp::ResetReply();
        Net::Icmp::SendEchoRequest(ipAddr, PING_ID, seq);

        uint64_t start = Timekeeping::GetMilliseconds();
        while (!Net::Icmp::HasReply(PING_ID, seq)) {
            if (Timekeeping::GetMilliseconds() - start >= timeoutMs) {
                return -1;
            }
            Sched::Schedule();
        }

        return (int32_t)(Timekeeping::GetMilliseconds() - start);
    }

    static int Sys_Spawn(const char* path, const char* args) {
        return Sched::Spawn(path, args);
    }

    static int Sys_GetArgs(char* buf, uint64_t maxLen) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr || buf == nullptr || maxLen == 0) return -1;
        int i = 0;
        for (; i < (int)maxLen - 1 && proc->args[i]; i++) {
            buf[i] = proc->args[i];
        }
        buf[i] = '\0';
        return i;
    }

    static uint64_t Sys_TermSize() {
        size_t cols = 0, rows = 0;
        flanterm_get_dimensions(Kt::ctx, &cols, &rows);
        return (rows << 32) | (cols & 0xFFFFFFFF);
    }

    static void Sys_GetTime(DateTime* out) {
        if (out == nullptr) return;
        Timekeeping::DateTime dt = Timekeeping::GetDateTime();
        out->Year   = dt.Year;
        out->Month  = dt.Month;
        out->Day    = dt.Day;
        out->Hour   = dt.Hour;
        out->Minute = dt.Minute;
        out->Second = dt.Second;
    }

    // ---- Socket syscalls ----

    static int Sys_Socket(int type) {
        return Net::Socket::Create(type, Sched::GetCurrentPid());
    }

    static int Sys_Connect(int fd, uint32_t ip, uint16_t port) {
        return Net::Socket::Connect(fd, ip, port, Sched::GetCurrentPid());
    }

    static int Sys_Bind(int fd, uint16_t port) {
        return Net::Socket::Bind(fd, port, Sched::GetCurrentPid());
    }

    static int Sys_Listen(int fd) {
        return Net::Socket::Listen(fd, Sched::GetCurrentPid());
    }

    static int Sys_Accept(int fd) {
        return Net::Socket::Accept(fd, Sched::GetCurrentPid());
    }

    static int Sys_Send(int fd, const uint8_t* data, uint32_t len) {
        return Net::Socket::Send(fd, data, len, Sched::GetCurrentPid());
    }

    static int Sys_Recv(int fd, uint8_t* buf, uint32_t maxLen) {
        return Net::Socket::Recv(fd, buf, maxLen, Sched::GetCurrentPid());
    }

    static void Sys_CloseSock(int fd) {
        Net::Socket::Close(fd, Sched::GetCurrentPid());
    }

    static int Sys_SendTo(int fd, const uint8_t* data, uint32_t len,
                          uint32_t destIp, uint16_t destPort) {
        return Net::Socket::SendTo(fd, data, len, destIp, destPort, Sched::GetCurrentPid());
    }

    static int Sys_RecvFrom(int fd, uint8_t* buf, uint32_t maxLen,
                            uint32_t* srcIp, uint16_t* srcPort) {
        return Net::Socket::RecvFrom(fd, buf, maxLen, srcIp, srcPort, Sched::GetCurrentPid());
    }

    static void Sys_GetNetCfg(NetCfg* out) {
        if (out == nullptr) return;
        out->ipAddress  = Net::GetIpAddress();
        out->subnetMask = Net::GetSubnetMask();
        out->gateway    = Net::GetGateway();

        const uint8_t* mac = nullptr;
        if (Drivers::Net::E1000::IsInitialized()) {
            mac = Drivers::Net::E1000::GetMacAddress();
        } else if (Drivers::Net::E1000E::IsInitialized()) {
            mac = Drivers::Net::E1000E::GetMacAddress();
        }
        if (mac) {
            for (int i = 0; i < 6; i++) out->macAddress[i] = mac[i];
        } else {
            for (int i = 0; i < 6; i++) out->macAddress[i] = 0;
        }
        out->_pad[0] = 0;
        out->_pad[1] = 0;
        out->dnsServer = Net::GetDnsServer();
    }

    static int Sys_SetNetCfg(const NetCfg* in) {
        if (in == nullptr) return -1;
        Net::SetIpAddress(in->ipAddress);
        Net::SetSubnetMask(in->subnetMask);
        Net::SetGateway(in->gateway);
        Net::SetDnsServer(in->dnsServer);
        return 0;
    }

    static void Sys_Reset() {
        /*
            Triple fault for now; TODO: implement UEFI runtime function for clean reboot.

            We implement the triple fault by loading a null IDT into the IDT register,
            and then immediately triggering an interrupt.

            This technique should pretty much work across the board but it's of course
            better to use the UEFI runtime API as it has a method for this purpose,
            along with shutdown.
        */
       
        struct [[gnu::packed]] { uint16_t limit; uint64_t base; } nullIdt = {0, 0};
        asm volatile("lidt %0; int $0x03" :: "m"(nullIdt));
        __builtin_unreachable();
    }

    // ---- File write/create ----

    static int Sys_FWrite(int handle, const uint8_t* data, uint64_t offset, uint64_t size) {
        return Fs::Vfs::VfsWrite(handle, data, offset, size);
    }

    static int Sys_FCreate(const char* path) {
        return Fs::Vfs::VfsCreate(path);
    }

    // ---- Terminal scaling ----

    static int64_t Sys_TermScale(uint64_t scale_x, uint64_t scale_y) {
        if (scale_x == 0) {
            return (int64_t)((Kt::GetFontScaleY() << 32) | (Kt::GetFontScaleX() & 0xFFFFFFFF));
        }
        Kt::Rescale((size_t)scale_x, (size_t)scale_y);
        size_t cols = 0, rows = 0;
        flanterm_get_dimensions(Kt::ctx, &cols, &rows);
        return (int64_t)((rows << 32) | (cols & 0xFFFFFFFF));
    }

    // ---- DNS resolve ----

    static int64_t Sys_Resolve(const char* hostname) {
        uint32_t ip = Net::Dns::Resolve(hostname);
        return (int64_t)ip;
    }

    // ---- Random number generation ----
    // Uses RDTSC mixed with xorshift64* PRNG for entropy.
    // RDRAND is intentionally avoided: some firmware disables the RDRAND
    // hardware unit while CPUID still advertises support (bit 30 of ECX),
    // causing #UD on real hardware. RDTSC-based entropy is sufficient for
    // seeding BearSSL's PRNG for TLS session keys.

    static int64_t Sys_GetRandom(uint8_t* buf, uint64_t len) {
        uint64_t tsc;
        asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) :: "rdx");
        uint64_t state = tsc;

        for (uint64_t i = 0; i < len; i += 8) {
            asm volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) :: "rdx");
            state ^= tsc;
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            uint64_t val = state * 0x2545F4914F6CDD1DULL;

            uint64_t remaining = len - i;
            uint64_t toCopy = remaining < 8 ? remaining : 8;
            for (uint64_t j = 0; j < toCopy; j++)
                buf[i + j] = (uint8_t)(val >> (j * 8));
        }
        return (int64_t)len;
    }

    // ---- Dispatch ----

    extern "C" int64_t SyscallDispatch(SyscallFrame* frame) {
        switch (frame->syscall_nr) {
            case SYS_EXIT:
                Sys_Exit((int)frame->arg1);
                return 0;
            case SYS_YIELD:
                Sys_Yield();
                return 0;
            case SYS_SLEEP_MS:
                Sys_SleepMs(frame->arg1);
                return 0;
            case SYS_GETPID:
                return (int64_t)Sys_GetPid();
            case SYS_PRINT:
                Sys_Print((const char*)frame->arg1);
                return 0;
            case SYS_PUTCHAR:
                Sys_Putchar((char)frame->arg1);
                return 0;
            case SYS_OPEN:
                return (int64_t)Sys_Open((const char*)frame->arg1);
            case SYS_READ:
                return (int64_t)Sys_Read((int)frame->arg1, (uint8_t*)frame->arg2,
                                         frame->arg3, frame->arg4);
            case SYS_GETSIZE:
                return (int64_t)Sys_GetSize((int)frame->arg1);
            case SYS_CLOSE:
                Sys_Close((int)frame->arg1);
                return 0;
            case SYS_READDIR:
                return (int64_t)Sys_ReadDir((const char*)frame->arg1,
                                            (const char**)frame->arg2,
                                            (int)frame->arg3);
            case SYS_ALLOC:
                return (int64_t)Sys_Alloc(frame->arg1);
            case SYS_FREE:
                Sys_Free(frame->arg1);
                return 0;
            case SYS_GETTICKS:
                return (int64_t)Sys_GetTicks();
            case SYS_GETMILLISECONDS:
                return (int64_t)Sys_GetMilliseconds();
            case SYS_GETINFO:
                Sys_GetInfo((SysInfo*)frame->arg1);
                return 0;
            case SYS_ISKEYAVAILABLE:
                return (int64_t)Sys_IsKeyAvailable();
            case SYS_GETKEY:
                Sys_GetKey((KeyEvent*)frame->arg1);
                return 0;
            case SYS_GETCHAR:
                return (int64_t)Sys_GetChar();
            case SYS_PING:
                return (int64_t)Sys_Ping((uint32_t)frame->arg1, (uint32_t)frame->arg2);
            case SYS_SPAWN:
                return (int64_t)Sys_Spawn((const char*)frame->arg1, (const char*)frame->arg2);
            case SYS_WAITPID:
                Sys_WaitPid((int)frame->arg1);
                return 0;
            case SYS_FBINFO:
                Sys_FbInfo((FbInfo*)frame->arg1);
                return 0;
            case SYS_FBMAP:
                return (int64_t)Sys_FbMap();
            case SYS_TERMSIZE:
                return (int64_t)Sys_TermSize();
            case SYS_GETARGS:
                return (int64_t)Sys_GetArgs((char*)frame->arg1, frame->arg2);
            case SYS_RESET:
                Sys_Reset();
                return 0;
            case SYS_SHUTDOWN:
                /* Unimplemented */
                return -1;
            case SYS_GETTIME:
                Sys_GetTime((DateTime*)frame->arg1);
                return 0;
            case SYS_SOCKET:
                return (int64_t)Sys_Socket((int)frame->arg1);
            case SYS_CONNECT:
                return (int64_t)Sys_Connect((int)frame->arg1, (uint32_t)frame->arg2, (uint16_t)frame->arg3);
            case SYS_BIND:
                return (int64_t)Sys_Bind((int)frame->arg1, (uint16_t)frame->arg2);
            case SYS_LISTEN:
                return (int64_t)Sys_Listen((int)frame->arg1);
            case SYS_ACCEPT:
                return (int64_t)Sys_Accept((int)frame->arg1);
            case SYS_SEND:
                return (int64_t)Sys_Send((int)frame->arg1, (const uint8_t*)frame->arg2, (uint32_t)frame->arg3);
            case SYS_RECV:
                return (int64_t)Sys_Recv((int)frame->arg1, (uint8_t*)frame->arg2, (uint32_t)frame->arg3);
            case SYS_CLOSESOCK:
                Sys_CloseSock((int)frame->arg1);
                return 0;
            case SYS_GETNETCFG:
                Sys_GetNetCfg((NetCfg*)frame->arg1);
                return 0;
            case SYS_SETNETCFG:
                return (int64_t)Sys_SetNetCfg((const NetCfg*)frame->arg1);
            case SYS_SENDTO:
                return (int64_t)Sys_SendTo((int)frame->arg1, (const uint8_t*)frame->arg2,
                                           (uint32_t)frame->arg3, (uint32_t)frame->arg4,
                                           (uint16_t)frame->arg5);
            case SYS_RECVFROM:
                return (int64_t)Sys_RecvFrom((int)frame->arg1, (uint8_t*)frame->arg2,
                                             (uint32_t)frame->arg3, (uint32_t*)frame->arg4,
                                             (uint16_t*)frame->arg5);
            case SYS_FWRITE:
                return (int64_t)Sys_FWrite((int)frame->arg1, (const uint8_t*)frame->arg2,
                                           frame->arg3, frame->arg4);
            case SYS_FCREATE:
                return (int64_t)Sys_FCreate((const char*)frame->arg1);
            case SYS_TERMSCALE:
                return Sys_TermScale(frame->arg1, frame->arg2);
            case SYS_RESOLVE:
                return Sys_Resolve((const char*)frame->arg1);
            case SYS_GETRANDOM:
                return Sys_GetRandom((uint8_t*)frame->arg1, frame->arg2);
            case SYS_KLOG:
                return Kt::ReadKernelLog((char*)frame->arg1, frame->arg2);
            default:
                return -1;
        }
    }

    // ---- SYSCALL MSR initialization ----

    void InitializeSyscalls() {
        // Enable SYSCALL/SYSRET in EFER
        uint64_t efer = Hal::ReadMSR(Hal::IA32_EFER);
        efer |= 1;  // SCE bit (Syscall Enable)
        Hal::WriteMSR(Hal::IA32_EFER, efer);

        // STAR: kernel CS in [47:32], sysret base in [63:48]
        // SYSCALL: CS=0x08, SS=0x10
        // SYSRET:  CS=0x10+16=0x20|RPL3=0x23, SS=0x10+8=0x18|RPL3=0x1B
        uint64_t star = (0x0010ULL << 48) | (0x0008ULL << 32);
        Hal::WriteMSR(Hal::IA32_STAR, star);

        // LSTAR: SYSCALL entry point
        Hal::WriteMSR(Hal::IA32_LSTAR, (uint64_t)SyscallEntry);

        // FMASK: mask IF on SYSCALL entry (bit 9 = IF)
        Hal::WriteMSR(Hal::IA32_FMASK, 0x200);

        Kt::KernelLogStream(Kt::OK, "Syscall") << "SYSCALL/SYSRET initialized (LSTAR="
            << kcp::hex << (uint64_t)SyscallEntry << kcp::dec << ", 47 syscalls)";
    }

}
