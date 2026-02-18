/*
    * Tcp.hpp
    * Transmission Control Protocol
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <CppLib/Spinlock.hpp>

namespace Net::Tcp {

    constexpr uint16_t HEADER_SIZE = 20; // Without options

    // TCP flags
    constexpr uint8_t FLAG_FIN = 0x01;
    constexpr uint8_t FLAG_SYN = 0x02;
    constexpr uint8_t FLAG_RST = 0x04;
    constexpr uint8_t FLAG_PSH = 0x08;
    constexpr uint8_t FLAG_ACK = 0x10;

    struct Header {
        uint16_t SrcPort;
        uint16_t DstPort;
        uint32_t SeqNum;
        uint32_t AckNum;
        uint8_t  DataOffset; // Upper 4 bits = offset in 32-bit words
        uint8_t  Flags;
        uint16_t Window;
        uint16_t Checksum;
        uint16_t UrgentPtr;
    } __attribute__((packed));

    // TCP connection states
    enum class State {
        Closed,
        Listen,
        SynSent,
        SynReceived,
        Established,
        FinWait1,
        FinWait2,
        CloseWait,
        LastAck,
        TimeWait
    };

    // Opaque connection handle
    struct Connection;

    // Initialize the TCP subsystem
    void Initialize();

    // Handle an incoming TCP segment (called by IPv4 layer)
    void OnPacketReceived(uint32_t srcIp, uint32_t dstIp, const uint8_t* data, uint16_t length);

    // Listen on a port. Returns a connection handle in Listen state.
    Connection* Listen(uint16_t port);

    // Accept an incoming connection on a listening socket.
    // Blocks until a connection arrives. Returns a new connection in Established state.
    Connection* Accept(Connection* listener);

    // Actively connect to a remote host:port. Returns connection in Established state or nullptr.
    Connection* Connect(uint32_t destIp, uint16_t destPort, uint16_t srcPort);

    // Send data on an established connection. Returns number of bytes sent.
    int Send(Connection* conn, const uint8_t* data, uint16_t length);

    // Receive data from an established connection. Returns number of bytes received.
    // Blocks until data is available or connection is closed.
    int Receive(Connection* conn, uint8_t* buffer, uint16_t bufferSize);

    // Non-blocking receive. Returns bytes read, 0 if no data available, or -1 on closed/error.
    int ReceiveNonBlocking(Connection* conn, uint8_t* buffer, uint16_t bufferSize);

    // Close a TCP connection gracefully
    void Close(Connection* conn);

    // Get the state of a connection
    State GetState(Connection* conn);

}
