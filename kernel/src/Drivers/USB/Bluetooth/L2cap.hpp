/*
    * L2cap.hpp
    * Bluetooth L2CAP (Logical Link Control and Adaptation Protocol)
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::Bluetooth::L2cap {

    // =========================================================================
    // L2CAP CIDs (Channel Identifiers)
    // =========================================================================

    constexpr uint16_t CID_SIGNALING = 0x0001;  // L2CAP signaling
    constexpr uint16_t CID_CONNLESS  = 0x0002;  // Connectionless reception
    constexpr uint16_t CID_DYNAMIC_START = 0x0040;  // First dynamic CID

    // =========================================================================
    // L2CAP PSMs (Protocol/Service Multiplexers)
    // =========================================================================

    constexpr uint16_t PSM_SDP    = 0x0001;  // Service Discovery Protocol
    constexpr uint16_t PSM_AVDTP  = 0x0019;  // Audio/Video Distribution Transport

    // =========================================================================
    // L2CAP packet header
    // =========================================================================

    struct L2capHeader {
        uint16_t Length;
        uint16_t ChannelId;
    } __attribute__((packed));

    // =========================================================================
    // L2CAP signaling command header
    // =========================================================================

    struct SignalHeader {
        uint8_t  Code;
        uint8_t  Identifier;
        uint16_t Length;
    } __attribute__((packed));

    // Signaling command codes
    constexpr uint8_t SIG_COMMAND_REJECT   = 0x01;
    constexpr uint8_t SIG_CONN_REQ         = 0x02;
    constexpr uint8_t SIG_CONN_RSP         = 0x03;
    constexpr uint8_t SIG_CONFIG_REQ       = 0x04;
    constexpr uint8_t SIG_CONFIG_RSP       = 0x05;
    constexpr uint8_t SIG_DISCONN_REQ      = 0x06;
    constexpr uint8_t SIG_DISCONN_RSP      = 0x07;
    constexpr uint8_t SIG_INFO_REQ         = 0x0A;
    constexpr uint8_t SIG_INFO_RSP         = 0x0B;

    // Connection response results
    constexpr uint16_t CONN_SUCCESS        = 0x0000;
    constexpr uint16_t CONN_PENDING        = 0x0001;
    constexpr uint16_t CONN_REFUSED_PSM    = 0x0002;

    // Configuration response results
    constexpr uint16_t CFG_SUCCESS         = 0x0000;

    // =========================================================================
    // L2CAP channel info
    // =========================================================================

    struct ChannelInfo {
        bool     Active;
        uint16_t LocalCid;
        uint16_t RemoteCid;
        uint16_t Psm;
        uint16_t RemoteMtu;
        bool     Configured;  // Both sides configured
        bool     LocalConfigDone;
        bool     RemoteConfigDone;
    };

    constexpr int MAX_CHANNELS = 8;

    // =========================================================================
    // Public API
    // =========================================================================

    // Initialize L2CAP for a new HCI connection
    void Initialize(uint16_t aclHandle);

    // Process an L2CAP packet (called from HCI ACL processing)
    void ProcessPacket(uint16_t aclHandle, const uint8_t* data, uint16_t len);

    // Connect to a remote PSM (initiate L2CAP connection)
    // Returns local CID, or 0 on failure
    uint16_t Connect(uint16_t psm);

    // Wait for connection to be configured
    bool WaitConfigured(uint16_t localCid, uint32_t timeoutMs = 5000);

    // Send data on an L2CAP channel
    bool SendData(uint16_t localCid, const uint8_t* data, uint16_t len);

    // Get channel info
    ChannelInfo* GetChannel(uint16_t localCid);

    // Find channel by PSM
    ChannelInfo* FindChannelByPsm(uint16_t psm);

    // Get the ACL handle
    uint16_t GetAclHandle();

}
