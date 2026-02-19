/*
    * Ethernet.cpp
    * Ethernet frame layer
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Ethernet.hpp"
#include <Net/ByteOrder.hpp>
#include <Net/Arp.hpp>
#include <Net/Ipv4.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Net::Ethernet {

    static const uint8_t* GetActiveNicMac() {
        if (Drivers::Net::E1000::IsInitialized())
            return Drivers::Net::E1000::GetMacAddress();
        return Drivers::Net::E1000E::GetMacAddress();
    }

    static bool ActiveNicSend(const uint8_t* data, uint16_t length) {
        if (Drivers::Net::E1000::IsInitialized())
            return Drivers::Net::E1000::SendPacket(data, length);
        return Drivers::Net::E1000E::SendPacket(data, length);
    }

    void Initialize() {
        KernelLogStream(OK, "Net") << "Ethernet layer initialized";
    }

    bool Send(const uint8_t* destMac, uint16_t etherType, const uint8_t* payload, uint16_t payloadLen) {
        if (payload == nullptr || payloadLen == 0 || payloadLen > MAX_PAYLOAD_SIZE) {
            return false;
        }

        uint8_t frame[MAX_FRAME_SIZE];
        Header* hdr = (Header*)frame;

        memcpy(hdr->DestMac, destMac, 6);
        memcpy(hdr->SrcMac, GetActiveNicMac(), 6);
        hdr->EtherType = Htons(etherType);

        memcpy(frame + HEADER_SIZE, payload, payloadLen);

        uint16_t totalLen = HEADER_SIZE + payloadLen;

        return ActiveNicSend(frame, totalLen);
    }

    void OnFrameReceived(const uint8_t* data, uint16_t length) {
        if (length < HEADER_SIZE) {
            return;
        }

        const Header* hdr = (const Header*)data;
        uint16_t etherType = Ntohs(hdr->EtherType);
        const uint8_t* payload = data + HEADER_SIZE;
        uint16_t payloadLen = length - HEADER_SIZE;

        switch (etherType) {
            case ETHERTYPE_ARP:
                Arp::OnPacketReceived(payload, payloadLen);
                break;
            case ETHERTYPE_IPV4:
                Ipv4::OnPacketReceived(payload, payloadLen);
                break;
            default:
                break;
        }
    }

}
