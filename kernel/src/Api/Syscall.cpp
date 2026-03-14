/*
    * Syscall.cpp
    * SYSCALL/SYSRET setup and number-based dispatch
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Syscall.hpp"
#include <Terminal/Terminal.hpp>
#include <Hal/MSR.hpp>
#include <Hal/GDT.hpp>

/* For common functions used by multiple syscall implementations*/
#include "Common.hpp"

/* Syscall impl. includes */
#include "Process.hpp"    // SYS_EXIT, SYS_YIELD, SYS_SLEEP_MS, SYS_GETPID, SYS_WAITPID, SYS_SPAWN, SYS_GETARGS, SYS_PROCLIST, SYS_KILL
#include "Terminal.hpp"   // SYS_PRINT, SYS_PUTCHAR
#include "Filesystem.hpp" // SYS_OPEN, SYS_READ, SYS_GETSIZE, SYS_CLOSE, SYS_READDIR, SYS_FWRITE, SYS_FCREATE
#include "Heap.hpp"       // SYS_ALLOC, SYS_FREE
#include "Time.hpp"       // SYS_GETTICKS, SYS_GETMILLISECONDS, SYS_GETTIME
#include "Keyboard.hpp"   // SYS_ISKEYAVAILABLE, SYS_GETKEY, SYS_GETCHAR
#include "Info.hpp"       // SYS_GETINFO
#include "Graphics.hpp"   // SYS_FBINFO, SYS_FBMAP, SYS_TERMSIZE, SYS_TERMSCALE
#include "Net.hpp"        // SYS_PING, SYS_SOCKET, SYS_CONNECT, SYS_BIND, SYS_LISTEN, SYS_ACCEPT, SYS_SEND, SYS_RECV, SYS_CLOSESOCK, SYS_SENDTO, SYS_RECVFROM, SYS_GETNETCFG, SYS_SETNETCFG, SYS_RESOLVE
#include "Power.hpp"      // SYS_RESET, SYS_SHUTDOWN
#include "Mouse.hpp"      // SYS_MOUSESTATE, SYS_SETMOUSEBOUNDS
#include "IoRedir.hpp"    // SYS_SPAWN_REDIR, SYS_CHILDIO_READ, SYS_CHILDIO_WRITE, SYS_CHILDIO_WRITEKEY, SYS_CHILDIO_SETTERMSZ
#include "Random.hpp"     // SYS_GETRANDOM
#include "MemInfo.hpp"    // SYS_MEMSTATS
#include "Device.hpp"     // SYS_DEVLIST, SYS_DISKINFO
#include "Storage.hpp"    // SYS_PARTLIST, SYS_DISKREAD, SYS_DISKWRITE
#include "Window.hpp"     // SYS_WINCREATE, SYS_WINDESTROY, SYS_WINPRESENT, SYS_WINPOLL, SYS_WINENUM, SYS_WINMAP, SYS_WINSENDEVENT, SYS_WINRESIZE, SYS_WINSETSCALE, SYS_WINGETSCALE
#include "Audio.hpp"      // SYS_AUDIOOPEN, SYS_AUDIOCLOSE, SYS_AUDIOWRITE, SYS_AUDIOCTL
#include "BluetoothSyscall.hpp" // SYS_BTSCAN, SYS_BTCONNECT, SYS_BTDISCONNECT, SYS_BTLIST, SYS_BTINFO

// Assembly entry point
extern "C" void SyscallEntry();

namespace Montauk {

    // ---- User pointer validation ----
    // Reject pointers that fall in the kernel-half of the address space
    // (canonical high addresses, i.e. >= 0x0000800000000000).
    // This prevents userspace from tricking the kernel into reading/writing
    // kernel memory via syscall arguments.

    static constexpr uint64_t USER_SPACE_END = 0x0000800000000000ULL;

    static bool IsUserPtr(uint64_t addr) {
        return addr == 0 || addr < USER_SPACE_END;
    }

    // Validate that a pointer is non-null and in user space
    static bool ValidUserPtr(uint64_t addr) {
        return addr != 0 && addr < USER_SPACE_END;
    }

    // ---- Dispatch ----

