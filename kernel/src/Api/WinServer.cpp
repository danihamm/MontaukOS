/*
    * WinServer.cpp
    * Window server kernel implementation for external process windows
    * Copyright (c) 2026 Daniel Hammer
*/

#include "WinServer.hpp"
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <Sched/Scheduler.hpp>

namespace WinServer {

    static WindowSlot g_slots[MaxWindows];
    static int g_uiScale = 1;

    int Create(int ownerPid, uint64_t ownerPml4, const char* title, int w, int h,
               uint64_t& heapNext, uint64_t& outVa) {
        // Find a free slot
        int slotIdx = -1;
        for (int i = 0; i < MaxWindows; i++) {
            if (!g_slots[i].used) {
                slotIdx = i;
                break;
            }
        }
        if (slotIdx < 0) return -1;

        // Validate dimensions (cap at 16384 to prevent integer overflow in w*h*4)
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;
        uint64_t bufSize = (uint64_t)w * h * 4;
        int numPages = (int)((bufSize + 0xFFF) / 0x1000);
        if (numPages > MaxPixelPages) return -1;

        WindowSlot& slot = g_slots[slotIdx];
        memset(&slot, 0, sizeof(WindowSlot));
        slot.used = true;
        slot.ownerPid = ownerPid;
        slot.width = w;
        slot.height = h;
        slot.pixelNumPages = numPages;
        slot.eventHead = 0;
        slot.eventTail = 0;
        slot.dirty = false;
        slot.desktopVa = 0;
        slot.desktopPid = 0;

        // Copy title
        int tlen = 0;
        while (title[tlen] && tlen < 63) {
            slot.title[tlen] = title[tlen];
            tlen++;
        }
        slot.title[tlen] = '\0';

        // Allocate physical pages and map into owner's address space
        uint64_t userVa = heapNext;
        for (int i = 0; i < numPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) {
                // Cleanup on failure - mark slot unused
                slot.used = false;
                return -1;
            }
            uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
            slot.pixelPhysPages[i] = physAddr;
            if (!Memory::VMM::Paging::MapUserIn(ownerPml4, physAddr, userVa + (uint64_t)i * 0x1000)) {
                slot.used = false;
                return -1;
            }
        }

        slot.ownerVa = userVa;
        heapNext += (uint64_t)numPages * 0x1000;
        outVa = userVa;

        Kt::KernelLogStream(Kt::OK, "WinServer") << "Created window " << slotIdx
            << " (" << w << "x" << h << ") for PID " << ownerPid;

