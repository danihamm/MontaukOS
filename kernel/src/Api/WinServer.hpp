/*
    * WinServer.hpp
    * Window server kernel state for external process windows
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include "Syscall.hpp"
#include <cstdint>

namespace WinServer {

    static constexpr int MaxWindows = 8;
    static constexpr int MaxEvents = 64;
    static constexpr int MaxPixelPages = 2048; // up to 2048x1024 @ 32bpp = 8MB

    struct WindowSlot {
        bool used;
        int ownerPid;
        char title[64];
        int width, height;
        uint64_t pixelPhysPages[MaxPixelPages];
        int pixelNumPages;
        uint64_t ownerVa;      // VA in owner's address space
        uint64_t desktopVa;    // VA in desktop's address space (0 = not yet mapped)
        int desktopPid;        // PID of the process that mapped it
        Zenith::WinEvent events[MaxEvents];
        int eventHead, eventTail;
        bool dirty;
    };

    int Create(int ownerPid, uint64_t ownerPml4, const char* title, int w, int h,
               uint64_t& heapNext, uint64_t& outVa);
    int Destroy(int windowId, int callerPid);
    int Present(int windowId, int callerPid);
    int Poll(int windowId, int callerPid, Zenith::WinEvent* outEvent);
    int Enumerate(Zenith::WinInfo* outArray, int maxCount);
    uint64_t Map(int windowId, int callerPid, uint64_t callerPml4, uint64_t& heapNext);
    int SendEvent(int windowId, const Zenith::WinEvent* event);
    int Resize(int windowId, int callerPid, uint64_t ownerPml4, int newW, int newH,
               uint64_t& heapNext, uint64_t& outVa);
    void CleanupProcess(int pid);
    int SetScale(int scale);
    int GetScale();

}
