/*
    * Socket.cpp
    * Socket descriptor table wrapping kernel TCP and UDP layers
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Socket.hpp"
#include <Ipc/Ipc.hpp>
#include <Terminal/Terminal.hpp>

using namespace Kt;

namespace Net::Socket {

    void Initialize() {
        KernelLogStream(OK, "Net") << "Socket handles initialized";
    }

    int Create(int type, int pid) {
        return Ipc::CreateSocketHandleForSlot(Ipc::SlotForPid(pid), type);
    }

    int Connect(int fd, uint32_t ip, uint16_t port, int /*pid*/) {
        return Ipc::SocketConnectHandle(fd, ip, port);
    }

    int Bind(int fd, uint16_t port, int /*pid*/) {
        return Ipc::SocketBindHandle(fd, port);
    }

    int Listen(int fd, int /*pid*/) {
        return Ipc::SocketListenHandle(fd);
    }

    int Accept(int fd, int /*pid*/) {
        return Ipc::SocketAcceptHandle(fd);
    }

    int Send(int fd, const uint8_t* data, uint32_t len, int /*pid*/) {
        return Ipc::SocketSendHandle(fd, data, len);
    }

    int Recv(int fd, uint8_t* buf, uint32_t maxLen, int /*pid*/) {
        return Ipc::SocketRecvHandle(fd, buf, maxLen);
    }

    int SendTo(int fd, const uint8_t* data, uint32_t len,
               uint32_t destIp, uint16_t destPort, int /*pid*/) {
        return Ipc::SocketSendToHandle(fd, data, len, destIp, destPort);
    }

    int RecvFrom(int fd, uint8_t* buf, uint32_t maxLen,
                 uint32_t* srcIp, uint16_t* srcPort, int /*pid*/) {
        return Ipc::SocketRecvFromHandle(fd, buf, maxLen, srcIp, srcPort);
    }

    void Close(int fd, int /*pid*/) {
        Ipc::CloseHandle(fd);
    }

    void CleanupProcess(int /*pid*/) {
    }

}
