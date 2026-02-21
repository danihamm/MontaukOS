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
#include <Drivers/PS2/Mouse.hpp>
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
#include "WinServer.hpp"
#include <Pci/Pci.hpp>
#include <Drivers/USB/Xhci.hpp>
#include <Drivers/Graphics/IntelGPU.hpp>
#include <Drivers/PS2/PS2Controller.hpp>
#include <Hal/Apic/ApicInit.hpp>

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

    static void RingWrite(uint8_t* buf, uint32_t& head, uint32_t /*tail*/, uint32_t size, uint8_t byte) {
        buf[head] = byte;
        head = (head + 1) % size;
    }

    static int RingRead(uint8_t* buf, uint32_t& head, uint32_t& tail, uint32_t size, uint8_t* out, int maxLen) {
        int count = 0;
        while (tail != head && count < maxLen) {
            out[count++] = buf[tail];
            tail = (tail + 1) % size;
        }
        return count;
    }

    // Find the process that owns the I/O ring buffers for a redirected process.
    // If proc owns buffers itself (spawned via spawn_redir), returns proc.
    // If proc inherited redirection (spawned via spawn from a redirected parent),
    // follows parentPid to find the buffer owner.
    static Sched::Process* GetRedirTarget(Sched::Process* proc) {
        if (!proc || !proc->redirected) return nullptr;
        if (proc->outBuf) return proc;  // owns buffers
        return Sched::GetProcessByPid(proc->parentPid);
    }

    static void Sys_Print(const char* text) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->outBuf) {
                for (int i = 0; text[i]; i++) {
                    RingWrite(target->outBuf, target->outHead, target->outTail, Sched::Process::IoBufSize, (uint8_t)text[i]);
                }
                return;
            }
        }
        Kt::Print(text);
    }

    static void Sys_Putchar(char c) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->outBuf) {
                RingWrite(target->outBuf, target->outHead, target->outTail, Sched::Process::IoBufSize, (uint8_t)c);
                return;
            }
        }
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
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target) return target->keyHead != target->keyTail;
        }
        return Drivers::PS2::Keyboard::IsKeyAvailable();
    }

    static void Sys_GetKey(KeyEvent* outEvent) {
        if (outEvent == nullptr) return;
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target) {
                // Wait for key in target's keyBuf ring
                while (target->keyHead == target->keyTail) {
                    Sched::Schedule();
                }
                *outEvent = target->keyBuf[target->keyTail];
                target->keyTail = (target->keyTail + 1) % 64;
                return;
            }
        }
        auto k = Drivers::PS2::Keyboard::GetKey();
        outEvent->scancode = k.Scancode;
        outEvent->ascii    = k.Ascii;
        outEvent->pressed  = k.Pressed;
        outEvent->shift    = k.Shift;
        outEvent->ctrl     = k.Ctrl;
        outEvent->alt      = k.Alt;
    }

    static char Sys_GetChar() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->inBuf) {
                // Wait for data in target's inBuf ring
                while (target->inTail == target->inHead) {
                    Sched::Schedule(); // yield until parent writes
                }
                uint8_t c = target->inBuf[target->inTail];
                target->inTail = (target->inTail + 1) % Sched::Process::IoBufSize;
                return (char)c;
            }
        }
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
        auto* parent = Sched::GetCurrentProcessPtr();
        int childPid = Sched::Spawn(path, args);
        if (childPid < 0) return childPid;

        // Inherit I/O redirection: if the parent is redirected, the child
        // is marked redirected too. It stores a parentPid pointing to the
        // process that owns the actual ring buffers (the one spawned via
        // spawn_redir). The child does NOT get its own buffers — Sys_Print
        // et al. look up the buffer owner at write time.
        if (parent && parent->redirected) {
            auto* child = Sched::GetProcessByPid(childPid);
            if (child) {
                child->redirected = true;
                // Point to the buffer owner: if parent owns buffers, target parent;
                // if parent itself inherited, follow the chain.
                child->parentPid = parent->outBuf ? parent->pid : parent->parentPid;
            }
        }

        return childPid;
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
        // If the process is redirected to a GUI terminal, return those dimensions
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->termCols > 0 && target->termRows > 0) {
                return ((uint64_t)target->termRows << 32) | ((uint64_t)target->termCols & 0xFFFFFFFF);
            }
        }
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

    // ---- Mouse syscalls ----

    static void Sys_MouseState(MouseState* out) {
        if (out == nullptr) return;
        auto state = Drivers::PS2::Mouse::GetMouseState();
        out->x = state.X;
        out->y = state.Y;
        out->scrollDelta = state.ScrollDelta;
        out->buttons = state.Buttons;
    }

    static void Sys_SetMouseBounds(int32_t maxX, int32_t maxY) {
        Drivers::PS2::Mouse::SetBounds(maxX, maxY);
    }

    // ---- I/O redirection syscalls ----

    static int Sys_SpawnRedir(const char* path, const char* args) {
        int childPid = Sched::Spawn(path, args);
        if (childPid < 0) return -1;

        auto* child = Sched::GetProcessByPid(childPid);
        if (child == nullptr) return -1;

        // Allocate ring buffers
        void* outPage = Memory::g_pfa->AllocateZeroed();
        void* inPage = Memory::g_pfa->AllocateZeroed();
        if (!outPage || !inPage) return -1;

        child->outBuf = (uint8_t*)outPage;
        child->inBuf = (uint8_t*)inPage;
        child->outHead = 0;
        child->outTail = 0;
        child->inHead = 0;
        child->inTail = 0;
        child->keyHead = 0;
        child->keyTail = 0;
        child->redirected = true;
        child->parentPid = Sched::GetCurrentPid();

        return childPid;
    }

    static int Sys_ChildIoRead(int childPid, char* buf, int maxLen) {
        auto* child = Sched::GetProcessByPid(childPid);
        if (child == nullptr || !child->redirected || !child->outBuf) return -1;
        return RingRead(child->outBuf, child->outHead, child->outTail, Sched::Process::IoBufSize, (uint8_t*)buf, maxLen);
    }

    static int Sys_ChildIoWrite(int childPid, const char* data, int len) {
        auto* child = Sched::GetProcessByPid(childPid);
        if (child == nullptr || !child->redirected || !child->inBuf) return -1;
        for (int i = 0; i < len; i++) {
            RingWrite(child->inBuf, child->inHead, child->inTail, Sched::Process::IoBufSize, (uint8_t)data[i]);
        }
        return len;
    }

    static int Sys_ChildIoWriteKey(int childPid, const KeyEvent* key) {
        if (key == nullptr) return -1;
        auto* child = Sched::GetProcessByPid(childPid);
        if (child == nullptr || !child->redirected) return -1;
        child->keyBuf[child->keyHead] = *key;
        child->keyHead = (child->keyHead + 1) % 64;
        return 0;
    }

    static int Sys_ChildIoSetTermsz(int childPid, int cols, int rows) {
        auto* child = Sched::GetProcessByPid(childPid);
        if (child == nullptr || !child->redirected) return -1;
        child->termCols = cols;
        child->termRows = rows;
        return 0;
    }

    // ---- Process listing / kill ----

    static int Sys_ProcList(ProcInfo* buf, int maxCount) {
        if (buf == nullptr || maxCount <= 0) return 0;
        int count = 0;
        for (int i = 0; i < Sched::MaxProcesses && count < maxCount; i++) {
            auto* proc = Sched::GetProcessSlot(i);
            if (!proc || proc->state == Sched::ProcessState::Free) continue;

            buf[count].pid = (int32_t)proc->pid;
            buf[count].parentPid = (int32_t)proc->parentPid;
            buf[count].state = (uint8_t)proc->state;
            buf[count]._pad[0] = 0;
            buf[count]._pad[1] = 0;
            buf[count]._pad[2] = 0;
            {
                int j = 0;
                for (; j < 63 && proc->name[j]; j++)
                    buf[count].name[j] = proc->name[j];
                buf[count].name[j] = '\0';
            }
            buf[count].heapUsed = (proc->heapNext > Sched::UserHeapBase)
                ? proc->heapNext - Sched::UserHeapBase : 0;
            count++;
        }
        return count;
    }

    static int Sys_Kill(int pid) {
        // Refuse to kill PID 0 (init)
        if (pid == 0) return -1;
        // Refuse to kill the caller's own process
        if (pid == Sched::GetCurrentPid()) return -1;

        auto* proc = Sched::GetProcessByPid(pid);
        if (!proc) return -1;

        // Clean up any windows owned by this process
        WinServer::CleanupProcess(pid);

        proc->state = Sched::ProcessState::Terminated;
        return 0;
    }

    // ---- Device list ----

    static void dl_strcpy(char* dst, const char* src, int max) {
        int i = 0;
        for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
        dst[i] = '\0';
    }

    static int dl_append(char* dst, int pos, const char* src, int max) {
        for (int i = 0; src[i] && pos < max - 1; i++) dst[pos++] = src[i];
        dst[pos] = '\0';
        return pos;
    }

    static int dl_append_hex(char* dst, int pos, unsigned val, int digits, int max) {
        const char* hex = "0123456789abcdef";
        char tmp[8];
        for (int i = digits - 1; i >= 0; i--) { tmp[i] = hex[val & 0xF]; val >>= 4; }
        for (int i = 0; i < digits && pos < max - 1; i++) dst[pos++] = tmp[i];
        dst[pos] = '\0';
        return pos;
    }

    static int dl_append_dec(char* dst, int pos, int val, int max) {
        if (val == 0) { if (pos < max - 1) dst[pos++] = '0'; dst[pos] = '\0'; return pos; }
        char tmp[12]; int i = 0;
        while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
        while (i > 0 && pos < max - 1) dst[pos++] = tmp[--i];
        dst[pos] = '\0';
        return pos;
    }

    static int Sys_DevList(DevInfo* buf, int maxCount) {
        if (buf == nullptr || maxCount <= 0) return 0;
        int count = 0;

        auto add = [&](uint8_t cat, const char* name, const char* detail) {
            if (count >= maxCount) return;
            buf[count].category = cat;
            buf[count]._pad[0] = 0; buf[count]._pad[1] = 0; buf[count]._pad[2] = 0;
            dl_strcpy(buf[count].name, name, 48);
            dl_strcpy(buf[count].detail, detail, 48);
            count++;
        };

        // CPU cores (category 0)
        int cpuCount = Hal::GetDetectedCpuCount();
        if (cpuCount > 0) {
            char detail[48];
            int p = 0;
            p = dl_append(detail, p, "x86_64, ", 48);
            p = dl_append_dec(detail, p, cpuCount, 48);
            p = dl_append(detail, p, " core(s)", 48);
            add(0, "Processor", detail);
        }

        // Interrupt controllers (category 1)
        add(1, "Local APIC", "Per-CPU interrupt controller");
        add(1, "I/O APIC", "System interrupt router");

        // Timer (category 2)
        add(2, "LAPIC Timer", "Local APIC periodic timer");

        // PS/2 Input (category 3)
        add(3, "PS/2 Keyboard", "IRQ 1, scan code set 1");
        if (Drivers::PS2::IsDualChannel()) {
            add(3, "PS/2 Mouse", "IRQ 12, dual-channel 8042");
        }

        // USB devices (category 4)
        if (Drivers::USB::Xhci::IsInitialized()) {
            for (uint8_t slot = 1; slot <= Drivers::USB::Xhci::MAX_SLOTS && count < maxCount; slot++) {
                auto* dev = Drivers::USB::Xhci::GetDevice(slot);
                if (!dev || !dev->Active) continue;
                const char* devName = "USB Device";
                if (dev->InterfaceClass == 3) {
                    if (dev->InterfaceProtocol == 1) devName = "USB HID Keyboard";
                    else if (dev->InterfaceProtocol == 2) devName = "USB HID Mouse";
                    else devName = "USB HID Device";
                } else if (dev->InterfaceClass == 8) {
                    devName = "USB Mass Storage";
                } else if (dev->InterfaceClass == 9) {
                    devName = "USB Hub";
                }
                char detail[48];
                int p = 0;
                p = dl_append(detail, p, "Port ", 48);
                p = dl_append_dec(detail, p, dev->PortId, 48);
                p = dl_append(detail, p, ", VID:", 48);
                p = dl_append_hex(detail, p, dev->VendorId, 4, 48);
                p = dl_append(detail, p, " PID:", 48);
                p = dl_append_hex(detail, p, dev->ProductId, 4, 48);
                add(4, devName, detail);
            }
        }

        // Network (category 5)
        if (Drivers::Net::E1000::IsInitialized()) {
            add(5, "Intel E1000", "Gigabit Ethernet (82540EM)");
        }
        if (Drivers::Net::E1000E::IsInitialized()) {
            add(5, "Intel E1000E", "Gigabit Ethernet (82574L)");
        }

        // Display (category 6)
        if (Drivers::Graphics::IntelGPU::IsInitialized()) {
            auto* gpu = Drivers::Graphics::IntelGPU::GetGpuInfo();
            if (gpu) {
                add(6, gpu->name, "Intel Integrated Graphics");
            }
        }

        // PCI devices (category 7)
        auto& pciDevs = Pci::GetDevices();
        for (int i = 0; i < (int)pciDevs.size() && count < maxCount; i++) {
            auto& d = pciDevs[i];
            const char* className = Pci::GetClassName(d.ClassCode, d.SubClass);
            char detail[48];
            int p = 0;
            p = dl_append_hex(detail, p, d.Bus, 2, 48);
            p = dl_append(detail, p, ":", 48);
            p = dl_append_hex(detail, p, d.Device, 2, 48);
            p = dl_append(detail, p, ".", 48);
            p = dl_append_dec(detail, p, d.Function, 48);
            p = dl_append(detail, p, " ", 48);
            p = dl_append_hex(detail, p, d.VendorId, 4, 48);
            p = dl_append(detail, p, ":", 48);
            p = dl_append_hex(detail, p, d.DeviceId, 4, 48);
            add(7, className, detail);
        }

        return count;
    }

    // ---- Window scale syscalls ----

    static int Sys_WinSetScale(int scale) {
        return WinServer::SetScale(scale);
    }

    static int Sys_WinGetScale() {
        return WinServer::GetScale();
    }

    // ---- Window server syscalls ----

    static int Sys_WinCreate(const char* title, int w, int h, WinCreateResult* result) {
        if (result == nullptr || title == nullptr) return -1;
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return -1;

        uint64_t outVa = 0;
        int id = WinServer::Create(proc->pid, proc->pml4Phys, title, w, h,
                                   proc->heapNext, outVa);
        result->id = id;
        result->pixelVa = (id >= 0) ? outVa : 0;
        return id >= 0 ? 0 : -1;
    }

    static int Sys_WinDestroy(int windowId) {
        return WinServer::Destroy(windowId, Sched::GetCurrentPid());
    }

    static int Sys_WinPresent(int windowId) {
        return WinServer::Present(windowId, Sched::GetCurrentPid());
    }

    static int Sys_WinPoll(int windowId, WinEvent* outEvent) {
        if (outEvent == nullptr) return -1;
        return WinServer::Poll(windowId, Sched::GetCurrentPid(), outEvent);
    }

    static int Sys_WinEnum(WinInfo* outArray, int maxCount) {
        if (outArray == nullptr || maxCount <= 0) return 0;
        return WinServer::Enumerate(outArray, maxCount);
    }

    static uint64_t Sys_WinMap(int windowId) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;
        return WinServer::Map(windowId, proc->pid, proc->pml4Phys, proc->heapNext);
    }

    static int Sys_WinSendEvent(int windowId, const WinEvent* event) {
        if (event == nullptr) return -1;
        return WinServer::SendEvent(windowId, event);
    }

    static uint64_t Sys_WinResize(int windowId, int newW, int newH) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;
        uint64_t outVa = 0;
        int r = WinServer::Resize(windowId, proc->pid, proc->pml4Phys, newW, newH,
                                  proc->heapNext, outVa);
        return (r == 0) ? outVa : 0;
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
            case SYS_MOUSESTATE:
                Sys_MouseState((MouseState*)frame->arg1);
                return 0;
            case SYS_SETMOUSEBOUNDS:
                Sys_SetMouseBounds((int32_t)frame->arg1, (int32_t)frame->arg2);
                return 0;
            case SYS_SPAWN_REDIR:
                return (int64_t)Sys_SpawnRedir((const char*)frame->arg1, (const char*)frame->arg2);
            case SYS_CHILDIO_READ:
                return (int64_t)Sys_ChildIoRead((int)frame->arg1, (char*)frame->arg2, (int)frame->arg3);
            case SYS_CHILDIO_WRITE:
                return (int64_t)Sys_ChildIoWrite((int)frame->arg1, (const char*)frame->arg2, (int)frame->arg3);
            case SYS_CHILDIO_WRITEKEY:
                return (int64_t)Sys_ChildIoWriteKey((int)frame->arg1, (const KeyEvent*)frame->arg2);
            case SYS_CHILDIO_SETTERMSZ:
                return (int64_t)Sys_ChildIoSetTermsz((int)frame->arg1, (int)frame->arg2, (int)frame->arg3);
            case SYS_WINCREATE:
                return (int64_t)Sys_WinCreate((const char*)frame->arg1, (int)frame->arg2,
                                              (int)frame->arg3, (WinCreateResult*)frame->arg4);
            case SYS_WINDESTROY:
                return (int64_t)Sys_WinDestroy((int)frame->arg1);
            case SYS_WINPRESENT:
                return (int64_t)Sys_WinPresent((int)frame->arg1);
            case SYS_WINPOLL:
                return (int64_t)Sys_WinPoll((int)frame->arg1, (WinEvent*)frame->arg2);
            case SYS_WINENUM:
                return (int64_t)Sys_WinEnum((WinInfo*)frame->arg1, (int)frame->arg2);
            case SYS_WINMAP:
                return (int64_t)Sys_WinMap((int)frame->arg1);
            case SYS_WINSENDEVENT:
                return (int64_t)Sys_WinSendEvent((int)frame->arg1, (const WinEvent*)frame->arg2);
            case SYS_WINRESIZE:
                return (int64_t)Sys_WinResize((int)frame->arg1, (int)frame->arg2, (int)frame->arg3);
            case SYS_PROCLIST:
                return (int64_t)Sys_ProcList((ProcInfo*)frame->arg1, (int)frame->arg2);
            case SYS_KILL:
                return (int64_t)Sys_Kill((int)frame->arg1);
            case SYS_DEVLIST:
                return (int64_t)Sys_DevList((DevInfo*)frame->arg1, (int)frame->arg2);
            case SYS_WINSETSCALE:
                return (int64_t)Sys_WinSetScale((int)frame->arg1);
            case SYS_WINGETSCALE:
                return (int64_t)Sys_WinGetScale();
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
            << kcp::hex << (uint64_t)SyscallEntry << kcp::dec << ", 64 syscalls)";
    }

}
