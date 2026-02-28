/*
    * IoRedir.hpp
    * SYS_SPAWN_REDIR, SYS_CHILDIO_READ, SYS_CHILDIO_WRITE,
    * SYS_CHILDIO_WRITEKEY, SYS_CHILDIO_SETTERMSZ syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>
#include <Memory/PageFrameAllocator.hpp>

#include "Syscall.hpp"
#include "Common.hpp"

namespace Montauk {

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
};
