/*
    * Arp.cpp
    * Address Resolution Protocol
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Arp.hpp"
#include <Net/ByteOrder.hpp>
#include <Net/Ethernet.hpp>
#include <Net/Ipv4.hpp>
#include <Net/NetConfig.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

namespace Net::Arp {

    static const uint8_t* GetActiveNicMac() {
        if (Drivers::Net::E1000::IsInitialized())
            return Drivers::Net::E1000::GetMacAddress();
        return Drivers::Net::E1000E::GetMacAddress();
    }

    // ARP cache entry
    struct CacheEntry {
        uint32_t Ip;
        uint8_t  Mac[6];
        uint64_t Timestamp;
        bool     Valid;
    };

    static constexpr uint32_t ARP_CACHE_SIZE = 32;
    static constexpr uint64_t ARP_CACHE_TIMEOUT_MS = 60000; // 60 seconds

    static CacheEntry g_cache[ARP_CACHE_SIZE] = {};

    void Initialize() {
        for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
            g_cache[i].Valid = false;
        }
        KernelLogStream(OK, "Net") << "ARP initialized";
    }

    static void CacheInsert(uint32_t ip, const uint8_t* mac) {
        // Look for existing entry or empty slot
        uint32_t emptySlot = ARP_CACHE_SIZE;
        for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
            if (g_cache[i].Valid && g_cache[i].Ip == ip) {
                // Update existing entry
                memcpy(g_cache[i].Mac, mac, 6);
                g_cache[i].Timestamp = Timekeeping::GetMilliseconds();
                return;
            }
            if (!g_cache[i].Valid && emptySlot == ARP_CACHE_SIZE) {
                emptySlot = i;
            }
        }

        if (emptySlot < ARP_CACHE_SIZE) {
            g_cache[emptySlot].Ip = ip;
            memcpy(g_cache[emptySlot].Mac, mac, 6);
            g_cache[emptySlot].Timestamp = Timekeeping::GetMilliseconds();
            g_cache[emptySlot].Valid = true;
        }
    }

    static bool CacheLookup(uint32_t ip, uint8_t* outMac) {
        uint64_t now = Timekeeping::GetMilliseconds();
        for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
            if (g_cache[i].Valid && g_cache[i].Ip == ip) {
                if ((now - g_cache[i].Timestamp) > ARP_CACHE_TIMEOUT_MS) {
                    g_cache[i].Valid = false;
                    return false;
                }
                memcpy(outMac, g_cache[i].Mac, 6);
                return true;
            }
        }
        return false;
    }

    void OnPacketReceived(const uint8_t* data, uint16_t length) {
        if (length < sizeof(Packet)) {
            return;
        }

        const Packet* pkt = (const Packet*)data;

        if (Ntohs(pkt->HardwareType) != HW_TYPE_ETHERNET ||
            Ntohs(pkt->ProtocolType) != PROTO_TYPE_IPV4) {
            return;
        }

        uint32_t senderIp = pkt->SenderIp; // Already in network byte order in struct
        uint32_t targetIp = pkt->TargetIp;

        // Cache the sender's IP->MAC mapping, then flush any packets waiting on it
        CacheInsert(senderIp, pkt->SenderMac);
        Ipv4::FlushPending();

        uint16_t op = Ntohs(pkt->Operation);

        if (op == OP_REQUEST && targetIp == GetIpAddress()) {
            // Someone is asking for our MAC address -- send a reply
            Packet reply;
            reply.HardwareType = Htons(HW_TYPE_ETHERNET);
            reply.ProtocolType = Htons(PROTO_TYPE_IPV4);
            reply.HardwareAddrLen = 6;
            reply.ProtocolAddrLen = 4;
            reply.Operation = Htons(OP_REPLY);

            memcpy(reply.SenderMac, GetActiveNicMac(), 6);
            reply.SenderIp = GetIpAddress();
            memcpy(reply.TargetMac, pkt->SenderMac, 6);
            reply.TargetIp = senderIp;

            Ethernet::Send(pkt->SenderMac, Ethernet::ETHERTYPE_ARP,
                          (const uint8_t*)&reply, sizeof(Packet));
        }
    }

    bool Resolve(uint32_t ip, uint8_t* outMac) {
        // Broadcast address
        if (ip == 0xFFFFFFFF) {
            memcpy(outMac, Ethernet::BROADCAST_MAC, 6);
            return true;
        }

        if (CacheLookup(ip, outMac)) {
            return true;
        }

        // Not in cache, send a request
        SendRequest(ip);
        return false;
    }

    void SendRequest(uint32_t targetIp) {
        Packet req;
        req.HardwareType = Htons(HW_TYPE_ETHERNET);
        req.ProtocolType = Htons(PROTO_TYPE_IPV4);
        req.HardwareAddrLen = 6;
        req.ProtocolAddrLen = 4;
        req.Operation = Htons(OP_REQUEST);

        memcpy(req.SenderMac, GetActiveNicMac(), 6);
        req.SenderIp = GetIpAddress();
        memset(req.TargetMac, 0, 6);
        req.TargetIp = targetIp;

        Ethernet::Send(Ethernet::BROADCAST_MAC, Ethernet::ETHERTYPE_ARP,
                      (const uint8_t*)&req, sizeof(Packet));
    }

}
