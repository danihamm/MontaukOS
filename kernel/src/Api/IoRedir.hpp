/*
    * IoRedir.hpp
    * SYS_SPAWN_REDIR, SYS_CHILDIO_READ, SYS_CHILDIO_WRITE,
    * SYS_CHILDIO_WRITEKEY, SYS_CHILDIO_SETTERMSZ syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>
#include "Syscall.hpp"
#include "Common.hpp"
#include "Path.hpp"

namespace Montauk {

    static int Sys_SpawnRedir(const char* path, const char* args) {
        char resolved[256];
        if (!ResolveProcessPath(path, resolved, sizeof(resolved))) return -1;

        int parentSlot = Ipc::CurrentSlot();
        int childPid = Sched::Spawn(resolved, args);
        if (childPid < 0) return -1;

        auto* child = Sched::GetProcessByPid(childPid);
        int childSlot = Ipc::SlotForPid(childPid);
        if (child == nullptr || childSlot < 0 || parentSlot < 0) return -1;

        Ipc::Stream* outStream = Ipc::CreateStream();
        Ipc::Stream* inStream = Ipc::CreateStream();
        Ipc::Mailbox* keyMailbox = Ipc::CreateMailbox();
        if (outStream == nullptr || inStream == nullptr || keyMailbox == nullptr) {
            if (outStream != nullptr) Ipc::ReleaseStream(outStream, false, false);
            if (inStream != nullptr) Ipc::ReleaseStream(inStream, false, false);
            if (keyMailbox != nullptr) Ipc::ReleaseMailbox(keyMailbox, false, false);
            Sched::KillProcess(childPid);
            return -1;
        }

        int parentOutHandle = Ipc::InstallHandleForSlot(
            parentSlot, (Ipc::Object*)outStream, Ipc::HandleType::Stream, Ipc::RightRead);
        int parentInHandle = Ipc::InstallHandleForSlot(
            parentSlot, (Ipc::Object*)inStream, Ipc::HandleType::Stream, Ipc::RightWrite);
        int parentKeyHandle = Ipc::InstallHandleForSlot(
            parentSlot, (Ipc::Object*)keyMailbox, Ipc::HandleType::Mailbox, Ipc::RightSend);

        child->ioOutHandle = Ipc::InstallHandleForSlot(
            childSlot, (Ipc::Object*)outStream, Ipc::HandleType::Stream,
            Ipc::RightWrite | Ipc::RightWait | Ipc::RightDup);
        child->ioInHandle = Ipc::InstallHandleForSlot(
            childSlot, (Ipc::Object*)inStream, Ipc::HandleType::Stream,
            Ipc::RightRead | Ipc::RightWait | Ipc::RightDup);
        child->ioKeyHandle = Ipc::InstallHandleForSlot(
            childSlot, (Ipc::Object*)keyMailbox, Ipc::HandleType::Mailbox,
            Ipc::RightRecv | Ipc::RightWait | Ipc::RightDup);

        if (parentOutHandle < 0 || parentInHandle < 0 || parentKeyHandle < 0 ||
            child->ioOutHandle < 0 || child->ioInHandle < 0 || child->ioKeyHandle < 0 ||
            !ConfigureRedirWaitsetForSlot(childSlot, child)) {
            if (parentOutHandle >= 0) Ipc::CloseHandleForSlot(parentSlot, parentOutHandle);
            if (parentInHandle >= 0) Ipc::CloseHandleForSlot(parentSlot, parentInHandle);
            if (parentKeyHandle >= 0) Ipc::CloseHandleForSlot(parentSlot, parentKeyHandle);
            if (child->ioOutHandle < 0) Ipc::ReleaseStream(outStream, false, false);
            if (child->ioInHandle < 0) Ipc::ReleaseStream(inStream, false, false);
            if (child->ioKeyHandle < 0) Ipc::ReleaseMailbox(keyMailbox, false, false);
            Sched::KillProcess(childPid);
            return -1;
        }

        child->redirected = true;
        child->parentPid = Sched::GetCurrentPid();
        child->termCols = 0;
        child->termRows = 0;

        return childPid;
    }

    static int Sys_ChildIoRead(int childPid, char* buf, int maxLen) {
        auto* child = Sched::GetProcessByPid(childPid);
        Ipc::Stream* stream = GetRedirOutStream(child);
        if (child == nullptr || !child->redirected || stream == nullptr) return -1;
        return Ipc::StreamRead(stream, (uint8_t*)buf, maxLen, true);
    }

    static int Sys_ChildIoWrite(int childPid, const char* data, int len) {
        auto* child = Sched::GetProcessByPid(childPid);
        Ipc::Stream* stream = GetRedirInStream(child);
        if (child == nullptr || !child->redirected || stream == nullptr) return -1;
        return WriteAllToStream(stream, (const uint8_t*)data, len);
    }

    static int Sys_ChildIoWriteKey(int childPid, const KeyEvent* key) {
        if (key == nullptr) return -1;
        auto* child = Sched::GetProcessByPid(childPid);
        Ipc::Mailbox* mailbox = GetRedirKeyMailbox(child);
        if (child == nullptr || !child->redirected || mailbox == nullptr) return -1;

        for (;;) {
            int rc = Ipc::MailboxSend(mailbox, 0, key, sizeof(KeyEvent));
            if (rc < 0) return -1;
            if (rc == 0) {
                Sched::BlockOnObject(mailbox, 0);
                continue;
            }
            return 0;
        }
    }

    static int Sys_ChildIoSetTermsz(int childPid, int cols, int rows) {
        auto* child = Sched::GetProcessByPid(childPid);
        if (child == nullptr || !child->redirected) return -1;
        child->termCols = cols;
        child->termRows = rows;
        return 0;
    }
};
