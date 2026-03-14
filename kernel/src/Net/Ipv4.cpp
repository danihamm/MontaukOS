/*
    * Ipv4.cpp
    * Internet Protocol version 4
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Ipv4.hpp"
#include <Net/ByteOrder.hpp>
#include <Net/Ethernet.hpp>
#include <Net/Arp.hpp>
#include <Net/Icmp.hpp>
#include <Net/Udp.hpp>
#include <Net/Tcp.hpp>
#include <Net/NetConfig.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Net::Ipv4 {

    static uint16_t g_identification = 0;

    // Deferred packet queue for packets awaiting ARP resolution
    struct PendingPacket {
        uint32_t DestIp;
        uint8_t  Protocol;
        uint8_t  Data[Ethernet::MAX_PAYLOAD_SIZE - HEADER_SIZE];
        uint16_t Length;
        bool     Active;
    };

    static constexpr uint32_t PENDING_QUEUE_SIZE = 8;
    static PendingPacket g_pendingQueue[PENDING_QUEUE_SIZE] = {};

    void Initialize() {
        g_identification = 0;
        KernelLogStream(OK, "Net") << "IPv4 initialized, IP: "
            << base::dec
            << (uint64_t)(GetIpAddress() & 0xFF) << "."
            << (uint64_t)((GetIpAddress() >> 8) & 0xFF) << "."
            << (uint64_t)((GetIpAddress() >> 16) & 0xFF) << "."
            << (uint64_t)((GetIpAddress() >> 24) & 0xFF);
    }

    uint16_t Checksum(const void* data, uint16_t length) {
        const uint16_t* ptr = (const uint16_t*)data;
        uint32_t sum = 0;

        while (length > 1) {
            sum += *ptr++;
            length -= 2;
        }

        // Handle odd byte
        if (length == 1) {
            sum += *(const uint8_t*)ptr;
        }

        // Fold 32-bit sum into 16 bits
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }

        return (uint16_t)(~sum);
    }

    uint16_t PseudoHeaderChecksum(uint32_t srcIp, uint32_t dstIp, uint8_t protocol,
                                   uint16_t length, const void* data, uint16_t dataLen) {
        uint32_t sum = 0;

        // Pseudo-header fields (already in network byte order)
        sum += (srcIp & 0xFFFF);
        sum += (srcIp >> 16);
        sum += (dstIp & 0xFFFF);
        sum += (dstIp >> 16);
        sum += Htons(protocol);
        sum += Htons(length);

        // Data
        const uint16_t* ptr = (const uint16_t*)data;
        uint16_t remaining = dataLen;
        while (remaining > 1) {
            sum += *ptr++;
            remaining -= 2;
        }
        if (remaining == 1) {
            sum += *(const uint8_t*)ptr;
        }

        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }

        return (uint16_t)(~sum);
    }

    void OnPacketReceived(const uint8_t* data, uint16_t length) {
        if (length < HEADER_SIZE) {
            return;
        }

        const Header* hdr = (const Header*)data;

        // Verify version
        uint8_t version = (hdr->VersionIhl >> 4) & 0xF;
        if (version != 4) {
            return;
        }

        // Get header length
        uint8_t ihl = (hdr->VersionIhl & 0xF) * 4;
        if (ihl < HEADER_SIZE || ihl > length) {
            return;
        }

        // Verify checksum
        if (Checksum(data, ihl) != 0) {
            return;
        }

        uint16_t totalLen = Ntohs(hdr->TotalLength);
        if (totalLen < ihl || totalLen > length) {
            return;
        }

        // Check destination: accept packets addressed to us or broadcast
        uint32_t ourIp = GetIpAddress();
        if (hdr->DstIp != ourIp && hdr->DstIp != 0xFFFFFFFF) {
            return;
        }

        const uint8_t* payload = data + ihl;
        uint16_t payloadLen = totalLen - ihl;

        switch (hdr->Protocol) {
            case PROTO_ICMP:
                Icmp::OnPacketReceived(hdr->SrcIp, payload, payloadLen);
                break;
            case PROTO_UDP:
                Udp::OnPacketReceived(hdr->SrcIp, hdr->DstIp, payload, payloadLen);
                break;
            case PROTO_TCP:
                Tcp::OnPacketReceived(hdr->SrcIp, hdr->DstIp, payload, payloadLen);
                break;
            default:
                break;
        }
    }

    // Build and send an IP packet over Ethernet (MAC already resolved)
    static bool SendDirect(uint32_t destIp, uint8_t protocol, const uint8_t* destMac,
                           const uint8_t* payload, uint16_t payloadLen) {
        uint8_t packet[Ethernet::MAX_PAYLOAD_SIZE];
        Header* hdr = (Header*)packet;

        hdr->VersionIhl = (4 << 4) | 5; // IPv4, 5 dwords (20 bytes)
        hdr->Tos = 0;
        hdr->TotalLength = Htons(HEADER_SIZE + payloadLen);
        hdr->Identification = Htons(g_identification++);
        hdr->FlagsFragment = 0;
        hdr->Ttl = DEFAULT_TTL;
        hdr->Protocol = protocol;
        hdr->Checksum = 0;
        hdr->SrcIp = GetIpAddress();
        hdr->DstIp = destIp;

        hdr->Checksum = Checksum(hdr, HEADER_SIZE);

        memcpy(packet + HEADER_SIZE, payload, payloadLen);

        return Ethernet::Send(destMac, Ethernet::ETHERTYPE_IPV4, packet, HEADER_SIZE + payloadLen);
    }

    bool Send(uint32_t destIp, uint8_t protocol, const uint8_t* payload, uint16_t payloadLen) {
        if (payloadLen > (Ethernet::MAX_PAYLOAD_SIZE - HEADER_SIZE)) {
            return false;
        }

        // Determine next-hop IP and resolve MAC
        uint32_t nextHop = GetNextHop(destIp);
        uint8_t destMac[6];

        if (Arp::Resolve(nextHop, destMac)) {
            return SendDirect(destIp, protocol, destMac, payload, payloadLen);
        }

        // ARP request already sent by Resolve(), queue the packet for later
        for (uint32_t i = 0; i < PENDING_QUEUE_SIZE; i++) {
            if (!g_pendingQueue[i].Active) {
                g_pendingQueue[i].DestIp = destIp;
                g_pendingQueue[i].Protocol = protocol;
                g_pendingQueue[i].Length = payloadLen;
                memcpy(g_pendingQueue[i].Data, payload, payloadLen);
                g_pendingQueue[i].Active = true;
                return true;
            }
        }

        // Queue full, drop the packet
        return false;
    }

    void FlushPending() {
        for (uint32_t i = 0; i < PENDING_QUEUE_SIZE; i++) {
            if (!g_pendingQueue[i].Active) {
                continue;
            }

            uint32_t nextHop = GetNextHop(g_pendingQueue[i].DestIp);
            uint8_t destMac[6];

            if (Arp::Resolve(nextHop, destMac)) {
                SendDirect(g_pendingQueue[i].DestIp, g_pendingQueue[i].Protocol,
                           destMac, g_pendingQueue[i].Data, g_pendingQueue[i].Length);
                g_pendingQueue[i].Active = false;
            }
        }
    }

}
