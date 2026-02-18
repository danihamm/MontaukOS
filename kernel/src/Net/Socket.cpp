/*
    * Socket.cpp
    * Socket descriptor table wrapping kernel TCP layer
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Socket.hpp"
#include <Net/Tcp.hpp>
#include <Net/NetConfig.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Net::Socket {

    static SocketEntry g_sockets[MAX_SOCKETS] = {};
    static uint16_t g_nextEphemeralPort = 49152;

    static uint16_t AllocEphemeralPort() {
        uint16_t port = g_nextEphemeralPort++;
        if (g_nextEphemeralPort == 0) g_nextEphemeralPort = 49152;
        return port;
    }

    static bool ValidFd(int fd, int pid) {
        if (fd < 0 || fd >= MAX_SOCKETS) return false;
        if (!g_sockets[fd].Active) return false;
        if (g_sockets[fd].OwnerPid != pid) return false;
        return true;
    }

    void Initialize() {
        for (int i = 0; i < MAX_SOCKETS; i++) {
            g_sockets[i].Active = false;
        }
        KernelLogStream(OK, "Net") << "Socket table initialized";
    }

    int Create(int type, int pid) {
        if (type != SOCK_TCP) return -1;

        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (!g_sockets[i].Active) {
                g_sockets[i].Active = true;
                g_sockets[i].Type = type;
                g_sockets[i].OwnerPid = pid;
                g_sockets[i].TcpConn = nullptr;
                g_sockets[i].LocalPort = 0;
                return i;
            }
        }
        return -1;
    }

    int Connect(int fd, uint32_t ip, uint16_t port, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].TcpConn != nullptr) return -1;

        uint16_t srcPort = AllocEphemeralPort();
        g_sockets[fd].LocalPort = srcPort;

        Tcp::Connection* conn = Tcp::Connect(ip, port, srcPort);
        if (conn == nullptr) return -1;

        g_sockets[fd].TcpConn = conn;
        return 0;
    }

    int Bind(int fd, uint16_t port, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        g_sockets[fd].LocalPort = port;
        return 0;
    }

    int Listen(int fd, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].LocalPort == 0) return -1;
        if (g_sockets[fd].TcpConn != nullptr) return -1;

        Tcp::Connection* conn = Tcp::Listen(g_sockets[fd].LocalPort);
        if (conn == nullptr) return -1;

        g_sockets[fd].TcpConn = conn;
        return 0;
    }

    int Accept(int fd, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].TcpConn == nullptr) return -1;

        Tcp::Connection* clientConn = Tcp::Accept(g_sockets[fd].TcpConn);
        if (clientConn == nullptr) return -1;

        // Allocate a new socket entry for the accepted connection
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (!g_sockets[i].Active) {
                g_sockets[i].Active = true;
                g_sockets[i].Type = SOCK_TCP;
                g_sockets[i].OwnerPid = pid;
                g_sockets[i].TcpConn = clientConn;
                g_sockets[i].LocalPort = g_sockets[fd].LocalPort;
                return i;
            }
        }

        // No free socket slot — close the accepted connection
        Tcp::Close(clientConn);
        return -1;
    }

    int Send(int fd, const uint8_t* data, uint32_t len, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].TcpConn == nullptr) return -1;
        return Tcp::Send(g_sockets[fd].TcpConn, data, (uint16_t)len);
    }

    int Recv(int fd, uint8_t* buf, uint32_t maxLen, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].TcpConn == nullptr) return -1;
        return Tcp::ReceiveNonBlocking(g_sockets[fd].TcpConn, buf, (uint16_t)maxLen);
    }

    void Close(int fd, int pid) {
        if (!ValidFd(fd, pid)) return;
        if (g_sockets[fd].TcpConn != nullptr) {
            Tcp::Close(g_sockets[fd].TcpConn);
            g_sockets[fd].TcpConn = nullptr;
        }
        g_sockets[fd].Active = false;
    }

    void CleanupProcess(int pid) {
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (g_sockets[i].Active && g_sockets[i].OwnerPid == pid) {
                if (g_sockets[i].TcpConn != nullptr) {
                    Tcp::Close(g_sockets[i].TcpConn);
                    g_sockets[i].TcpConn = nullptr;
                }
                g_sockets[i].Active = false;
            }
        }
    }

}