        return slotIdx;
    }

    int Destroy(int windowId, int callerPid) {
        if (windowId < 0 || windowId >= MaxWindows) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;

        // Unmap pixel pages from desktop's address space
        if (slot.desktopVa != 0 && slot.desktopPid != 0) {
            auto* desktopProc = Sched::GetProcessByPid(slot.desktopPid);
            if (desktopProc) {
                for (int p = 0; p < slot.pixelNumPages; p++) {
                    Memory::VMM::Paging::UnmapUserIn(
                        desktopProc->pml4Phys,
                        slot.desktopVa + (uint64_t)p * 0x1000);
                }
            }
        }

        // Unmap pixel pages from the owner's address space so that
        // FreeUserHalf() won't double-free them when the process exits.
        {
            auto* ownerProc = Sched::GetProcessByPid(slot.ownerPid);
            if (ownerProc) {
                for (int p = 0; p < slot.pixelNumPages; p++) {
                    Memory::VMM::Paging::UnmapUserIn(
                        ownerProc->pml4Phys,
                        slot.ownerVa + (uint64_t)p * 0x1000);
                }
            }
        }

        // Free physical pixel pages
        for (int i = 0; i < slot.pixelNumPages; i++) {
            if (slot.pixelPhysPages[i] != 0) {
                Memory::g_pfa->Free((void*)Memory::HHDM(slot.pixelPhysPages[i]));
            }
        }

        slot.used = false;
        return 0;
    }

    int Present(int windowId, int callerPid) {
        if (windowId < 0 || windowId >= MaxWindows) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;

        slot.dirty = true;
        return 0;
    }

    int Poll(int windowId, int callerPid, Montauk::WinEvent* outEvent) {
        if (windowId < 0 || windowId >= MaxWindows) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;

        if (slot.eventHead == slot.eventTail) return 0; // no events

        *outEvent = slot.events[slot.eventTail];
        slot.eventTail = (slot.eventTail + 1) % MaxEvents;
        return 1;
    }

    int Enumerate(Montauk::WinInfo* outArray, int maxCount) {
        int count = 0;
        for (int i = 0; i < MaxWindows && count < maxCount; i++) {
            if (!g_slots[i].used) continue;
            Montauk::WinInfo& info = outArray[count];
            info.id = i;
            info.ownerPid = g_slots[i].ownerPid;
            for (int j = 0; j < 64; j++) info.title[j] = g_slots[i].title[j];
            info.width = g_slots[i].width;
            info.height = g_slots[i].height;
            info.dirty = g_slots[i].dirty ? 1 : 0;
            info.cursor = g_slots[i].cursor;
            g_slots[i].dirty = false; // clear dirty after read
            count++;
        }
        return count;
    }

    uint64_t Map(int windowId, int callerPid, uint64_t callerPml4, uint64_t& heapNext) {
        if (windowId < 0 || windowId >= MaxWindows) return 0;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used) return 0;

        // If already mapped into this process, return existing VA
        if (slot.desktopPid == callerPid && slot.desktopVa != 0) {
            return slot.desktopVa;
        }

        uint64_t userVa = heapNext;

        for (int i = 0; i < slot.pixelNumPages; i++) {
            if (!Memory::VMM::Paging::MapUserIn(callerPml4, slot.pixelPhysPages[i],
                                           userVa + (uint64_t)i * 0x1000)) {
                return 0;
            }
        }

        slot.desktopVa = userVa;
        slot.desktopPid = callerPid;
        heapNext += (uint64_t)slot.pixelNumPages * 0x1000;

        return userVa;
    }

    int SendEvent(int windowId, const Montauk::WinEvent* event) {
        if (windowId < 0 || windowId >= MaxWindows) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used) return -1;

        int nextHead = (slot.eventHead + 1) % MaxEvents;
        if (nextHead == slot.eventTail) return -1; // queue full, drop event

        slot.events[slot.eventHead] = *event;
        slot.eventHead = nextHead;
        return 0;
    }

    int Resize(int windowId, int callerPid, uint64_t ownerPml4, int newW, int newH,
               uint64_t& heapNext, uint64_t& outVa) {
        if (windowId < 0 || windowId >= MaxWindows) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;
        if (newW <= 0 || newH <= 0 || newW > 16384 || newH > 16384) return -1;
        if (newW == slot.width && newH == slot.height) {
            outVa = slot.ownerVa;
            return 0;
        }

        uint64_t bufSize = (uint64_t)newW * newH * 4;
        int numPages = (int)((bufSize + 0xFFF) / 0x1000);
        if (numPages > MaxPixelPages) return -1;

        // Unmap old pixel pages from owner's address space, then free them
        int oldNumPages = slot.pixelNumPages;
        for (int i = 0; i < oldNumPages; i++) {
            Memory::VMM::Paging::UnmapUserIn(
                ownerPml4, slot.ownerVa + (uint64_t)i * 0x1000);
            if (slot.pixelPhysPages[i] != 0) {
                Memory::g_pfa->Free((void*)Memory::HHDM(slot.pixelPhysPages[i]));
                slot.pixelPhysPages[i] = 0;
            }
        }

        // Allocate new pages and map into owner's address space
        uint64_t userVa = heapNext;
        for (int i = 0; i < numPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) return -1;
            uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
            slot.pixelPhysPages[i] = physAddr;
            if (!Memory::VMM::Paging::MapUserIn(ownerPml4, physAddr, userVa + (uint64_t)i * 0x1000)) {
                return -1;
            }
        }

        slot.width = newW;
        slot.height = newH;
        slot.pixelNumPages = numPages;
        slot.ownerVa = userVa;
        heapNext += (uint64_t)numPages * 0x1000;

        // Invalidate desktop mapping so it re-maps on next enumerate
        slot.desktopVa = 0;
        slot.desktopPid = 0;

        outVa = userVa;
        return 0;
    }

    int SetCursor(int windowId, int callerPid, int cursor) {
        if (windowId < 0 || windowId >= MaxWindows) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;
        slot.cursor = (uint8_t)cursor;
        return 0;
    }

    int SetScale(int scale) {
        if (scale < 0) scale = 0;
        if (scale > 2) scale = 2;
        g_uiScale = scale;

        // Broadcast scale event to all active windows
        Montauk::WinEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = 4;
        ev.scale.scale = scale;
        for (int i = 0; i < MaxWindows; i++) {
            if (g_slots[i].used) {
                SendEvent(i, &ev);
            }
        }
        return 0;
    }

    int GetScale() {
        return g_uiScale;
    }

    void CleanupProcess(int pid) {
        for (int i = 0; i < MaxWindows; i++) {
            if (g_slots[i].used && g_slots[i].ownerPid == pid) {
                Kt::KernelLogStream(Kt::INFO, "WinServer") << "Cleaning up window "
                    << i << " for exited PID " << pid;

                // Unmap pixel pages from desktop's address space to prevent stale access
                if (g_slots[i].desktopVa != 0 && g_slots[i].desktopPid != 0) {
                    auto* desktopProc = Sched::GetProcessByPid(g_slots[i].desktopPid);
                    if (desktopProc) {
                        for (int p = 0; p < g_slots[i].pixelNumPages; p++) {
                            Memory::VMM::Paging::UnmapUserIn(
                                desktopProc->pml4Phys,
                                g_slots[i].desktopVa + (uint64_t)p * 0x1000);
                        }
                    }
                }

                // Do NOT free physical pixel pages here — they are still mapped
                // in the owner's page tables and FreeUserHalf() (called right
                // after CleanupProcess) will free them.  Freeing here would
                // cause a double-free, creating a cycle in the PFA free list.

                g_slots[i].used = false;
            }
        }
    }

}
