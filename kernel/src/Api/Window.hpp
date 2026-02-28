/*
    * Window.hpp
    * SYS_WINCREATE, SYS_WINDESTROY, SYS_WINPRESENT, SYS_WINPOLL,
    * SYS_WINENUM, SYS_WINMAP, SYS_WINSENDEVENT, SYS_WINRESIZE,
    * SYS_WINSETSCALE, SYS_WINGETSCALE syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>

#include "Syscall.hpp"
#include "WinServer.hpp"

namespace Montauk {

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

    static uint64_t Sys_WinPresent(int windowId) {
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

    static int Sys_WinSetScale(int scale) {
        return WinServer::SetScale(scale);
    }

    static int Sys_WinGetScale() {
        return WinServer::GetScale();
    }
};
