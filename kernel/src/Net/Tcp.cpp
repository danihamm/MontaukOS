/*
    * Tcp.cpp
    * Transmission Control Protocol
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Tcp.hpp"
#include <Net/Ipv4.hpp>
#include <Net/ByteOrder.hpp>
#include <Net/NetConfig.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <CppLib/Spinlock.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

namespace Net::Tcp {

    // Receive buffer size per connection
    static constexpr uint16_t RECV_BUFFER_SIZE = 4096;
    static constexpr uint16_t WINDOW_SIZE = 4096;
    static constexpr uint32_t MAX_CONNECTIONS = 16;
    static constexpr uint64_t RETRANSMIT_TIMEOUT_MS = 1000;
    static constexpr int      MAX_RETRANSMITS = 5;
    static constexpr uint64_t TIME_WAIT_MS = 2000;

    struct Connection {
        State    CurrentState;
        uint32_t LocalIp;
        uint16_t LocalPort;
        uint32_t RemoteIp;
        uint16_t RemotePort;

        // Sequence numbers
        uint32_t SendNext;    // Next sequence number to send
        uint32_t SendUnack;   // Oldest unacknowledged sequence number
        uint32_t RecvNext;    // Next expected sequence number from remote

        // Receive buffer (ring buffer)
        uint8_t  RecvBuffer[RECV_BUFFER_SIZE];
        uint16_t RecvHead;    // Read position
        uint16_t RecvTail;    // Write position
        uint16_t RecvCount;   // Bytes in buffer

        // Retransmission tracking
        uint8_t  RetransmitBuffer[1500];
        uint16_t RetransmitLen;
        uint64_t RetransmitTime;
        int      RetransmitCount;

        // For Listen/Accept
        bool     PendingAccept;
        uint32_t PendingRemoteIp;
        uint16_t PendingRemotePort;
        uint32_t PendingSeq;

        bool     Active;

        kcp::Spinlock Lock;
    };

    static Connection g_connections[MAX_CONNECTIONS] = {};
    static kcp::Spinlock g_connectionsLock;

    // Simple ISN generator using timer
    static uint32_t GenerateISN() {
        return (uint32_t)(Timekeeping::GetMilliseconds() * 2654435761u);
    }

    static Connection* FindConnection(uint32_t remoteIp, uint16_t remotePort,
                                       uint16_t localPort) {
        for (uint32_t i = 0; i < MAX_CONNECTIONS; i++) {
            Connection* c = &g_connections[i];
            if (c->Active &&
                c->LocalPort == localPort &&
                c->RemoteIp == remoteIp &&
                c->RemotePort == remotePort &&
                c->CurrentState != State::Listen) {
                return c;
            }
        }
        return nullptr;
    }

    static Connection* FindListener(uint16_t localPort) {
        for (uint32_t i = 0; i < MAX_CONNECTIONS; i++) {
            Connection* c = &g_connections[i];
            if (c->Active && c->LocalPort == localPort && c->CurrentState == State::Listen) {
                return c;
            }
        }
        return nullptr;
    }

    static Connection* AllocateConnection() {
        for (uint32_t i = 0; i < MAX_CONNECTIONS; i++) {
            if (!g_connections[i].Active) {
                Connection* c = &g_connections[i];
                memset(c, 0, sizeof(Connection));
                c->Active = true;
                c->CurrentState = State::Closed;
                return c;
            }
        }
        return nullptr;
    }

    static bool SendSegment(Connection* conn, uint8_t flags,
                             const uint8_t* payload, uint16_t payloadLen) {
        uint8_t packet[1500];
        Header* hdr = (Header*)packet;

        hdr->SrcPort = Htons(conn->LocalPort);
        hdr->DstPort = Htons(conn->RemotePort);
        hdr->SeqNum = Htonl(conn->SendNext);
        hdr->AckNum = Htonl(conn->RecvNext);
        hdr->DataOffset = (HEADER_SIZE / 4) << 4;
        hdr->Flags = flags;
        hdr->Window = Htons(WINDOW_SIZE);
        hdr->Checksum = 0;
        hdr->UrgentPtr = 0;

        uint16_t totalLen = HEADER_SIZE + payloadLen;
        if (payload != nullptr && payloadLen > 0) {
            memcpy(packet + HEADER_SIZE, payload, payloadLen);
        }

        // Calculate checksum with pseudo-header
        hdr->Checksum = Ipv4::PseudoHeaderChecksum(
            conn->LocalIp, conn->RemoteIp, Ipv4::PROTO_TCP,
            totalLen, packet, totalLen);

        return Ipv4::Send(conn->RemoteIp, Ipv4::PROTO_TCP, packet, totalLen);
    }

    // Send a RST to an unexpected packet
    static void SendReset(uint32_t destIp, uint16_t destPort, uint16_t srcPort,
                           uint32_t seqNum, uint32_t ackNum) {
        uint8_t packet[HEADER_SIZE];
        Header* hdr = (Header*)packet;

        hdr->SrcPort = Htons(srcPort);
        hdr->DstPort = Htons(destPort);
        hdr->SeqNum = Htonl(seqNum);
        hdr->AckNum = Htonl(ackNum);
        hdr->DataOffset = (HEADER_SIZE / 4) << 4;
        hdr->Flags = FLAG_RST | FLAG_ACK;
        hdr->Window = 0;
        hdr->Checksum = 0;
        hdr->UrgentPtr = 0;

        uint32_t localIp = Net::GetIpAddress();
        hdr->Checksum = Ipv4::PseudoHeaderChecksum(
            localIp, destIp, Ipv4::PROTO_TCP, HEADER_SIZE, packet, HEADER_SIZE);

        Ipv4::Send(destIp, Ipv4::PROTO_TCP, packet, HEADER_SIZE);
    }

    static void RecvBufferWrite(Connection* conn, const uint8_t* data, uint16_t len) {
        for (uint16_t i = 0; i < len && conn->RecvCount < RECV_BUFFER_SIZE; i++) {
            conn->RecvBuffer[conn->RecvTail] = data[i];
            conn->RecvTail = (conn->RecvTail + 1) % RECV_BUFFER_SIZE;
            conn->RecvCount++;
        }
    }

    void Initialize() {
        for (uint32_t i = 0; i < MAX_CONNECTIONS; i++) {
            g_connections[i].Active = false;
        }
        KernelLogStream(OK, "Net") << "TCP initialized";
    }

    void OnPacketReceived(uint32_t srcIp, uint32_t dstIp, const uint8_t* data, uint16_t length) {
        if (length < HEADER_SIZE) {
            return;
        }

        const Header* hdr = (const Header*)data;

        // Verify checksum
        uint16_t check = Ipv4::PseudoHeaderChecksum(srcIp, dstIp, Ipv4::PROTO_TCP,
                                                     length, data, length);
        if (check != 0) {
            return;
        }

        uint16_t srcPort = Ntohs(hdr->SrcPort);
        uint16_t dstPort = Ntohs(hdr->DstPort);
        uint32_t seqNum  = Ntohl(hdr->SeqNum);
        uint32_t ackNum  = Ntohl(hdr->AckNum);
        uint8_t  flags   = hdr->Flags;
        uint8_t  dataOff = (hdr->DataOffset >> 4) * 4;

        if (dataOff < HEADER_SIZE || dataOff > length) {
            return;
        }

        const uint8_t* payload = data + dataOff;
        uint16_t payloadLen = length - dataOff;

        // Find existing connection
        Connection* conn = FindConnection(srcIp, srcPort, dstPort);

        if (conn == nullptr) {
            // Check for a listening socket
            if (flags & FLAG_SYN) {
                Connection* listener = FindListener(dstPort);
                if (listener != nullptr) {
                    // Signal the listener about this incoming connection
                    listener->Lock.Acquire();
                    listener->PendingAccept = true;
                    listener->PendingRemoteIp = srcIp;
                    listener->PendingRemotePort = srcPort;
                    listener->PendingSeq = seqNum;
                    listener->Lock.Release();
                    return;
                }
            }

            // No matching connection or listener -- send RST
            if (!(flags & FLAG_RST)) {
                if (flags & FLAG_ACK) {
                    SendReset(srcIp, srcPort, dstPort, ackNum, 0);
                } else {
                    uint32_t rstAck = seqNum + payloadLen;
                    if (flags & FLAG_SYN) rstAck++;
                    if (flags & FLAG_FIN) rstAck++;
                    SendReset(srcIp, srcPort, dstPort, 0, rstAck);
                }
            }
            return;
        }

        conn->Lock.Acquire();

        // RST handling
        if (flags & FLAG_RST) {
            conn->CurrentState = State::Closed;
            conn->Active = false;
            conn->Lock.Release();
            return;
        }

        switch (conn->CurrentState) {
            case State::SynSent: {
                // Expecting SYN-ACK
                if ((flags & (FLAG_SYN | FLAG_ACK)) == (FLAG_SYN | FLAG_ACK)) {
                    if (ackNum == conn->SendNext) {
                        conn->RecvNext = seqNum + 1;
                        conn->SendUnack = ackNum;
                        conn->CurrentState = State::Established;

                        // Send ACK
                        SendSegment(conn, FLAG_ACK, nullptr, 0);

                        KernelLogStream(INFO, "Net") << "TCP connection established to port "
                            << base::dec << (uint64_t)conn->RemotePort;
                    }
                }
                break;
            }

            case State::SynReceived: {
                // Expecting ACK to complete handshake
                if (flags & FLAG_ACK) {
                    if (ackNum == conn->SendNext) {
                        conn->SendUnack = ackNum;
                        conn->CurrentState = State::Established;
                    }
                }
                break;
            }

            case State::Established: {
                // Handle incoming data
                if (flags & FLAG_ACK) {
                    conn->SendUnack = ackNum;
                }

                if (payloadLen > 0 && seqNum == conn->RecvNext) {
                    RecvBufferWrite(conn, payload, payloadLen);
                    conn->RecvNext += payloadLen;

                    // Send ACK
                    SendSegment(conn, FLAG_ACK, nullptr, 0);
                }

                if (flags & FLAG_FIN) {
                    conn->RecvNext = seqNum + payloadLen + 1;
                    conn->CurrentState = State::CloseWait;

                    // Send ACK for the FIN
                    SendSegment(conn, FLAG_ACK, nullptr, 0);
                }
                break;
            }

            case State::FinWait1: {
                if (flags & FLAG_ACK) {
                    conn->SendUnack = ackNum;
                    if (flags & FLAG_FIN) {
                        conn->RecvNext = seqNum + 1;
                        conn->CurrentState = State::TimeWait;
                        SendSegment(conn, FLAG_ACK, nullptr, 0);
                    } else {
                        conn->CurrentState = State::FinWait2;
                    }
                } else if (flags & FLAG_FIN) {
                    conn->RecvNext = seqNum + 1;
                    conn->CurrentState = State::TimeWait;
                    SendSegment(conn, FLAG_ACK, nullptr, 0);
                }
                break;
            }

            case State::FinWait2: {
                if (flags & FLAG_FIN) {
                    conn->RecvNext = seqNum + 1;
                    conn->CurrentState = State::TimeWait;
                    SendSegment(conn, FLAG_ACK, nullptr, 0);
                }
                break;
            }

            case State::LastAck: {
                if (flags & FLAG_ACK) {
                    conn->CurrentState = State::Closed;
                    conn->Active = false;
                }
                break;
            }

            case State::TimeWait: {
                // Ignore, will time out
                break;
            }

            default:
                break;
        }

        conn->Lock.Release();
    }

    Connection* Listen(uint16_t port) {
        g_connectionsLock.Acquire();
        Connection* conn = AllocateConnection();
        g_connectionsLock.Release();

        if (conn == nullptr) {
            return nullptr;
        }

        conn->LocalIp = Net::GetIpAddress();
        conn->LocalPort = port;
        conn->CurrentState = State::Listen;
        conn->PendingAccept = false;

        KernelLogStream(INFO, "Net") << "TCP listening on port " << base::dec << (uint64_t)port;
        return conn;
    }

    Connection* Accept(Connection* listener) {
        if (listener == nullptr || listener->CurrentState != State::Listen) {
            return nullptr;
        }

        // Block until a SYN arrives
        while (true) {
            listener->Lock.Acquire();
            if (listener->PendingAccept) {
                listener->PendingAccept = false;

                uint32_t remoteIp = listener->PendingRemoteIp;
                uint16_t remotePort = listener->PendingRemotePort;
                uint32_t remoteSeq = listener->PendingSeq;
                listener->Lock.Release();

                // Allocate a new connection for this client
                g_connectionsLock.Acquire();
                Connection* conn = AllocateConnection();
                g_connectionsLock.Release();

                if (conn == nullptr) {
                    return nullptr;
                }

                conn->LocalIp = Net::GetIpAddress();
                conn->LocalPort = listener->LocalPort;
                conn->RemoteIp = remoteIp;
                conn->RemotePort = remotePort;
                conn->RecvNext = remoteSeq + 1;

                uint32_t isn = GenerateISN();
                conn->SendNext = isn;
                conn->SendUnack = isn;
                conn->CurrentState = State::SynReceived;

                // Send SYN-ACK
                conn->SendNext = isn + 1;
                {
                    // Manually build the SYN-ACK with ISN as seqnum
                    uint8_t packet[HEADER_SIZE];
                    Header* hdr = (Header*)packet;

                    hdr->SrcPort = Htons(conn->LocalPort);
                    hdr->DstPort = Htons(conn->RemotePort);
                    hdr->SeqNum = Htonl(isn);
                    hdr->AckNum = Htonl(conn->RecvNext);
                    hdr->DataOffset = (HEADER_SIZE / 4) << 4;
                    hdr->Flags = FLAG_SYN | FLAG_ACK;
                    hdr->Window = Htons(WINDOW_SIZE);
                    hdr->Checksum = 0;
                    hdr->UrgentPtr = 0;

                    hdr->Checksum = Ipv4::PseudoHeaderChecksum(
                        conn->LocalIp, conn->RemoteIp, Ipv4::PROTO_TCP,
                        HEADER_SIZE, packet, HEADER_SIZE);

                    Ipv4::Send(conn->RemoteIp, Ipv4::PROTO_TCP, packet, HEADER_SIZE);
                }

                // Wait for ACK to complete the handshake
                for (int i = 0; i < 100; i++) {
                    if (conn->CurrentState == State::Established) {
                        return conn;
                    }
                    Timekeeping::Sleep(50);
                }

                // Timed out waiting for ACK
                conn->Active = false;
                return nullptr;
            }
            listener->Lock.Release();
            Timekeeping::Sleep(10);
        }
    }

    Connection* Connect(uint32_t destIp, uint16_t destPort, uint16_t srcPort) {
        g_connectionsLock.Acquire();
        Connection* conn = AllocateConnection();
        g_connectionsLock.Release();

        if (conn == nullptr) {
            return nullptr;
        }

        conn->LocalIp = Net::GetIpAddress();
        conn->LocalPort = srcPort;
        conn->RemoteIp = destIp;
        conn->RemotePort = destPort;

        uint32_t isn = GenerateISN();
        conn->SendNext = isn + 1;
        conn->SendUnack = isn;
        conn->CurrentState = State::SynSent;

        // Send SYN
        {
            uint8_t packet[HEADER_SIZE];
            Header* hdr = (Header*)packet;

            hdr->SrcPort = Htons(conn->LocalPort);
            hdr->DstPort = Htons(conn->RemotePort);
            hdr->SeqNum = Htonl(isn);
            hdr->AckNum = 0;
            hdr->DataOffset = (HEADER_SIZE / 4) << 4;
            hdr->Flags = FLAG_SYN;
            hdr->Window = Htons(WINDOW_SIZE);
            hdr->Checksum = 0;
            hdr->UrgentPtr = 0;

            hdr->Checksum = Ipv4::PseudoHeaderChecksum(
                conn->LocalIp, conn->RemoteIp, Ipv4::PROTO_TCP,
                HEADER_SIZE, packet, HEADER_SIZE);

            Ipv4::Send(conn->RemoteIp, Ipv4::PROTO_TCP, packet, HEADER_SIZE);
        }

        // Wait for SYN-ACK
        for (int attempt = 0; attempt < MAX_RETRANSMITS; attempt++) {
            for (int i = 0; i < 20; i++) {
                if (conn->CurrentState == State::Established) {
                    return conn;
                }
                Timekeeping::Sleep(50);
            }

            if (conn->CurrentState == State::SynSent) {
                // Retransmit SYN
                uint8_t packet[HEADER_SIZE];
                Header* hdr = (Header*)packet;

                hdr->SrcPort = Htons(conn->LocalPort);
                hdr->DstPort = Htons(conn->RemotePort);
                hdr->SeqNum = Htonl(isn);
                hdr->AckNum = 0;
                hdr->DataOffset = (HEADER_SIZE / 4) << 4;
                hdr->Flags = FLAG_SYN;
                hdr->Window = Htons(WINDOW_SIZE);
                hdr->Checksum = 0;
                hdr->UrgentPtr = 0;

                hdr->Checksum = Ipv4::PseudoHeaderChecksum(
                    conn->LocalIp, conn->RemoteIp, Ipv4::PROTO_TCP,
                    HEADER_SIZE, packet, HEADER_SIZE);

                Ipv4::Send(conn->RemoteIp, Ipv4::PROTO_TCP, packet, HEADER_SIZE);
            }
        }

        // Failed to connect
        conn->Active = false;
        return nullptr;
    }

    int Send(Connection* conn, const uint8_t* data, uint16_t length) {
        if (conn == nullptr || conn->CurrentState != State::Established) {
            return -1;
        }

        // Phase 1: Send data segments with interrupts disabled (lock-safe)
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        conn->Lock.Acquire();

        constexpr uint16_t MSS = 1460;
        uint16_t sent = 0;

        while (sent < length) {
            uint16_t segLen = length - sent;
            if (segLen > MSS) {
                segLen = MSS;
            }

            bool ok = SendSegment(conn, FLAG_ACK | FLAG_PSH, data + sent, segLen);
            if (!ok) {
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");
                return sent > 0 ? sent : -1;
            }

            conn->SendNext += segLen;

            // Store for retransmission
            if (segLen <= sizeof(conn->RetransmitBuffer)) {
                memcpy(conn->RetransmitBuffer, data + sent, segLen);
                conn->RetransmitLen = segLen;
                conn->RetransmitTime = Timekeeping::GetMilliseconds();
                conn->RetransmitCount = 0;
            }

            sent += segLen;
        }

        conn->Lock.Release();
        asm volatile("push %0; popfq" :: "r"(flags) : "memory");

        // Phase 2: Wait for ACK without holding the lock (interrupts enabled)
        uint64_t startTime = Timekeeping::GetMilliseconds();
        while (conn->SendUnack != conn->SendNext) {
            uint64_t now = Timekeeping::GetMilliseconds();
            if ((now - startTime) > (RETRANSMIT_TIMEOUT_MS * MAX_RETRANSMITS)) {
                break;
            }
            if ((now - conn->RetransmitTime) > RETRANSMIT_TIMEOUT_MS && conn->RetransmitLen > 0) {
                conn->RetransmitCount++;
                if (conn->RetransmitCount > MAX_RETRANSMITS) {
                    break;
                }
                // Retransmit with brief interrupt-disabled window
                asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
                conn->Lock.Acquire();
                uint32_t savedNext = conn->SendNext;
                conn->SendNext = conn->SendUnack;
                SendSegment(conn, FLAG_ACK | FLAG_PSH,
                           conn->RetransmitBuffer, conn->RetransmitLen);
                conn->SendNext = savedNext;
                conn->RetransmitTime = Timekeeping::GetMilliseconds();
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");
            }
            Timekeeping::Sleep(10);
        }

        return sent;
    }

    int Receive(Connection* conn, uint8_t* buffer, uint16_t bufferSize) {
        if (conn == nullptr) {
            return -1;
        }

        // Block until data is available or connection is closing
        while (true) {
            uint64_t flags;
            asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
            conn->Lock.Acquire();

            if (conn->RecvCount > 0) {
                uint16_t toRead = conn->RecvCount;
                if (toRead > bufferSize) {
                    toRead = bufferSize;
                }

                for (uint16_t i = 0; i < toRead; i++) {
                    buffer[i] = conn->RecvBuffer[conn->RecvHead];
                    conn->RecvHead = (conn->RecvHead + 1) % RECV_BUFFER_SIZE;
                }
                conn->RecvCount -= toRead;

                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");
                return toRead;
            }

            if (conn->CurrentState == State::CloseWait ||
                conn->CurrentState == State::Closed ||
                conn->CurrentState == State::TimeWait) {
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");
                return 0; // Connection closed
            }

            conn->Lock.Release();
            asm volatile("push %0; popfq" :: "r"(flags) : "memory");
            Timekeeping::Sleep(10);
        }
    }

    int ReceiveNonBlocking(Connection* conn, uint8_t* buffer, uint16_t bufferSize) {
        if (conn == nullptr) {
            return -1;
        }

        // Disable interrupts while holding the lock to prevent deadlock
        // with OnPacketReceived (called from the network interrupt handler)
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

        conn->Lock.Acquire();

        int result;
        if (conn->RecvCount > 0) {
            uint16_t toRead = conn->RecvCount;
            if (toRead > bufferSize) {
                toRead = bufferSize;
            }

            for (uint16_t i = 0; i < toRead; i++) {
                buffer[i] = conn->RecvBuffer[conn->RecvHead];
                conn->RecvHead = (conn->RecvHead + 1) % RECV_BUFFER_SIZE;
            }
            conn->RecvCount -= toRead;
            result = toRead;
        } else if (conn->CurrentState == State::CloseWait ||
                   conn->CurrentState == State::Closed ||
                   conn->CurrentState == State::TimeWait) {
            result = -1; // Connection closed
        } else {
            result = 0; // No data available
        }

        conn->Lock.Release();
        asm volatile("push %0; popfq" :: "r"(flags) : "memory");

        return result;
    }

    void Close(Connection* conn) {
        if (conn == nullptr) {
            return;
        }

        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        conn->Lock.Acquire();

        switch (conn->CurrentState) {
            case State::Established: {
                conn->CurrentState = State::FinWait1;
                SendSegment(conn, FLAG_FIN | FLAG_ACK, nullptr, 0);
                conn->SendNext++;
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");

                // Wait for close to complete
                for (int i = 0; i < 100; i++) {
                    if (conn->CurrentState == State::TimeWait ||
                        conn->CurrentState == State::Closed) {
                        break;
                    }
                    Timekeeping::Sleep(50);
                }
                conn->Active = false;
                return;
            }

            case State::CloseWait: {
                conn->CurrentState = State::LastAck;
                SendSegment(conn, FLAG_FIN | FLAG_ACK, nullptr, 0);
                conn->SendNext++;
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");

                // Wait for final ACK
                for (int i = 0; i < 100; i++) {
                    if (conn->CurrentState == State::Closed) {
                        break;
                    }
                    Timekeeping::Sleep(50);
                }
                conn->Active = false;
                return;
            }

            case State::Listen:
            case State::SynSent: {
                conn->CurrentState = State::Closed;
                conn->Active = false;
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");
                return;
            }

            default:
                conn->Lock.Release();
                asm volatile("push %0; popfq" :: "r"(flags) : "memory");
                conn->Active = false;
                return;
        }
    }

    State GetState(Connection* conn) {
        if (conn == nullptr) {
            return State::Closed;
        }
        return conn->CurrentState;
    }

}
