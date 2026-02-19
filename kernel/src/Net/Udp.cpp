/*
    * Udp.cpp
    * User Datagram Protocol
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Udp.hpp"
#include <Net/Ipv4.hpp>
#include <Net/ByteOrder.hpp>
#include <Net/NetConfig.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Net::Udp {

    struct PortBinding {
        uint16_t     Port;
        RecvCallback Callback;
        bool         Active;
    };

    static constexpr uint32_t MAX_BINDINGS = 16;
    static PortBinding g_bindings[MAX_BINDINGS] = {};

    void Initialize() {
        for (uint32_t i = 0; i < MAX_BINDINGS; i++) {
            g_bindings[i].Active = false;
        }
        KernelLogStream(OK, "Net") << "UDP initialized";
    }

    void OnPacketReceived(uint32_t srcIp, uint32_t dstIp, const uint8_t* data, uint16_t length) {
        if (length < HEADER_SIZE) {
            return;
        }

        const Header* hdr = (const Header*)data;
        uint16_t srcPort = Ntohs(hdr->SrcPort);
        uint16_t dstPort = Ntohs(hdr->DstPort);
        uint16_t udpLen = Ntohs(hdr->Length);

        if (udpLen < HEADER_SIZE || udpLen > length) {
            return;
        }

        // Verify checksum if present
        if (hdr->Checksum != 0) {
            uint16_t check = Ipv4::PseudoHeaderChecksum(srcIp, dstIp, Ipv4::PROTO_UDP,
                                                         udpLen, data, udpLen);
            if (check != 0) {
                return;
            }
        }

        const uint8_t* payload = data + HEADER_SIZE;
        uint16_t payloadLen = udpLen - HEADER_SIZE;

        // Dispatch to bound callback
        for (uint32_t i = 0; i < MAX_BINDINGS; i++) {
            if (g_bindings[i].Active && g_bindings[i].Port == dstPort) {
                g_bindings[i].Callback(srcIp, srcPort, dstPort, payload, payloadLen);
                return;
            }
        }
    }

    bool Send(uint32_t destIp, uint16_t srcPort, uint16_t destPort,
              const uint8_t* payload, uint16_t payloadLen) {
        uint16_t udpLen = HEADER_SIZE + payloadLen;
        uint8_t packet[1500];

        if (udpLen > sizeof(packet)) {
            return false;
        }

        Header* hdr = (Header*)packet;
        hdr->SrcPort = Htons(srcPort);
        hdr->DstPort = Htons(destPort);
        hdr->Length = Htons(udpLen);
        hdr->Checksum = 0;

        memcpy(packet + HEADER_SIZE, payload, payloadLen);

        // Calculate checksum with pseudo-header
        hdr->Checksum = Ipv4::PseudoHeaderChecksum(
            Net::GetIpAddress(), destIp, Ipv4::PROTO_UDP,
            udpLen, packet, udpLen);
        if (hdr->Checksum == 0) {
            hdr->Checksum = 0xFFFF; // RFC 768: zero checksum transmitted as all ones
        }

        return Ipv4::Send(destIp, Ipv4::PROTO_UDP, packet, udpLen);
    }

    bool Bind(uint16_t port, RecvCallback callback) {
        // Check for duplicate
        for (uint32_t i = 0; i < MAX_BINDINGS; i++) {
            if (g_bindings[i].Active && g_bindings[i].Port == port) {
                return false;
            }
        }

        // Find empty slot
        for (uint32_t i = 0; i < MAX_BINDINGS; i++) {
            if (!g_bindings[i].Active) {
                g_bindings[i].Port = port;
                g_bindings[i].Callback = callback;
                g_bindings[i].Active = true;
                return true;
            }
        }
        return false;
    }

    void Unbind(uint16_t port) {
        for (uint32_t i = 0; i < MAX_BINDINGS; i++) {
            if (g_bindings[i].Active && g_bindings[i].Port == port) {
                g_bindings[i].Active = false;
                return;
            }
        }
    }

}
