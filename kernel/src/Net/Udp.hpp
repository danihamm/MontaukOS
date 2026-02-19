/*
    * Udp.hpp
    * User Datagram Protocol
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Net::Udp {

    constexpr uint16_t HEADER_SIZE = 8;

    struct Header {
        uint16_t SrcPort;
        uint16_t DstPort;
        uint16_t Length;
        uint16_t Checksum;
    } __attribute__((packed));

    // Callback type for receiving UDP data
    using RecvCallback = void(*)(uint32_t srcIp, uint16_t srcPort,
                                  uint16_t dstPort,
                                  const uint8_t* data, uint16_t length);

    // Initialize the UDP subsystem
    void Initialize();

    // Handle an incoming UDP packet (called by IPv4 layer)
    void OnPacketReceived(uint32_t srcIp, uint32_t dstIp, const uint8_t* data, uint16_t length);

    // Send a UDP datagram
    bool Send(uint32_t destIp, uint16_t srcPort, uint16_t destPort,
              const uint8_t* payload, uint16_t payloadLen);

    // Bind a callback to a local port. Returns true on success.
    bool Bind(uint16_t port, RecvCallback callback);

    // Unbind a port
    void Unbind(uint16_t port);

}
