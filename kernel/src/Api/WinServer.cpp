/*
    * WinServer.cpp
    * Window server kernel implementation for external process windows
    * Copyright (c) 2026 Daniel Hammer
*/

#include "WinServer.hpp"
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Spinlock.hpp>
#include <Sched/Scheduler.hpp>

namespace WinServer {

    static WindowSlot g_slots[MaxWindows];
    static int g_uiScale = 1;
    static kcp::Mutex wsLock;
    static constexpr int MaxRetiredSnapshots = MaxWindows * 4;

    struct RetiredSnapshot {
        bool used;
        int windowId;
        Ipc::Surface* surface;
    };

    static RetiredSnapshot g_retiredSnapshots[MaxRetiredSnapshots];

    struct WsGuard {
        WsGuard()  { wsLock.Acquire(); }
        ~WsGuard() { wsLock.Release(); }
    };

    static void ResetSlotLocked(WindowSlot& slot) {
        memset(&slot, 0, sizeof(WindowSlot));
    }

    static void RetireSnapshotLocked(int windowId, Ipc::Surface* surface) {
        if (surface == nullptr) return;

        for (int i = 0; i < MaxRetiredSnapshots; i++) {
            if (g_retiredSnapshots[i].used) continue;
            g_retiredSnapshots[i].used = true;
            g_retiredSnapshots[i].windowId = windowId;
            g_retiredSnapshots[i].surface = surface;
            return;
        }

        Ipc::ReleaseSurface(surface);
    }

    static int UnmapRetiredSnapshotsLocked(int windowId, int callerPid, uint64_t callerPml4) {
        int result = -1;
        for (int i = 0; i < MaxRetiredSnapshots; i++) {
            if (!g_retiredSnapshots[i].used || g_retiredSnapshots[i].windowId != windowId) continue;

            if (g_retiredSnapshots[i].surface != nullptr) {
                if (Ipc::UnmapSurfaceForPid(g_retiredSnapshots[i].surface, callerPid, callerPml4) == 0) {
                    result = 0;
                }
                Ipc::ReleaseSurface(g_retiredSnapshots[i].surface);
            }

            g_retiredSnapshots[i].used = false;
            g_retiredSnapshots[i].windowId = -1;
            g_retiredSnapshots[i].surface = nullptr;
        }
        return result;
    }

    static void ReleaseSlotResourcesLocked(WindowSlot& slot, bool unmapOwner) {
        if (unmapOwner && slot.liveSurface != nullptr) {
            auto* owner = Sched::GetProcessByPid(slot.ownerPid);
            if (owner != nullptr) {
                Ipc::UnmapSurfaceForPid(slot.liveSurface, slot.ownerPid, owner->pml4Phys);
            }
        }

        if (slot.eventMailbox != nullptr) {
            Ipc::ReleaseMailbox(slot.eventMailbox, true, true);
        }
        if (slot.liveSurface != nullptr) {
            Ipc::ReleaseSurface(slot.liveSurface);
        }
        if (slot.snapshotSurface != nullptr) {
            RetireSnapshotLocked((int)(&slot - g_slots), slot.snapshotSurface);
        }

        ResetSlotLocked(slot);
    }

    static int SendEventLocked(int windowId, const Montauk::WinEvent* event) {
        if (windowId < 0 || windowId >= MaxWindows || event == nullptr) return -1;
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.eventMailbox == nullptr) return -1;