    extern "C" int64_t SyscallDispatch(SyscallFrame* frame) {
        switch (frame->syscall_nr) {
            case SYS_EXIT: {
                int slot = GetCurrentSlot();
                auto* proc = Sched::GetCurrentProcessPtr();
                if (slot >= 0 && proc)
                    CleanupHeapForSlot(slot, proc->pml4Phys);
                Sys_Exit((int)frame->arg1);
                return 0;
            }
            case SYS_YIELD:
                Sys_Yield();
                return 0;
            case SYS_SLEEP_MS:
                Sys_SleepMs(frame->arg1);
                return 0;
            case SYS_GETPID:
                return (int64_t)Sys_GetPid();
            case SYS_PRINT:
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_Print((const char*)frame->arg1);
                return 0;
            case SYS_PUTCHAR:
                Sys_Putchar((char)frame->arg1);
                return 0;
            case SYS_OPEN:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_Open((const char*)frame->arg1);
            case SYS_READ:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_Read((int)frame->arg1, (uint8_t*)frame->arg2,
                                         frame->arg3, frame->arg4);
            case SYS_GETSIZE:
                return (int64_t)Sys_GetSize((int)frame->arg1);
            case SYS_CLOSE:
                Sys_Close((int)frame->arg1);
                return 0;
            case SYS_READDIR:
                if (!ValidUserPtr(frame->arg1) || !ValidUserPtr(frame->arg2)) return -1;
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
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_GetInfo((SysInfo*)frame->arg1);
                return 0;
            case SYS_ISKEYAVAILABLE:
                return (int64_t)Sys_IsKeyAvailable();
            case SYS_GETKEY:
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_GetKey((KeyEvent*)frame->arg1);
                return 0;
            case SYS_GETCHAR:
                return (int64_t)Sys_GetChar();
            case SYS_PING:
                return (int64_t)Sys_Ping((uint32_t)frame->arg1, (uint32_t)frame->arg2);
            case SYS_SPAWN:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_Spawn((const char*)frame->arg1,
                                          IsUserPtr(frame->arg2) ? (const char*)frame->arg2 : nullptr);
            case SYS_WAITPID:
                Sys_WaitPid((int)frame->arg1);
                return 0;
            case SYS_FBINFO:
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_FbInfo((FbInfo*)frame->arg1);
                return 0;
            case SYS_FBMAP:
                return (int64_t)Sys_FbMap();
            case SYS_TERMSIZE:
                return (int64_t)Sys_TermSize();
            case SYS_GETARGS:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_GetArgs((char*)frame->arg1, frame->arg2);
            case SYS_RESET:
                Sys_Reset();
                return 0;
            case SYS_SHUTDOWN:
                Sys_Shutdown();
                return 0;
            case SYS_GETTIME:
                if (!ValidUserPtr(frame->arg1)) return -1;
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
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_Send((int)frame->arg1, (const uint8_t*)frame->arg2, (uint32_t)frame->arg3);
            case SYS_RECV:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_Recv((int)frame->arg1, (uint8_t*)frame->arg2, (uint32_t)frame->arg3);
            case SYS_CLOSESOCK:
                Sys_CloseSock((int)frame->arg1);
                return 0;
            case SYS_GETNETCFG:
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_GetNetCfg((NetCfg*)frame->arg1);
                return 0;
            case SYS_SETNETCFG:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_SetNetCfg((const NetCfg*)frame->arg1);
            case SYS_SENDTO:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_SendTo((int)frame->arg1, (const uint8_t*)frame->arg2,
                                           (uint32_t)frame->arg3, (uint32_t)frame->arg4,
                                           (uint16_t)frame->arg5);
            case SYS_RECVFROM:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_RecvFrom((int)frame->arg1, (uint8_t*)frame->arg2,
                                             (uint32_t)frame->arg3,
                                             IsUserPtr(frame->arg4) ? (uint32_t*)frame->arg4 : nullptr,
                                             IsUserPtr(frame->arg5) ? (uint16_t*)frame->arg5 : nullptr);
            case SYS_FWRITE:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_FWrite((int)frame->arg1, (const uint8_t*)frame->arg2,
                                           frame->arg3, frame->arg4);
            case SYS_FCREATE:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_FCreate((const char*)frame->arg1);
            case SYS_FDELETE:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_FDelete((const char*)frame->arg1);
            case SYS_FMKDIR:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_FMkdir((const char*)frame->arg1);
            case SYS_DRIVELIST:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_DriveList((int*)frame->arg1, (int)frame->arg2);
            case SYS_TERMSCALE:
                return Sys_TermScale(frame->arg1, frame->arg2);
            case SYS_RESOLVE:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_Resolve((const char*)frame->arg1);
            case SYS_GETRANDOM:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_GetRandom((uint8_t*)frame->arg1, frame->arg2);
            case SYS_KLOG:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Kt::ReadKernelLog((char*)frame->arg1, frame->arg2);
            case SYS_MOUSESTATE:
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_MouseState((MouseState*)frame->arg1);
                return 0;
            case SYS_SETMOUSEBOUNDS:
                Sys_SetMouseBounds((int32_t)frame->arg1, (int32_t)frame->arg2);
                return 0;
            case SYS_SPAWN_REDIR:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_SpawnRedir((const char*)frame->arg1,
                                               IsUserPtr(frame->arg2) ? (const char*)frame->arg2 : nullptr);
            case SYS_CHILDIO_READ:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_ChildIoRead((int)frame->arg1, (char*)frame->arg2, (int)frame->arg3);
            case SYS_CHILDIO_WRITE:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_ChildIoWrite((int)frame->arg1, (const char*)frame->arg2, (int)frame->arg3);
            case SYS_CHILDIO_WRITEKEY:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_ChildIoWriteKey((int)frame->arg1, (const KeyEvent*)frame->arg2);
            case SYS_CHILDIO_SETTERMSZ:
                return (int64_t)Sys_ChildIoSetTermsz((int)frame->arg1, (int)frame->arg2, (int)frame->arg3);
            case SYS_WINCREATE:
                if (!ValidUserPtr(frame->arg1) || !ValidUserPtr(frame->arg4)) return -1;
                return (int64_t)Sys_WinCreate((const char*)frame->arg1, (int)frame->arg2,
                                              (int)frame->arg3, (WinCreateResult*)frame->arg4);
            case SYS_WINDESTROY:
                return (int64_t)Sys_WinDestroy((int)frame->arg1);
            case SYS_WINPRESENT:
                return (int64_t)Sys_WinPresent((int)frame->arg1);
            case SYS_WINPOLL:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_WinPoll((int)frame->arg1, (WinEvent*)frame->arg2);
            case SYS_WINENUM:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_WinEnum((WinInfo*)frame->arg1, (int)frame->arg2);
            case SYS_WINMAP:
                return (int64_t)Sys_WinMap((int)frame->arg1);
            case SYS_WINSENDEVENT:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return (int64_t)Sys_WinSendEvent((int)frame->arg1, (const WinEvent*)frame->arg2);
            case SYS_WINRESIZE:
                return (int64_t)Sys_WinResize((int)frame->arg1, (int)frame->arg2, (int)frame->arg3);
            case SYS_PROCLIST:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_ProcList((ProcInfo*)frame->arg1, (int)frame->arg2);
            case SYS_KILL: {
                // Free heap allocations for the target process before killing it
                auto* target = Sched::GetProcessByPid((int)frame->arg1);
                if (target) {
                    auto* slot0 = Sched::GetProcessSlot(0);
                    int targetSlot = (int)(target - slot0);
                    CleanupHeapForSlot(targetSlot, target->pml4Phys);
                }
                return (int64_t)Sys_Kill((int)frame->arg1);
            }
            case SYS_DEVLIST:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_DevList((DevInfo*)frame->arg1, (int)frame->arg2);
            case SYS_DISKINFO:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_DiskInfo((DiskInfo*)frame->arg1, (int)frame->arg2);
            case SYS_WINSETSCALE:
                return (int64_t)Sys_WinSetScale((int)frame->arg1);
            case SYS_WINGETSCALE:
                return (int64_t)Sys_WinGetScale();
            case SYS_WINSETCURSOR:
                return (int64_t)Sys_WinSetCursor((int)frame->arg1, (int)frame->arg2);
            case SYS_MEMSTATS:
                if (!ValidUserPtr(frame->arg1)) return -1;
                Sys_MemStats((MemStats*)frame->arg1);
                return 0;
            case SYS_PARTLIST:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_PartList((PartInfo*)frame->arg1, (int)frame->arg2);
            case SYS_DISKREAD:
                if (!ValidUserPtr(frame->arg4)) return -1;
                return (int64_t)Sys_DiskRead((int)frame->arg1, frame->arg2,
                                             (uint32_t)frame->arg3, (void*)frame->arg4);
            case SYS_DISKWRITE:
                if (!ValidUserPtr(frame->arg4)) return -1;
                return (int64_t)Sys_DiskWrite((int)frame->arg1, frame->arg2,
                                              (uint32_t)frame->arg3, (const void*)frame->arg4);
            case SYS_GPTINIT:
                return (int64_t)Sys_GptInit((int)frame->arg1);
            case SYS_GPTADD:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_GptAdd((const GptAddParams*)frame->arg1);
            case SYS_FSMOUNT:
                return (int64_t)Sys_FsMount((int)frame->arg1, (int)frame->arg2);
            case SYS_FSFORMAT:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return (int64_t)Sys_FsFormat((const FsFormatParams*)frame->arg1);
            case SYS_AUDIOOPEN:
                return Sys_AudioOpen((uint32_t)frame->arg1, (uint8_t)frame->arg2, (uint8_t)frame->arg3);
            case SYS_AUDIOCLOSE:
                return Sys_AudioClose((int)frame->arg1);
            case SYS_AUDIOWRITE:
                if (!ValidUserPtr(frame->arg2)) return -1;
                return Sys_AudioWrite((int)frame->arg1, (const uint8_t*)frame->arg2, (uint32_t)frame->arg3);
            case SYS_AUDIOCTL:
                return Sys_AudioCtl((int)frame->arg1, (int)frame->arg2, (int)frame->arg3);
            case SYS_BTSCAN:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_BtScan((BtScanResult*)frame->arg1, (int)frame->arg2, (uint32_t)frame->arg3);
            case SYS_BTCONNECT:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_BtConnect((const uint8_t*)frame->arg1);
            case SYS_BTDISCONNECT:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_BtDisconnect((const uint8_t*)frame->arg1);
            case SYS_BTLIST:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_BtList((BtDevInfo*)frame->arg1, (int)frame->arg2);
            case SYS_BTINFO:
                if (!ValidUserPtr(frame->arg1)) return -1;
                return Sys_BtInfo((BtAdapterInfo*)frame->arg1);
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
