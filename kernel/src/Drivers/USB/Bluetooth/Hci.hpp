/*
    * Hci.hpp
    * Bluetooth HCI (Host Controller Interface) layer
    * HCI transport over USB bulk/interrupt/control endpoints
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::Bluetooth::Hci {

    // =========================================================================
    // HCI packet types (for USB transport)
    // =========================================================================

    // USB transport uses different endpoints for each packet type:
    // Commands  -> Control EP0 (class request)
    // ACL data  -> Bulk OUT / Bulk IN
    // Events    -> Interrupt IN

    // =========================================================================
    // HCI command opcodes (OGF << 10 | OCF)
    // =========================================================================

    // Link Control (OGF 0x01)
    constexpr uint16_t OP_INQUIRY               = 0x0401;
    constexpr uint16_t OP_INQUIRY_CANCEL         = 0x0402;
    constexpr uint16_t OP_CREATE_CONNECTION      = 0x0405;
    constexpr uint16_t OP_DISCONNECT             = 0x0406;
    constexpr uint16_t OP_ACCEPT_CONN_REQ        = 0x0409;
    constexpr uint16_t OP_REJECT_CONN_REQ        = 0x040A;
    constexpr uint16_t OP_AUTH_REQUESTED         = 0x0411;
    constexpr uint16_t OP_SET_CONN_ENCRYPT       = 0x0413;
    constexpr uint16_t OP_IO_CAPABILITY_REPLY    = 0x042B;
    constexpr uint16_t OP_USER_CONFIRM_REPLY     = 0x042C;

    // Link Policy (OGF 0x02)
    constexpr uint16_t OP_WRITE_DEFAULT_LP       = 0x080F;
    constexpr uint16_t OP_SNIFF_MODE             = 0x0803;

    // Controller & Baseband (OGF 0x03)
    constexpr uint16_t OP_RESET                  = 0x0C03;
    constexpr uint16_t OP_SET_EVENT_FILTER       = 0x0C05;
    constexpr uint16_t OP_WRITE_LOCAL_NAME       = 0x0C13;
    constexpr uint16_t OP_READ_LOCAL_NAME        = 0x0C14;
    constexpr uint16_t OP_WRITE_SCAN_ENABLE      = 0x0C1A;
    constexpr uint16_t OP_WRITE_CLASS_OF_DEVICE  = 0x0C24;
    constexpr uint16_t OP_WRITE_SSP_MODE         = 0x0C56;
    constexpr uint16_t OP_WRITE_INQUIRY_MODE     = 0x0C45;
    constexpr uint16_t OP_WRITE_PAGE_TIMEOUT     = 0x0C18;
    constexpr uint16_t OP_WRITE_AUTH_ENABLE      = 0x0C20;
    constexpr uint16_t OP_SET_EVENT_MASK         = 0x0C01;

    // Informational Parameters (OGF 0x04)
    constexpr uint16_t OP_READ_BD_ADDR           = 0x1009;
    constexpr uint16_t OP_READ_LOCAL_VERSION     = 0x1001;
    constexpr uint16_t OP_READ_LOCAL_FEATURES    = 0x1003;
    constexpr uint16_t OP_READ_BUFFER_SIZE       = 0x1005;

    // Intel vendor commands (OGF 0x3F)
    constexpr uint16_t OP_INTEL_READ_VERSION     = 0xFC05;
    constexpr uint16_t OP_INTEL_RESET            = 0xFC01;
    constexpr uint16_t OP_INTEL_SET_EVENT_MASK   = 0xFC52;
    constexpr uint16_t OP_INTEL_DDC_CONFIG_WRITE = 0xFC8B;

    // =========================================================================
    // HCI event codes
    // =========================================================================

    constexpr uint8_t EVT_INQUIRY_COMPLETE       = 0x01;
    constexpr uint8_t EVT_INQUIRY_RESULT         = 0x02;
    constexpr uint8_t EVT_CONNECTION_COMPLETE     = 0x03;
    constexpr uint8_t EVT_CONNECTION_REQUEST      = 0x04;
    constexpr uint8_t EVT_DISCONNECTION_COMPLETE  = 0x05;
    constexpr uint8_t EVT_AUTH_COMPLETE           = 0x06;
    constexpr uint8_t EVT_ENCRYPT_CHANGE          = 0x08;
    constexpr uint8_t EVT_COMMAND_COMPLETE        = 0x0E;
    constexpr uint8_t EVT_COMMAND_STATUS          = 0x0F;
    constexpr uint8_t EVT_NUM_COMPLETED_PACKETS   = 0x13;
    constexpr uint8_t EVT_IO_CAPABILITY_REQUEST   = 0x31;
    constexpr uint8_t EVT_IO_CAPABILITY_RESPONSE  = 0x32;
    constexpr uint8_t EVT_USER_CONFIRM_REQUEST    = 0x33;
    constexpr uint8_t EVT_SIMPLE_PAIRING_COMPLETE = 0x36;
    constexpr uint8_t EVT_INQUIRY_RESULT_RSSI     = 0x22;
    constexpr uint8_t EVT_EXTENDED_INQUIRY_RESULT = 0x2F;
    constexpr uint8_t EVT_VENDOR_SPECIFIC         = 0xFF;

    // =========================================================================
    // Inquiry result storage
    // =========================================================================

    struct InquiryDevice {
        uint8_t  BdAddr[6];
        uint8_t  _pad[2];
        uint32_t ClassOfDevice;
        int8_t   Rssi;
        uint8_t  _pad2[3];
        char     Name[64];       // From Extended Inquiry Result or Remote Name Request
    };

    constexpr int MAX_INQUIRY_RESULTS = 16;

    // =========================================================================
    // HCI packet headers
    // =========================================================================

    struct CommandHeader {
        uint16_t Opcode;
        uint8_t  ParamLength;
    } __attribute__((packed));

    struct EventHeader {
        uint8_t  EventCode;
        uint8_t  ParamLength;
    } __attribute__((packed));

    struct AclHeader {
        uint16_t HandleFlags;   // bits 11:0 = handle, 13:12 = PB flag, 15:14 = BC flag
        uint16_t DataLength;
    } __attribute__((packed));

    // ACL PB (Packet Boundary) flag values
    constexpr uint16_t ACL_PB_FIRST_NON_FLUSH = 0x0000;  // First non-auto-flushable
    constexpr uint16_t ACL_PB_CONTINUING      = 0x1000;  // Continuing fragment
    constexpr uint16_t ACL_PB_FIRST_FLUSH     = 0x2000;  // First auto-flushable

    // =========================================================================
    // HCI connection info
    // =========================================================================

    struct ConnectionInfo {
        bool     Active;
        uint16_t Handle;
        uint8_t  BdAddr[6];
        uint8_t  LinkType;      // 0x01 = ACL
        bool     Encrypted;
    };

    constexpr int MAX_CONNECTIONS = 4;

    // =========================================================================
    // Intel Bluetooth version info
    // =========================================================================

    struct IntelVersion {
        uint8_t  Status;
        uint8_t  HwPlatform;
        uint8_t  HwVariant;
        uint8_t  HwRevision;
        uint8_t  FwVariant;     // 0x06 = bootloader, 0x23 = operational
        uint8_t  FwRevision;
        uint8_t  FwBuildNum;
        uint8_t  FwBuildWw;
        uint8_t  FwBuildYy;
        uint8_t  FwPatchNum;
    } __attribute__((packed));

    // =========================================================================
    // Public API
    // =========================================================================

    // Initialize HCI transport over USB for the given slot
    void Initialize(uint8_t slotId);

    // Start receiving HCI events and ACL data (call after HCI init sequence)
    void StartEventPipe();

    // Send an HCI command via USB control transfer (EP0)
    bool SendCommand(uint16_t opcode, const uint8_t* params, uint8_t paramLen);

    // Wait for a Command Complete event matching the given opcode
    // Returns true if received within timeout, fills outParams (excluding status byte)
    bool WaitCommandComplete(uint16_t opcode, uint8_t* outParams = nullptr,
                             uint8_t maxLen = 0, uint32_t timeoutMs = 2000);

    // Wait for a Command Status event matching the given opcode
    bool WaitCommandStatus(uint16_t opcode, uint32_t timeoutMs = 2000);

    // Send ACL data via USB bulk OUT
    bool SendAcl(uint16_t handle, uint16_t pbFlag, const uint8_t* data, uint16_t len);

    // Process an HCI event received on the interrupt IN endpoint
    void ProcessEvent(const uint8_t* data, uint32_t len);

    // Process ACL data received on the bulk IN endpoint
    void ProcessAcl(const uint8_t* data, uint32_t len);

    // Get connection info
    ConnectionInfo* GetConnection(uint16_t handle);
    ConnectionInfo* GetActiveConnection();
    ConnectionInfo* GetConnectionByIndex(int index);  // 0..MAX_CONNECTIONS-1

    // HCI Reset command
    bool Reset();

    // Read local BD_ADDR
    bool ReadBdAddr(uint8_t* addr);

    // Read standard HCI local version info
    struct LocalVersion {
        uint8_t  Status;
        uint8_t  HciVersion;
        uint16_t HciRevision;
        uint8_t  LmpVersion;
        uint16_t Manufacturer;
        uint16_t LmpSubversion;
    } __attribute__((packed));

    bool ReadLocalVersion(LocalVersion* ver);

    // Read Intel-specific version info
    bool ReadIntelVersion(IntelVersion* ver);

    // Set local name
    bool WriteLocalName(const char* name);

    // Set class of device
    bool WriteClassOfDevice(uint32_t cod);

    // Enable scan (inquiry + page)
    bool WriteScanEnable(uint8_t mode);

    // Write Simple Secure Pairing mode
    bool WriteSSPMode(uint8_t mode);

    // Accept an incoming connection
    bool AcceptConnection(const uint8_t* bdAddr, uint8_t role);

    // Disconnect a connection
    bool Disconnect(uint16_t handle, uint8_t reason);

    // Read ACL buffer size from controller
    bool ReadBufferSize(uint16_t* aclLen, uint8_t* scoLen,
                        uint16_t* aclNum, uint16_t* scoNum);

    // Inquiry (device discovery)
    bool StartInquiry(uint8_t durationUnits);   // duration in 1.28s units (e.g., 8 = ~10s)
    bool CancelInquiry();
    int  GetInquiryResults(InquiryDevice* buf, int maxCount);
    void ClearInquiryResults();
    bool IsInquiryActive();

    // Create ACL connection to a remote device
    bool CreateConnection(const uint8_t* bdAddr);

    // Drain any pending HCI events (call in poll loops that aren't inside
    // WaitCommandComplete/WaitCommandStatus)
    void DrainEvents();

}
