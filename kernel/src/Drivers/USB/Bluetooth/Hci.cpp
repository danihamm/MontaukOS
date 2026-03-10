/*
    * Hci.cpp
    * Bluetooth HCI transport over USB
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Hci.hpp"
#include "L2cap.hpp"
#include <Drivers/USB/Xhci.hpp>
#include <Drivers/USB/UsbDevice.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Libraries/Memory.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

namespace Drivers::USB::Bluetooth::Hci {

    // =========================================================================
    // State
    // =========================================================================

    static uint8_t g_slotId = 0;
    static bool g_initialized = false;

    // Event receive buffer (filled by xHCI interrupt IN callback)
    static uint8_t g_eventBuf[256] = {};
    static volatile uint32_t g_eventLen = 0;
    static volatile bool g_eventReady = false;

    // ACL receive buffer
    static uint8_t g_aclRxBuf[1024] = {};
    static volatile uint32_t g_aclRxLen = 0;
    static volatile bool g_aclRxReady = false;

    // ACL transmit DMA buffer
    static uint8_t* g_aclTxBuf = nullptr;
    static uint64_t g_aclTxBufPhys = 0;

    // HCI command DMA buffer (separate from ACL to avoid conflicts)
    static uint8_t* g_cmdDmaBuf = nullptr;
    static uint64_t g_cmdDmaBufPhys = 0;

    // Connection table
    static ConnectionInfo g_connections[MAX_CONNECTIONS] = {};

    // ACL buffer size (from controller)
    static uint16_t g_aclMaxLen = 0;
    static uint16_t g_aclMaxNum = 0;
    static volatile uint16_t g_aclPendingCount = 0;

    // Inquiry results
    static InquiryDevice g_inquiryResults[MAX_INQUIRY_RESULTS] = {};
    static volatile int g_inquiryResultCount = 0;
    static volatile bool g_inquiryActive = false;

    // =========================================================================
    // USB transfer callback
    // =========================================================================

    static void TransferCallback(uint8_t slotId, uint8_t epDci,
                                 const uint8_t* data, uint32_t length,
                                 uint32_t completionCode) {
        if (slotId != g_slotId) return;

        auto* dev = Xhci::GetDevice(slotId);
        if (!dev) return;

        uint8_t intDci = dev->InterruptEpNum ? (dev->InterruptEpNum * 2 + 1) : 0;
        uint8_t bulkInDci = dev->BulkInEpNum ? (dev->BulkInEpNum * 2 + 1) : 0;

        if (epDci == intDci && data && length > 0) {
            // HCI Event received on interrupt IN.
            // Dispatch asynchronous events (inquiry results, connection events,
            // etc.) immediately so they are never lost.  Only buffer
            // Command Complete / Command Status events — those are consumed
            // by WaitCommandComplete / WaitCommandStatus.
            uint8_t evtCode = (length >= 1) ? data[0] : 0;

            if (evtCode == EVT_COMMAND_COMPLETE || evtCode == EVT_COMMAND_STATUS) {
                uint32_t copyLen = length;
                if (copyLen > sizeof(g_eventBuf)) copyLen = sizeof(g_eventBuf);
                memcpy(g_eventBuf, data, copyLen);
                g_eventLen = copyLen;
                g_eventReady = true;
            } else {
                // Process immediately (inquiry results, connection events, etc.)
                ProcessEvent(data, length);
            }

            // Re-queue interrupt transfer for next event
            Xhci::QueueInterruptTransfer(slotId);
        } else if (epDci == bulkInDci && data && length > 0) {
            // ACL data received on bulk IN
            uint32_t copyLen = length;
            if (copyLen > sizeof(g_aclRxBuf)) copyLen = sizeof(g_aclRxBuf);
            memcpy(g_aclRxBuf, data, copyLen);
            g_aclRxLen = copyLen;
            g_aclRxReady = true;

            // Re-queue bulk IN transfer
            Xhci::QueueBulkInTransfer(slotId, nullptr, 0, dev->BulkInMaxPacket);
        } else if (epDci == (dev->BulkOutEpNum ? (uint8_t)(dev->BulkOutEpNum * 2) : (uint8_t)0)) {
            // Bulk OUT completion — decrement pending count
            if (g_aclPendingCount > 0) g_aclPendingCount--;
        }
    }

    // =========================================================================
    // Busy wait with event polling
    // =========================================================================

    static void BusyWaitMs(uint64_t ms) {
        uint64_t start = Timekeeping::GetMilliseconds();
        while (Timekeeping::GetMilliseconds() - start < ms) {
            asm volatile("pause" ::: "memory");
        }
    }

    // Poll for events while waiting
    static void PollWait(uint32_t ms) {
        uint64_t start = Timekeeping::GetMilliseconds();
        while (Timekeeping::GetMilliseconds() - start < ms) {
            Xhci::PollEvents();
            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    void Initialize(uint8_t slotId) {
        g_slotId = slotId;

        // Register our transfer callback
        Xhci::RegisterTransferCallback(slotId, TransferCallback);

        // Allocate DMA buffers for HCI commands and ACL data
        g_cmdDmaBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        g_cmdDmaBufPhys = Memory::SubHHDM(g_cmdDmaBuf);

        g_aclTxBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        g_aclTxBufPhys = Memory::SubHHDM(g_aclTxBuf);

        // NOTE: Do NOT queue interrupt IN or bulk IN transfers here.
        // The BT controller is not yet HCI-initialized and may misbehave.
        // Call StartEventPipe() after HCI Reset and initial setup.

        g_initialized = true;
        KernelLogStream(OK, "BT-HCI") << "HCI transport initialized on slot " << (uint64_t)slotId;
    }

    // Start receiving HCI events and ACL data — call after HCI init sequence
    void StartEventPipe() {
        if (!g_initialized) return;

        // Queue initial interrupt IN transfer for HCI events
        Xhci::QueueInterruptTransfer(g_slotId);

        // Queue initial bulk IN transfer for ACL data
        auto* dev = Xhci::GetDevice(g_slotId);
        if (dev && dev->BulkInEpNum) {
            Xhci::QueueBulkInTransfer(g_slotId, nullptr, 0, dev->BulkInMaxPacket);
        }

        KernelLogStream(INFO, "BT-HCI") << "Event pipe started (interrupt IN + bulk IN)";
    }

    // =========================================================================
    // SendCommand — via USB control transfer on EP0
    // =========================================================================

    bool SendCommand(uint16_t opcode, const uint8_t* params, uint8_t paramLen) {
        if (!g_initialized || !g_cmdDmaBuf) return false;

        // HCI command packet: opcode (2) + paramLen (1) + params
        // USB-BT spec: HCI commands are sent via control transfer
        // bmRequestType = 0x20 (Host-to-device, Class, Device)
        // bRequest = 0x00
        // wValue = 0, wIndex = 0
        // wLength = sizeof(CommandHeader) + paramLen

        // Use DMA-allocated buffer (not stack) for the command data.
        // xHCI reads from this buffer via DMA for OUT transfers.
        memset(g_cmdDmaBuf, 0, 512);
        g_cmdDmaBuf[0] = (uint8_t)(opcode & 0xFF);
        g_cmdDmaBuf[1] = (uint8_t)(opcode >> 8);
        g_cmdDmaBuf[2] = paramLen;
        if (params && paramLen > 0) {
            memcpy(&g_cmdDmaBuf[3], params, paramLen);
        }

        uint16_t totalLen = 3 + paramLen;

        uint32_t cc = Xhci::ControlTransfer(g_slotId,
            0x20,    // bmRequestType: Host-to-device, Class, Device
            0x00,    // bRequest: 0
            0x0000,  // wValue
            0x0000,  // wIndex
            totalLen,
            g_cmdDmaBuf,
            false);  // dirIn = false (host to device)

        if (cc != Xhci::CC_SUCCESS) {
            KernelLogStream(WARNING, "BT-HCI") << "SendCommand failed, opcode="
                << base::hex << (uint64_t)opcode << " cc=" << base::dec << (uint64_t)cc;
            return false;
        }

        return true;
    }

    // =========================================================================
    // WaitCommandComplete
    // =========================================================================

    bool WaitCommandComplete(uint16_t opcode, uint8_t* outParams,
                             uint8_t maxLen, uint32_t timeoutMs) {
        uint64_t start = Timekeeping::GetMilliseconds();

        while (Timekeeping::GetMilliseconds() - start < timeoutMs) {
            Xhci::PollEvents();

            if (g_eventReady) {
                g_eventReady = false;

                if (g_eventLen >= 2) {
                    uint8_t evtCode = g_eventBuf[0];
                    uint8_t evtParamLen = g_eventBuf[1];

                    if (evtCode == EVT_COMMAND_COMPLETE && evtParamLen >= 3) {
                        // Command Complete: NumPkts(1) + Opcode(2) + Status(1) + Params
                        uint16_t evtOpcode = (uint16_t)g_eventBuf[3] | ((uint16_t)g_eventBuf[4] << 8);
                        if (evtOpcode == opcode) {
                            if (outParams && maxLen > 0) {
                                // Copy params starting after the status byte
                                uint8_t availLen = (evtParamLen > 4) ? (evtParamLen - 4) : 0;
                                uint8_t copyLen = (availLen < maxLen) ? availLen : maxLen;
                                // Include status byte + return params
                                copyLen = (evtParamLen > 3) ? (evtParamLen - 3) : 0;
                                if (copyLen > maxLen) copyLen = maxLen;
                                memcpy(outParams, &g_eventBuf[5], copyLen);
                            }
                            // Check status
                            uint8_t status = g_eventBuf[5];
                            if (status != 0) {
                                KernelLogStream(WARNING, "BT-HCI") << "Command Complete status="
                                    << (uint64_t)status << " opcode=" << base::hex << (uint64_t)opcode;
                            }
                            return true;
                        }
                    }

                }
            }

            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }

        KernelLogStream(WARNING, "BT-HCI") << "WaitCommandComplete timeout, opcode="
            << base::hex << (uint64_t)opcode;
        return false;
    }

    // =========================================================================
    // WaitCommandStatus
    // =========================================================================

    bool WaitCommandStatus(uint16_t opcode, uint32_t timeoutMs) {
        uint64_t start = Timekeeping::GetMilliseconds();

        while (Timekeeping::GetMilliseconds() - start < timeoutMs) {
            Xhci::PollEvents();

            if (g_eventReady) {
                g_eventReady = false;

                if (g_eventLen >= 2) {
                    uint8_t evtCode = g_eventBuf[0];
                    uint8_t evtParamLen = g_eventBuf[1];

                    if (evtCode == EVT_COMMAND_STATUS && evtParamLen >= 4) {
                        uint8_t status = g_eventBuf[2];
                        uint16_t evtOpcode = (uint16_t)g_eventBuf[4] | ((uint16_t)g_eventBuf[5] << 8);
                        if (evtOpcode == opcode) {
                            return (status == 0);
                        }
                    }

                }
            }

            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }

        return false;
    }

    // =========================================================================
    // SendAcl — via USB bulk OUT
    // =========================================================================

    bool SendAcl(uint16_t handle, uint16_t pbFlag, const uint8_t* data, uint16_t len) {
        if (!g_initialized || !g_aclTxBuf) return false;
        if (len + sizeof(AclHeader) > 4096) return false;  // Single page DMA buffer

        // Build ACL packet in DMA buffer
        auto* hdr = (AclHeader*)g_aclTxBuf;
        hdr->HandleFlags = (handle & 0x0FFF) | pbFlag;
        hdr->DataLength = len;
        if (data && len > 0) {
            memcpy(g_aclTxBuf + sizeof(AclHeader), data, len);
        }

        uint32_t totalLen = sizeof(AclHeader) + len;

        g_aclPendingCount++;
        Xhci::QueueBulkOutTransfer(g_slotId, g_aclTxBuf, g_aclTxBufPhys, totalLen);

        return true;
    }

    // =========================================================================
    // ProcessEvent — handle HCI events
    // =========================================================================

    void ProcessEvent(const uint8_t* data, uint32_t len) {
        if (len < 2) return;

        uint8_t evtCode = data[0];
        uint8_t evtParamLen = data[1];
        const uint8_t* params = data + 2;

        switch (evtCode) {
            case EVT_CONNECTION_COMPLETE: {
                if (evtParamLen >= 11) {
                    uint8_t status = params[0];
                    uint16_t handle = (uint16_t)params[1] | ((uint16_t)params[2] << 8);
                    const uint8_t* bdAddr = &params[3];
                    uint8_t linkType = params[9];

                    KernelLogStream(INFO, "BT-HCI") << "Connection Complete: status="
                        << (uint64_t)status << " handle=" << (uint64_t)handle
                        << " link=" << (uint64_t)linkType;

                    if (status == 0) {
                        // Find empty connection slot
                        for (int i = 0; i < MAX_CONNECTIONS; i++) {
                            if (!g_connections[i].Active) {
                                g_connections[i].Active = true;
                                g_connections[i].Handle = handle;
                                memcpy(g_connections[i].BdAddr, bdAddr, 6);
                                g_connections[i].LinkType = linkType;
                                g_connections[i].Encrypted = false;
                                break;
                            }
                        }

                        // Initialize L2CAP for this connection
                        L2cap::Initialize(handle);
                    }
                }
                break;
            }

            case EVT_DISCONNECTION_COMPLETE: {
                if (evtParamLen >= 4) {
                    uint16_t handle = (uint16_t)params[1] | ((uint16_t)params[2] << 8);
                    uint8_t reason = params[3];

                    KernelLogStream(INFO, "BT-HCI") << "Disconnection: handle="
                        << (uint64_t)handle << " reason=" << (uint64_t)reason;

                    for (int i = 0; i < MAX_CONNECTIONS; i++) {
                        if (g_connections[i].Active && g_connections[i].Handle == handle) {
                            g_connections[i].Active = false;
                            break;
                        }
                    }
                }
                break;
            }

            case EVT_CONNECTION_REQUEST: {
                if (evtParamLen >= 10) {
                    const uint8_t* bdAddr = &params[0];
                    uint8_t linkType = params[9];

                    KernelLogStream(INFO, "BT-HCI") << "Connection Request: link="
                        << (uint64_t)linkType;

                    // Auto-accept ACL connections
                    if (linkType == 0x01) {
                        AcceptConnection(bdAddr, 0x01);  // Role = slave
                    }
                }
                break;
            }

            case EVT_NUM_COMPLETED_PACKETS: {
                if (evtParamLen >= 1) {
                    uint8_t numHandles = params[0];
                    for (int i = 0; i < numHandles && (3 + i * 4) < evtParamLen; i++) {
                        uint16_t completed = (uint16_t)params[3 + i * 4]
                                           | ((uint16_t)params[4 + i * 4] << 8);
                        if (g_aclPendingCount >= completed) {
                            g_aclPendingCount -= completed;
                        } else {
                            g_aclPendingCount = 0;
                        }
                    }
                }
                break;
            }

            case EVT_IO_CAPABILITY_REQUEST: {
                if (evtParamLen >= 6) {
                    // Reply with NoInputNoOutput for simple pairing
                    uint8_t reply[9] = {};
                    memcpy(reply, &params[0], 6);  // BD_ADDR
                    reply[6] = 0x03;  // IO Capability: NoInputNoOutput
                    reply[7] = 0x00;  // OOB data not present
                    reply[8] = 0x00;  // Authentication requirements: MITM not required
                    SendCommand(OP_IO_CAPABILITY_REPLY, reply, 9);
                    WaitCommandComplete(OP_IO_CAPABILITY_REPLY, nullptr, 0, 1000);
                }
                break;
            }

            case EVT_USER_CONFIRM_REQUEST: {
                if (evtParamLen >= 6) {
                    // Auto-confirm
                    SendCommand(OP_USER_CONFIRM_REPLY, &params[0], 6);
                    WaitCommandComplete(OP_USER_CONFIRM_REPLY, nullptr, 0, 1000);
                }
                break;
            }

            case EVT_INQUIRY_COMPLETE: {
                g_inquiryActive = false;
                KernelLogStream(INFO, "BT-HCI") << "Inquiry complete, "
                    << (uint64_t)g_inquiryResultCount << " device(s) found";
                break;
            }

            case EVT_INQUIRY_RESULT: {
                // Standard inquiry result: NumResp(1) + per-device(14 bytes each)
                if (evtParamLen >= 1) {
                    uint8_t numResp = params[0];
                    for (int i = 0; i < numResp && g_inquiryResultCount < MAX_INQUIRY_RESULTS; i++) {
                        const uint8_t* entry = &params[1 + i * 14];
                        auto& dev = g_inquiryResults[g_inquiryResultCount];
                        memset(&dev, 0, sizeof(dev));
                        memcpy(dev.BdAddr, entry, 6);
                        dev.ClassOfDevice = (uint32_t)entry[9]
                                          | ((uint32_t)entry[10] << 8)
                                          | ((uint32_t)entry[11] << 16);
                        dev.Rssi = -128; // Unknown for standard inquiry
                        g_inquiryResultCount++;
                    }
                }
                break;
            }

            case EVT_INQUIRY_RESULT_RSSI: {
                // Inquiry Result with RSSI: NumResp(1) + per-device(15 bytes each)
                if (evtParamLen >= 1) {
                    uint8_t numResp = params[0];
                    for (int i = 0; i < numResp && g_inquiryResultCount < MAX_INQUIRY_RESULTS; i++) {
                        const uint8_t* entry = &params[1 + i * 15];
                        auto& dev = g_inquiryResults[g_inquiryResultCount];
                        memset(&dev, 0, sizeof(dev));
                        memcpy(dev.BdAddr, entry, 6);
                        dev.ClassOfDevice = (uint32_t)entry[9]
                                          | ((uint32_t)entry[10] << 8)
                                          | ((uint32_t)entry[11] << 16);
                        dev.Rssi = (int8_t)entry[14];
                        g_inquiryResultCount++;
                    }
                }
                break;
            }

            case EVT_EXTENDED_INQUIRY_RESULT: {
                // Extended Inquiry Result: NumResp(1) + BD_ADDR(6) + PSRM(1) + reserved(1)
                //   + CoD(3) + ClockOff(2) + RSSI(1) + EIR(240)
                if (evtParamLen >= 15 && g_inquiryResultCount < MAX_INQUIRY_RESULTS) {
                    auto& dev = g_inquiryResults[g_inquiryResultCount];
                    memset(&dev, 0, sizeof(dev));
                    memcpy(dev.BdAddr, &params[1], 6);
                    dev.ClassOfDevice = (uint32_t)params[9]
                                      | ((uint32_t)params[10] << 8)
                                      | ((uint32_t)params[11] << 16);
                    dev.Rssi = (int8_t)params[14];

                    // Parse EIR data for device name
                    const uint8_t* eir = &params[15];
                    int eirLen = evtParamLen - 15;
                    int pos = 0;
                    while (pos < eirLen && pos < 240) {
                        uint8_t len = eir[pos];
                        if (len == 0) break;
                        if (pos + 1 + len > eirLen) break;
                        uint8_t type = eir[pos + 1];
                        // Type 0x08 = Shortened Local Name, 0x09 = Complete Local Name
                        if (type == 0x08 || type == 0x09) {
                            int nameLen = len - 1;
                            if (nameLen > 63) nameLen = 63;
                            memcpy(dev.Name, &eir[pos + 2], nameLen);
                            dev.Name[nameLen] = '\0';
                        }
                        pos += 1 + len;
                    }

                    g_inquiryResultCount++;
                }
                break;
            }

            case EVT_ENCRYPT_CHANGE: {
                if (evtParamLen >= 4) {
                    uint16_t handle = (uint16_t)params[1] | ((uint16_t)params[2] << 8);
                    uint8_t encryption = params[3];

                    for (int i = 0; i < MAX_CONNECTIONS; i++) {
                        if (g_connections[i].Active && g_connections[i].Handle == handle) {
                            g_connections[i].Encrypted = (encryption != 0);
                            break;
                        }
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    // =========================================================================
    // ProcessAcl — handle incoming ACL data
    // =========================================================================

    void ProcessAcl(const uint8_t* data, uint32_t len) {
        if (len < sizeof(AclHeader)) return;

        auto* hdr = (const AclHeader*)data;
        uint16_t handle = hdr->HandleFlags & 0x0FFF;
        uint16_t pbFlag = hdr->HandleFlags & 0x3000;
        uint16_t dataLen = hdr->DataLength;

        if (dataLen + sizeof(AclHeader) > len) return;

        // Dispatch to L2CAP
        L2cap::ProcessPacket(handle, data + sizeof(AclHeader), dataLen);
    }

    // =========================================================================
    // Connection management
    // =========================================================================

    ConnectionInfo* GetConnection(uint16_t handle) {
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (g_connections[i].Active && g_connections[i].Handle == handle) {
                return &g_connections[i];
            }
        }
        return nullptr;
    }

    ConnectionInfo* GetActiveConnection() {
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (g_connections[i].Active) {
                return &g_connections[i];
            }
        }
        return nullptr;
    }

    ConnectionInfo* GetConnectionByIndex(int index) {
        if (index < 0 || index >= MAX_CONNECTIONS) return nullptr;
        return &g_connections[index];
    }

    // =========================================================================
    // Convenience HCI commands
    // =========================================================================

    bool Reset() {
        if (!SendCommand(OP_RESET, nullptr, 0)) return false;
        BusyWaitMs(100);
        return WaitCommandComplete(OP_RESET, nullptr, 0, 5000);
    }

    bool ReadBdAddr(uint8_t* addr) {
        if (!SendCommand(OP_READ_BD_ADDR, nullptr, 0)) return false;
        uint8_t params[7] = {};
        if (!WaitCommandComplete(OP_READ_BD_ADDR, params, sizeof(params))) return false;
        // params[0] = status, params[1..6] = BD_ADDR
        if (params[0] != 0) return false;
        memcpy(addr, &params[1], 6);
        return true;
    }

    bool ReadLocalVersion(LocalVersion* ver) {
        if (!SendCommand(OP_READ_LOCAL_VERSION, nullptr, 0)) return false;
        uint8_t params[9] = {};
        if (!WaitCommandComplete(OP_READ_LOCAL_VERSION, params, sizeof(params))) return false;
        if (params[0] != 0) return false;
        if (ver) {
            ver->Status = params[0];
            ver->HciVersion = params[1];
            ver->HciRevision = (uint16_t)params[2] | ((uint16_t)params[3] << 8);
            ver->LmpVersion = params[4];
            ver->Manufacturer = (uint16_t)params[5] | ((uint16_t)params[6] << 8);
            ver->LmpSubversion = (uint16_t)params[7] | ((uint16_t)params[8] << 8);
        }
        return true;
    }

    bool ReadIntelVersion(IntelVersion* ver) {
        // Newer Intel BT controllers (AX200/AX201/AX211, THP+) require a
        // parameter byte of 0xFF for 0xFC05 to return the full version in
        // TLV format.  Try with the parameter first; fall back to the
        // legacy (no-param) format if the command fails.
        uint8_t param = 0xFF;
        bool sent = SendCommand(OP_INTEL_READ_VERSION, &param, 1);
        if (!sent) {
            // Fallback: legacy format (no parameter)
            sent = SendCommand(OP_INTEL_READ_VERSION, nullptr, 0);
        }
        if (!sent) return false;

        uint8_t params[32] = {};
        if (!WaitCommandComplete(OP_INTEL_READ_VERSION, params, sizeof(params))) return false;

        // Log raw response for diagnostics
        KernelLogStream(INFO, "BT-HCI") << "Intel version raw: "
            << base::hex
            << (uint64_t)params[0] << " " << (uint64_t)params[1] << " "
            << (uint64_t)params[2] << " " << (uint64_t)params[3] << " "
            << (uint64_t)params[4] << " " << (uint64_t)params[5] << " "
            << (uint64_t)params[6] << " " << (uint64_t)params[7] << " "
            << (uint64_t)params[8] << " " << (uint64_t)params[9]
            << base::dec;

        if (ver) memcpy(ver, params, sizeof(IntelVersion));
        return true;
    }

    bool WriteLocalName(const char* name) {
        uint8_t params[248] = {};
        int i = 0;
        for (; i < 247 && name[i]; i++) params[i] = name[i];
        params[i] = '\0';
        if (!SendCommand(OP_WRITE_LOCAL_NAME, params, 248)) return false;
        return WaitCommandComplete(OP_WRITE_LOCAL_NAME);
    }

    bool WriteClassOfDevice(uint32_t cod) {
        uint8_t params[3] = {
            (uint8_t)(cod & 0xFF),
            (uint8_t)((cod >> 8) & 0xFF),
            (uint8_t)((cod >> 16) & 0xFF)
        };
        if (!SendCommand(OP_WRITE_CLASS_OF_DEVICE, params, 3)) return false;
        return WaitCommandComplete(OP_WRITE_CLASS_OF_DEVICE);
    }

    bool WriteScanEnable(uint8_t mode) {
        if (!SendCommand(OP_WRITE_SCAN_ENABLE, &mode, 1)) return false;
        return WaitCommandComplete(OP_WRITE_SCAN_ENABLE);
    }

    bool WriteSSPMode(uint8_t mode) {
        if (!SendCommand(OP_WRITE_SSP_MODE, &mode, 1)) return false;
        return WaitCommandComplete(OP_WRITE_SSP_MODE);
    }

    bool AcceptConnection(const uint8_t* bdAddr, uint8_t role) {
        uint8_t params[7];
        memcpy(params, bdAddr, 6);
        params[6] = role;
        if (!SendCommand(OP_ACCEPT_CONN_REQ, params, 7)) return false;
        return WaitCommandStatus(OP_ACCEPT_CONN_REQ);
    }

    bool Disconnect(uint16_t handle, uint8_t reason) {
        uint8_t params[3] = {
            (uint8_t)(handle & 0xFF),
            (uint8_t)((handle >> 8) & 0xFF),
            reason
        };
        if (!SendCommand(OP_DISCONNECT, params, 3)) return false;
        return WaitCommandStatus(OP_DISCONNECT);
    }

    bool ReadBufferSize(uint16_t* aclLen, uint8_t* scoLen,
                        uint16_t* aclNum, uint16_t* scoNum) {
        if (!SendCommand(OP_READ_BUFFER_SIZE, nullptr, 0)) return false;
        uint8_t params[8] = {};
        if (!WaitCommandComplete(OP_READ_BUFFER_SIZE, params, sizeof(params))) return false;
        if (params[0] != 0) return false;
        if (aclLen) *aclLen = (uint16_t)params[1] | ((uint16_t)params[2] << 8);
        if (scoLen) *scoLen = params[3];
        if (aclNum) *aclNum = (uint16_t)params[4] | ((uint16_t)params[5] << 8);
        if (scoNum) *scoNum = (uint16_t)params[6] | ((uint16_t)params[7] << 8);
        g_aclMaxLen = (uint16_t)params[1] | ((uint16_t)params[2] << 8);
        g_aclMaxNum = (uint16_t)params[4] | ((uint16_t)params[5] << 8);
        return true;
    }

    // =========================================================================
    // Inquiry (device discovery)
    // =========================================================================

    bool StartInquiry(uint8_t durationUnits) {
        g_inquiryResultCount = 0;
        g_inquiryActive = true;

        // HCI Inquiry: LAP(3) + InquiryLength(1) + NumResponses(1)
        // GIAC LAP = 0x9E8B33
        uint8_t params[5] = {
            0x33, 0x8B, 0x9E,   // LAP (General Inquiry Access Code)
            durationUnits,       // Duration in 1.28s units
            0x00                 // Unlimited responses
        };

        if (!SendCommand(OP_INQUIRY, params, 5)) {
            g_inquiryActive = false;
            return false;
        }

        // Inquiry uses Command Status (not Command Complete)
        if (!WaitCommandStatus(OP_INQUIRY)) {
            g_inquiryActive = false;
            return false;
        }

        return true;
    }

    bool CancelInquiry() {
        if (!g_inquiryActive) return true;
        if (!SendCommand(OP_INQUIRY_CANCEL, nullptr, 0)) return false;
        WaitCommandComplete(OP_INQUIRY_CANCEL, nullptr, 0, 2000);
        g_inquiryActive = false;
        return true;
    }

    int GetInquiryResults(InquiryDevice* buf, int maxCount) {
        int count = g_inquiryResultCount;
        if (count > maxCount) count = maxCount;
        if (buf && count > 0) {
            memcpy(buf, g_inquiryResults, count * sizeof(InquiryDevice));
        }
        return count;
    }

    void ClearInquiryResults() {
        g_inquiryResultCount = 0;
    }

    bool IsInquiryActive() {
        return g_inquiryActive;
    }

    // =========================================================================
    // Create ACL connection
    // =========================================================================

    void DrainEvents() {
        // Discard any unconsumed Command Complete/Status events that weren't
        // picked up by WaitCommandComplete/WaitCommandStatus.
        if (g_eventReady) {
            g_eventReady = false;
        }

        // Drain ACL data
        if (g_aclRxReady) {
            g_aclRxReady = false;
            if (g_aclRxLen > 0) {
                ProcessAcl(g_aclRxBuf, g_aclRxLen);
            }
        }
    }

    bool CreateConnection(const uint8_t* bdAddr) {
        // HCI Create Connection:
        // BD_ADDR(6) + PacketType(2) + PSRM(1) + reserved(1) + ClockOffset(2) + AllowRoleSwitch(1)
        uint8_t params[13] = {};
        memcpy(params, bdAddr, 6);
        // Packet types: DM1, DH1, DM3, DH3, DM5, DH5
        params[6] = 0x18;  // CC18 = allow DM1, DH1, DM3, DH3, DM5, DH5
        params[7] = 0xCC;
        params[8] = 0x02;  // Page Scan Repetition Mode R2
        params[9] = 0x00;  // Reserved
        params[10] = 0x00; // Clock offset
        params[11] = 0x00;
        params[12] = 0x01; // Allow role switch

        if (!SendCommand(OP_CREATE_CONNECTION, params, 13)) return false;

        // Create Connection uses Command Status, then Connection Complete event
        return WaitCommandStatus(OP_CREATE_CONNECTION, 5000);
    }

}
