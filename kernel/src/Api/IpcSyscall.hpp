/*
    * IpcSyscall.hpp
    * Generic IPC handle, stream, mailbox, waitset, process, and surface syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Ipc/Ipc.hpp>
#include <Sched/Scheduler.hpp>
#include "Syscall.hpp"

namespace Montauk {

    static int Sys_DupHandle(int handle) {
        return Ipc::DupHandle(handle);
    }

    static uint32_t Sys_WaitHandle(int handle, uint32_t wantedSignals, uint64_t timeoutMs) {
        return Ipc::WaitOnHandle(handle, wantedSignals, timeoutMs);
    }

    static int Sys_StreamCreate(int* outReadHandle, int* outWriteHandle, uint32_t capacity) {
        if (outReadHandle == nullptr || outWriteHandle == nullptr) return -1;

        int readHandle = -1;
        int writeHandle = -1;
        if (Ipc::CreateStreamHandlePair(capacity, readHandle, writeHandle) < 0) return -1;

        *outReadHandle = readHandle;
        *outWriteHandle = writeHandle;
        return 0;
    }

    static int Sys_StreamRead(int handle, uint8_t* buffer, int maxLen) {
        return Ipc::StreamReadHandle(handle, buffer, maxLen);
    }

    static int Sys_StreamWrite(int handle, const uint8_t* data, int len) {
        return Ipc::StreamWriteHandle(handle, data, len);
    }

    static int Sys_MailboxCreate(int* outSendHandle, int* outRecvHandle) {
        if (outSendHandle == nullptr || outRecvHandle == nullptr) return -1;

        int sendHandle = -1;
        int recvHandle = -1;
        if (Ipc::CreateMailboxHandlePair(sendHandle, recvHandle) < 0) return -1;

        *outSendHandle = sendHandle;
        *outRecvHandle = recvHandle;
        return 0;
    }

    static int Sys_MailboxSend(int handle, uint32_t msgType, const void* data, uint16_t len, int attachHandle) {
        return Ipc::MailboxSendHandle(handle, msgType, data, len, attachHandle);
    }

    static int Sys_MailboxRecv(int handle, uint32_t* outMsgType, void* data,
                               uint16_t* inOutLen, int* outAttachHandle) {
        return Ipc::MailboxRecvHandle(handle, outMsgType, data, inOutLen, outAttachHandle);
    }

    static int Sys_WaitsetCreate() {
        return Ipc::CreateWaitsetHandleForCurrent();
    }

    static int Sys_WaitsetAdd(int waitsetHandle, int targetHandle, uint32_t signals) {
        return Ipc::WaitsetAddHandle(waitsetHandle, targetHandle, signals);
    }

    static int Sys_WaitsetRemove(int waitsetHandle, int index) {
        return Ipc::WaitsetRemoveIndex(waitsetHandle, index);
    }

    static int Sys_WaitsetWait(int waitsetHandle, IpcWaitResult* outReady, uint64_t timeoutMs) {
        Ipc::WaitsetReady ready = {-1, 0};
        int result = Ipc::WaitsetWaitHandle(waitsetHandle, outReady ? &ready : nullptr, timeoutMs);
        if (result > 0 && outReady != nullptr) {
            outReady->index = ready.index;
            outReady->signals = ready.signals;
        }
        return result;
    }

    static int Sys_ProcOpen(int pid) {
        return Ipc::OpenProcessHandle(pid);
    }

    static int Sys_SurfaceCreate(uint64_t byteSize) {
        return Ipc::CreateSurfaceHandle(byteSize);
    }

    static uint64_t Sys_SurfaceMap(int handle) {
        return Ipc::MapSurfaceHandle(handle);
    }

    static int Sys_SurfaceResize(int handle, uint64_t newSize) {
        return Ipc::ResizeSurfaceHandle(handle, newSize);
    }

}
