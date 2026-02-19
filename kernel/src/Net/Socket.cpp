/*
    * Socket.cpp
    * Socket descriptor table wrapping kernel TCP and UDP layers
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Socket.hpp"
#include <Net/Tcp.hpp>
#include <Net/Udp.hpp>
#include <Net/NetConfig.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Libraries/Memory.hpp>

using namespace Kt;

namespace Net::Socket {

    // ---- UDP socket state ----

    static constexpr uint32_t UDP_RING_SIZE = 4096;
    static constexpr int MAX_UDP_SOCKETS = 16;

    struct UdpDgramHeader {
        uint32_t SrcIp;
        uint16_t SrcPort;
        uint16_t DataLen;
    };

    struct UdpSocketState {
        uint8_t  Ring[UDP_RING_SIZE];
        uint32_t Head;
        uint32_t Tail;
        uint32_t Count;
        uint16_t LocalPort;
        bool     Active;
    };

    static UdpSocketState g_udpSockets[MAX_UDP_SOCKETS] = {};
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

    static UdpSocketState* AllocUdpState() {
        for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
            if (!g_udpSockets[i].Active) {
                g_udpSockets[i].Active = true;
                g_udpSockets[i].Head = 0;
                g_udpSockets[i].Tail = 0;
                g_udpSockets[i].Count = 0;
                g_udpSockets[i].LocalPort = 0;
                return &g_udpSockets[i];
            }
        }
        return nullptr;
    }

    static void FreeUdpState(UdpSocketState* state) {
        if (state) {
            if (state->LocalPort != 0) {
                Udp::Unbind(state->LocalPort);
            }
            state->Active = false;
        }
    }

    static void UdpSocketDispatcher(uint32_t srcIp, uint16_t srcPort,
                                     uint16_t dstPort,
                                     const uint8_t* data, uint16_t length) {
        for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
            if (g_udpSockets[i].Active && g_udpSockets[i].LocalPort == dstPort) {
                UdpSocketState* st = &g_udpSockets[i];
                uint32_t needed = sizeof(UdpDgramHeader) + length;
                if (st->Count + needed > UDP_RING_SIZE) {
                    return; // drop if buffer full
                }

                // Enqueue header
                UdpDgramHeader hdr;
                hdr.SrcIp = srcIp;
                hdr.SrcPort = srcPort;
                hdr.DataLen = length;

                const uint8_t* hdrBytes = (const uint8_t*)&hdr;
                for (uint32_t j = 0; j < sizeof(UdpDgramHeader); j++) {
                    st->Ring[st->Tail] = hdrBytes[j];
                    st->Tail = (st->Tail + 1) % UDP_RING_SIZE;
                }

                // Enqueue payload
                for (uint16_t j = 0; j < length; j++) {
                    st->Ring[st->Tail] = data[j];
                    st->Tail = (st->Tail + 1) % UDP_RING_SIZE;
                }

                st->Count += needed;
                return;
            }
        }
    }

    // ---- Public API ----

    void Initialize() {
        for (int i = 0; i < MAX_SOCKETS; i++) {
            g_sockets[i].Active = false;
        }
        for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
            g_udpSockets[i].Active = false;
        }
        KernelLogStream(OK, "Net") << "Socket table initialized";
    }

    int Create(int type, int pid) {
        if (type != SOCK_TCP && type != SOCK_UDP) return -1;

        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (!g_sockets[i].Active) {
                g_sockets[i].Active = true;
                g_sockets[i].Type = type;
                g_sockets[i].OwnerPid = pid;
                g_sockets[i].TcpConn = nullptr;
                g_sockets[i].UdpState = nullptr;
                g_sockets[i].LocalPort = 0;

                if (type == SOCK_UDP) {
                    UdpSocketState* us = AllocUdpState();
                    if (!us) {
                        g_sockets[i].Active = false;
                        return -1;
                    }
                    g_sockets[i].UdpState = us;
                }

                return i;
            }
        }
        return -1;
    }

    int Connect(int fd, uint32_t ip, uint16_t port, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].Type != SOCK_TCP) return -1;
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

        if (g_sockets[fd].Type == SOCK_UDP) {
            UdpSocketState* us = g_sockets[fd].UdpState;
            if (!us) return -1;
            us->LocalPort = port;
            if (!Udp::Bind(port, UdpSocketDispatcher)) return -1;
        }

        return 0;
    }

    int Listen(int fd, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].Type != SOCK_TCP) return -1;
        if (g_sockets[fd].LocalPort == 0) return -1;
        if (g_sockets[fd].TcpConn != nullptr) return -1;

        Tcp::Connection* conn = Tcp::Listen(g_sockets[fd].LocalPort);
        if (conn == nullptr) return -1;

        g_sockets[fd].TcpConn = conn;
        return 0;
    }

    int Accept(int fd, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].Type != SOCK_TCP) return -1;
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
                g_sockets[i].UdpState = nullptr;
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
        if (g_sockets[fd].Type != SOCK_TCP) return -1;
        if (g_sockets[fd].TcpConn == nullptr) return -1;
        return Tcp::Send(g_sockets[fd].TcpConn, data, (uint16_t)len);
    }

    int Recv(int fd, uint8_t* buf, uint32_t maxLen, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].Type != SOCK_TCP) return -1;
        if (g_sockets[fd].TcpConn == nullptr) return -1;
        return Tcp::ReceiveNonBlocking(g_sockets[fd].TcpConn, buf, (uint16_t)maxLen);
    }

    int SendTo(int fd, const uint8_t* data, uint32_t len,
               uint32_t destIp, uint16_t destPort, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].Type != SOCK_UDP) return -1;

        UdpSocketState* us = g_sockets[fd].UdpState;
        if (!us) return -1;

        // Auto-bind ephemeral port if not already bound
        if (us->LocalPort == 0) {
            uint16_t ep = AllocEphemeralPort();
            us->LocalPort = ep;
            g_sockets[fd].LocalPort = ep;
            if (!Udp::Bind(ep, UdpSocketDispatcher)) return -1;
        }

        if (!Udp::Send(destIp, us->LocalPort, destPort, data, (uint16_t)len)) {
            return -1;
        }
        return (int)len;
    }

    int RecvFrom(int fd, uint8_t* buf, uint32_t maxLen,
                 uint32_t* srcIp, uint16_t* srcPort, int pid) {
        if (!ValidFd(fd, pid)) return -1;
        if (g_sockets[fd].Type != SOCK_UDP) return -1;

        UdpSocketState* us = g_sockets[fd].UdpState;
        if (!us) return -1;

        if (us->Count < sizeof(UdpDgramHeader)) {
            return -1; // no data available
        }

        // Dequeue header
        UdpDgramHeader hdr;
        uint8_t* hdrBytes = (uint8_t*)&hdr;
        for (uint32_t j = 0; j < sizeof(UdpDgramHeader); j++) {
            hdrBytes[j] = us->Ring[us->Head];
            us->Head = (us->Head + 1) % UDP_RING_SIZE;
        }
        us->Count -= sizeof(UdpDgramHeader);

        // Dequeue payload
        uint16_t copyLen = hdr.DataLen;
        if (copyLen > maxLen) copyLen = (uint16_t)maxLen;

        for (uint16_t j = 0; j < copyLen; j++) {
            buf[j] = us->Ring[us->Head];
            us->Head = (us->Head + 1) % UDP_RING_SIZE;
        }

        // Skip remaining data if buffer was too small
        for (uint16_t j = copyLen; j < hdr.DataLen; j++) {
            us->Head = (us->Head + 1) % UDP_RING_SIZE;
        }
        us->Count -= hdr.DataLen;

        if (srcIp) *srcIp = hdr.SrcIp;
        if (srcPort) *srcPort = hdr.SrcPort;

        return (int)copyLen;
    }

    void Close(int fd, int pid) {
        if (!ValidFd(fd, pid)) return;
        if (g_sockets[fd].TcpConn != nullptr) {
            Tcp::Close(g_sockets[fd].TcpConn);
            g_sockets[fd].TcpConn = nullptr;
        }
        if (g_sockets[fd].UdpState != nullptr) {
            FreeUdpState(g_sockets[fd].UdpState);
            g_sockets[fd].UdpState = nullptr;
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
                if (g_sockets[i].UdpState != nullptr) {
                    FreeUdpState(g_sockets[i].UdpState);
                    g_sockets[i].UdpState = nullptr;
                }
                g_sockets[i].Active = false;
            }
        }
    }

}