        int rc = Ipc::MailboxSend(slot.eventMailbox, 0, event, sizeof(*event));
        return rc > 0 ? 0 : -1;
    }

    int Create(int ownerPid, uint64_t ownerPml4, const char* title, int w, int h,
               uint64_t& heapNext, uint64_t& outVa) {
        WsGuard guard;

        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;

        int slotIdx = -1;
        for (int i = 0; i < MaxWindows; i++) {
            if (!g_slots[i].used) {
                slotIdx = i;
                break;
            }
        }
        if (slotIdx < 0) return -1;

        uint64_t bufSize = (uint64_t)w * (uint64_t)h * 4ULL;
        Ipc::Surface* live = Ipc::CreateSurface(bufSize);
        Ipc::Surface* snapshot = Ipc::CreateSurface(bufSize);
        Ipc::Mailbox* mailbox = Ipc::CreateMailbox();
        if (live == nullptr || snapshot == nullptr || mailbox == nullptr) {
            if (live != nullptr) Ipc::ReleaseSurface(live);
            if (snapshot != nullptr) Ipc::ReleaseSurface(snapshot);
            if (mailbox != nullptr) Ipc::ReleaseMailbox(mailbox, false, false);
            return -1;
        }

        Ipc::RetainSurface(live);
        Ipc::RetainSurface(snapshot);
        Ipc::RetainMailbox(mailbox, true, true);

        uint64_t userVa = 0;
        if (Ipc::MapSurfaceForPid(live, ownerPid, ownerPml4, heapNext, userVa) != 0) {
            Ipc::ReleaseMailbox(mailbox, true, true);
            Ipc::ReleaseSurface(snapshot);
            Ipc::ReleaseSurface(live);
            return -1;
        }

        WindowSlot& slot = g_slots[slotIdx];
        ResetSlotLocked(slot);
        slot.used = true;
        slot.ownerPid = ownerPid;
        slot.width = w;
        slot.height = h;
        slot.liveSurface = live;
        slot.snapshotSurface = snapshot;
        slot.eventMailbox = mailbox;
        slot.ownerVa = userVa;
        slot.dirty = false;
        slot.cursor = 0;

        int tlen = 0;
        while (title[tlen] && tlen < 63) {
            slot.title[tlen] = title[tlen];
            tlen++;
        }
        slot.title[tlen] = '\0';

        outVa = userVa;
        return slotIdx;
    }

    int Destroy(int windowId, int callerPid) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows) return -1;

        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;

        ReleaseSlotResourcesLocked(slot, true);
        return 0;
    }

    int Present(int windowId, int callerPid) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows) return -1;

        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid ||
            slot.liveSurface == nullptr || slot.snapshotSurface == nullptr) {
            return -1;
        }

        if (Ipc::CopySurface(slot.snapshotSurface, slot.liveSurface) != 0) return -1;
        slot.dirty = true;
        return 0;
    }

    int Poll(int windowId, int callerPid, Montauk::WinEvent* outEvent) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows || outEvent == nullptr) return -1;

        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid || slot.eventMailbox == nullptr) return -1;

        uint16_t len = sizeof(*outEvent);
        int rc = Ipc::MailboxRecv(slot.eventMailbox, nullptr, outEvent, &len, true);
        if (rc < 0) return -1;
        return rc > 0 ? 1 : 0;
    }

    int Enumerate(Montauk::WinInfo* outArray, int maxCount) {
        WsGuard guard;
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
            g_slots[i].dirty = false;
            count++;
        }
        return count;
    }

    uint64_t Map(int windowId, int callerPid, uint64_t callerPml4, uint64_t& heapNext) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows) return 0;

        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.snapshotSurface == nullptr) return 0;

        uint64_t outVa = 0;
        if (Ipc::MapSurfaceForPid(slot.snapshotSurface, callerPid, callerPml4, heapNext, outVa) != 0) {
            return 0;
        }
        return outVa;
    }

    int Unmap(int windowId, int callerPid, uint64_t callerPml4) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows) {
            return UnmapRetiredSnapshotsLocked(windowId, callerPid, callerPml4);
        }

        int result = UnmapRetiredSnapshotsLocked(windowId, callerPid, callerPml4);
        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.snapshotSurface == nullptr) return result;

        int current = Ipc::UnmapSurfaceForPid(slot.snapshotSurface, callerPid, callerPml4);
        if (current == 0) return 0;
        return result;
    }

    int SendEvent(int windowId, const Montauk::WinEvent* event) {
        WsGuard guard;
        return SendEventLocked(windowId, event);
    }

    int Resize(int windowId, int callerPid, uint64_t ownerPml4, int newW, int newH,
               uint64_t& heapNext, uint64_t& outVa) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows) return -1;

        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;
        if (newW <= 0 || newH <= 0 || newW > 16384 || newH > 16384) return -1;
        if (newW == slot.width && newH == slot.height) {
            outVa = slot.ownerVa;
            return 0;
        }

        uint64_t bufSize = (uint64_t)newW * (uint64_t)newH * 4ULL;
        Ipc::Surface* newLive = Ipc::CreateSurface(bufSize);
        Ipc::Surface* newSnapshot = Ipc::CreateSurface(bufSize);
        if (newLive == nullptr || newSnapshot == nullptr) {
            if (newLive != nullptr) Ipc::ReleaseSurface(newLive);
            if (newSnapshot != nullptr) Ipc::ReleaseSurface(newSnapshot);
            return -1;
        }

        Ipc::RetainSurface(newLive);
        Ipc::RetainSurface(newSnapshot);

        uint64_t newOwnerVa = 0;
        if (Ipc::MapSurfaceForPid(newLive, callerPid, ownerPml4, heapNext, newOwnerVa) != 0) {
            Ipc::ReleaseSurface(newSnapshot);
            Ipc::ReleaseSurface(newLive);
            return -1;
        }

        if (slot.liveSurface != nullptr) {
            Ipc::UnmapSurfaceForPid(slot.liveSurface, callerPid, ownerPml4);
            Ipc::ReleaseSurface(slot.liveSurface);
        }
        if (slot.snapshotSurface != nullptr) {
            RetireSnapshotLocked(windowId, slot.snapshotSurface);
        }

        slot.liveSurface = newLive;
        slot.snapshotSurface = newSnapshot;
        slot.width = newW;
        slot.height = newH;
        slot.ownerVa = newOwnerVa;
        slot.dirty = true;

        outVa = newOwnerVa;
        return 0;
    }

    int SetCursor(int windowId, int callerPid, int cursor) {
        WsGuard guard;
        if (windowId < 0 || windowId >= MaxWindows) return -1;

        WindowSlot& slot = g_slots[windowId];
        if (!slot.used || slot.ownerPid != callerPid) return -1;
        slot.cursor = (uint8_t)cursor;
        return 0;
    }

    int SetScale(int scale) {
        WsGuard guard;
        if (scale < 0) scale = 0;
        if (scale > 2) scale = 2;
        g_uiScale = scale;

        Montauk::WinEvent ev{};
        ev.type = 4;
        ev.scale.scale = scale;
        for (int i = 0; i < MaxWindows; i++) {
            if (g_slots[i].used) {
                SendEventLocked(i, &ev);
            }
        }
        return 0;
    }

    int GetScale() {
        return g_uiScale;
    }

    void CleanupProcess(int pid) {
        WsGuard guard;
        for (int i = 0; i < MaxWindows; i++) {
            if (!g_slots[i].used || g_slots[i].ownerPid != pid) continue;
            ReleaseSlotResourcesLocked(g_slots[i], false);
        }
    }

}
