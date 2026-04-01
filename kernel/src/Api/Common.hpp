#pragma once

#include <Sched/Scheduler.hpp>
#include <Ipc/Ipc.hpp>

namespace Montauk {

    static Sched::Process* GetRedirTarget(Sched::Process* proc) {
        if (!proc || !proc->redirected) return nullptr;
        if (proc->ioOutHandle >= 0 || proc->ioInHandle >= 0 || proc->ioKeyHandle >= 0) {
            return proc;
        }
        return (proc->parentPid >= 0) ? Sched::GetProcessByPid(proc->parentPid) : nullptr;
    }

    static bool ResolveProcessHandle(Sched::Process* proc, int handle, Ipc::HandleType expectedType,
                                     Ipc::Object*& outObject, uint32_t* outRights = nullptr) {
        if (proc == nullptr || handle < 0) return false;

        int slot = Ipc::SlotForPid(proc->pid);
        if (slot < 0) return false;

        Ipc::HandleType type = Ipc::HandleType::None;
        Ipc::Object* object = nullptr;
        uint32_t rights = 0;
        if (!Ipc::SnapshotHandleForSlot(slot, handle, type, object, rights)) return false;
        if (type != expectedType || object == nullptr) return false;

        outObject = object;
        if (outRights != nullptr) *outRights = rights;
        return true;
    }

    static Ipc::Stream* GetRedirOutStream(Sched::Process* proc) {
        proc = GetRedirTarget(proc);
        if (proc == nullptr) return nullptr;

        Ipc::Object* object = nullptr;
        if (!ResolveProcessHandle(proc, proc->ioOutHandle, Ipc::HandleType::Stream, object)) return nullptr;
        return (Ipc::Stream*)object;
    }

    static Ipc::Stream* GetRedirInStream(Sched::Process* proc) {
        proc = GetRedirTarget(proc);
        if (proc == nullptr) return nullptr;

        Ipc::Object* object = nullptr;
        if (!ResolveProcessHandle(proc, proc->ioInHandle, Ipc::HandleType::Stream, object)) return nullptr;
        return (Ipc::Stream*)object;
    }

    static Ipc::Mailbox* GetRedirKeyMailbox(Sched::Process* proc) {
        proc = GetRedirTarget(proc);
        if (proc == nullptr) return nullptr;

        Ipc::Object* object = nullptr;
        if (!ResolveProcessHandle(proc, proc->ioKeyHandle, Ipc::HandleType::Mailbox, object)) return nullptr;
        return (Ipc::Mailbox*)object;
    }

    static int DuplicateHandleBetweenSlots(int srcSlot, int handle, int dstSlot) {
        if (srcSlot < 0 || dstSlot < 0 || handle < 0) return -1;

        Ipc::HandleType type = Ipc::HandleType::None;
        Ipc::Object* object = nullptr;
        uint32_t rights = 0;
        if (!Ipc::SnapshotHandleForSlot(srcSlot, handle, type, object, rights)) return -1;
        if ((rights & Ipc::RightDup) == 0) return -1;
        return Ipc::InstallHandleForSlot(dstSlot, object, type, rights);
    }

    static bool ConfigureRedirWaitsetForSlot(int slot, Sched::Process* proc) {
        if (slot < 0 || proc == nullptr) return false;

        int waitset = Ipc::CreateWaitsetHandleForSlot(slot);
        if (waitset < 0) return false;

        if (proc->ioInHandle >= 0) {
            if (Ipc::WaitsetAddHandleForSlot(slot, waitset, proc->ioInHandle,
                                             Ipc::SignalReadable | Ipc::SignalPeerClosed) < 0) {
                Ipc::CloseHandleForSlot(slot, waitset);
                return false;
            }
        }

        if (proc->ioKeyHandle >= 0) {
            if (Ipc::WaitsetAddHandleForSlot(slot, waitset, proc->ioKeyHandle,
                                             Ipc::SignalReadable | Ipc::SignalPeerClosed) < 0) {
                Ipc::CloseHandleForSlot(slot, waitset);
                return false;
            }
        }

        proc->ioWaitsetHandle = waitset;
        return true;
    }

    static int WriteAllToStream(Ipc::Stream* stream, const uint8_t* data, int len) {
        if (stream == nullptr || data == nullptr || len <= 0) return -1;

        int total = 0;
        while (total < len) {
            int written = Ipc::StreamWrite(stream, data + total, len - total, false);
            if (written < 0) {
                return (total > 0) ? total : -1;
            }
            if (written == 0) {
                Sched::BlockOnObject(stream, 0);
                continue;
            }
            total += written;
        }
        return total;
    }
}
