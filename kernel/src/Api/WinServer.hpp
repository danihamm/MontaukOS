/*
    * WinServer.hpp
    * Window server kernel state for external process windows
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "Syscall.hpp"
#include <cstdint>
#include <Ipc/Ipc.hpp>

namespace WinServer {

    static constexpr int MaxWindows = 8;

    struct WindowSlot {
        bool used;
        int ownerPid;
        char title[64];
        int width, height;
        Ipc::Surface* liveSurface;      // app writes here
        Ipc::Surface* snapshotSurface;  // compositor/viewers read here
        Ipc::Mailbox* eventMailbox;     // input/control events for the app
        uint64_t ownerVa;               // mapped VA of liveSurface in owner
        bool dirty;
        uint8_t cursor;                 // 0=arrow, 1=resize_h, 2=resize_v
    };

    int Create(int ownerPid, uint64_t ownerPml4, const char* title, int w, int h,
               uint64_t& heapNext, uint64_t& outVa);
    int Destroy(int windowId, int callerPid);
    int Present(int windowId, int callerPid);
    int Poll(int windowId, int callerPid, Montauk::WinEvent* outEvent);
    int Enumerate(Montauk::WinInfo* outArray, int maxCount);
    uint64_t Map(int windowId, int callerPid, uint64_t callerPml4, uint64_t& heapNext);
    int Unmap(int windowId, int callerPid, uint64_t callerPml4);
    int SendEvent(int windowId, const Montauk::WinEvent* event);
    int Resize(int windowId, int callerPid, uint64_t ownerPml4, int newW, int newH,
               uint64_t& heapNext, uint64_t& outVa);
    void CleanupProcess(int pid);
    int SetCursor(int windowId, int callerPid, int cursor);
    int SetScale(int scale);
    int GetScale();

}
