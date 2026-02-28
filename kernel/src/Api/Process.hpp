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

#include "Syscall.hpp"
#include "WinServer.hpp"

namespace Montauk {
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

    static void Sys_WaitPid(int pid) {
        while (Sched::IsAlive(pid)) {
            Sched::Schedule();  // yield until the process exits
        }
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

        // Clean up any windows owned by this process (unmaps pixel pages from desktop)
        WinServer::CleanupProcess(pid);

        // Free I/O redirect buffers
        if (proc->outBuf) {
            Memory::g_pfa->Free(proc->outBuf);
            proc->outBuf = nullptr;
        }
        if (proc->inBuf) {
            Memory::g_pfa->Free(proc->inBuf);
            proc->inBuf = nullptr;
        }

        // Free all user-space pages and page table structures
        Memory::VMM::Paging::FreeUserHalf(proc->pml4Phys);

        // Free kernel stack (safe — killed process isn't running on single-core)
        if (proc->stackBase != 0) {
            Memory::g_pfa->Free((void*)proc->stackBase, Sched::StackPages);
            proc->stackBase = 0;
        }

        // Free the PML4 page
        if (proc->pml4Phys != 0) {
            Memory::g_pfa->Free((void*)Memory::HHDM(proc->pml4Phys));
            proc->pml4Phys = 0;
        }

        proc->state = Sched::ProcessState::Terminated;
        return 0;
    }
};