/*
    * Process.hpp
    * SYS_EXIT, SYS_YIELD, SYS_SLEEP_MS, SYS_GETPID,
    * SYS_WAITPID, SYS_SPAWN, SYS_GETARGS, SYS_PROCLIST, SYS_KILL syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Memory/Paging.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/HHDM.hpp>
#include <Fs/Vfs.hpp>

#include "Syscall.hpp"
#include "Common.hpp"
#include "WinServer.hpp"
#include "Path.hpp"

namespace Montauk {
    static void Sys_Exit(int exitCode) {
        (void)exitCode;
        Sched::ExitProcess();
    }

    static void Sys_Yield() {
        Sched::Schedule();
    }

    static void Sys_SleepMs(uint64_t ms) {
        Sched::BlockForSleep(ms);
    }

    static int Sys_GetPid() {
        return Sched::GetCurrentPid();
    }

    static void Sys_WaitPid(int pid) {
        Sched::BlockOnPid(pid);
    }

    static int Sys_Spawn(const char* path, const char* args) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;

        auto* parent = Sched::GetCurrentProcessPtr();
        int parentSlot = Ipc::CurrentSlot();
        int childPid = Sched::Spawn(resolved, args);
        if (childPid < 0) return childPid;

        if (parent && parent->redirected) {
            auto* child = Sched::GetProcessByPid(childPid);
            int childSlot = Ipc::SlotForPid(childPid);
            if (child == nullptr || childSlot < 0 || parentSlot < 0) {
                Sched::KillProcess(childPid);
                return -1;
            }

            child->ioOutHandle = DuplicateHandleBetweenSlots(parentSlot, parent->ioOutHandle, childSlot);
            child->ioInHandle = DuplicateHandleBetweenSlots(parentSlot, parent->ioInHandle, childSlot);
            child->ioKeyHandle = DuplicateHandleBetweenSlots(parentSlot, parent->ioKeyHandle, childSlot);

            if (child->ioOutHandle < 0 || child->ioInHandle < 0 || child->ioKeyHandle < 0 ||
                !ConfigureRedirWaitsetForSlot(childSlot, child)) {
                Sched::KillProcess(childPid);
                return -1;
            }

            child->redirected = true;
            child->parentPid = parent->pid;
            child->termCols = parent->termCols;
            child->termRows = parent->termRows;
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
            buf[count].heapUsed = Sched::g_allocatedPages[i] * 0x1000;
            count++;
        }
        return count;
    }

    static int Sys_Kill(int pid) {
        return Sched::KillProcess(pid);
    }

    static int Sys_SetUser(int pid, const char* name) {
        if (name == nullptr) return -1;
        auto* target = Sched::GetProcessByPid(pid);
        if (target == nullptr) return -1;
        int i = 0;
        for (; i < 31 && name[i]; i++)
            target->user[i] = name[i];
        target->user[i] = '\0';
        return 0;
    }

    static int Sys_GetUser(char* buf, uint64_t maxLen) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr || buf == nullptr || maxLen == 0) return -1;
        int i = 0;
        for (; i < (int)maxLen - 1 && proc->user[i]; i++)
            buf[i] = proc->user[i];
        buf[i] = '\0';
        return i;
    }

    static int Sys_GetCwd(char* buf, uint64_t maxLen) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr || buf == nullptr || maxLen == 0) return -1;

        const char* cwd = proc->cwd[0] ? proc->cwd : "0:/";
        int i = 0;
        for (; i < (int)maxLen - 1 && cwd[i]; i++) buf[i] = cwd[i];
        buf[i] = '\0';
        return i;
    }

    static int Sys_Chdir(const char* path) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr || path == nullptr) return -1;

        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;

        bool isDriveRoot = false;
        {
            int prefixLen = 0;
            if (ParseDrivePrefix(resolved, &prefixLen) >= 0 &&
                resolved[prefixLen] == '/' && resolved[prefixLen + 1] == '\0') {
                isDriveRoot = true;
            }
        }

        if (isDriveRoot) {
            const char* entries[1];
            if (Fs::Vfs::VfsReadDir(resolved, entries, 1) < 0) return -1;
        } else {
            Fs::Vfs::BackendFile file = {-1, -1};
            if (Fs::Vfs::OpenBackendFile(resolved, file) < 0) return -1;
            Fs::Vfs::CloseBackendFile(file);
        }

        int i = 0;
        for (; i < 255 && resolved[i]; i++) proc->cwd[i] = resolved[i];
        proc->cwd[i] = '\0';
        return 0;
    }
};
