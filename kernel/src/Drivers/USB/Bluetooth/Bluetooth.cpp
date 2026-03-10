/*
    * Bluetooth.cpp
    * Top-level Bluetooth subsystem — adapter registration and Intel BT initialization
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Bluetooth.hpp"
#include "Hci.hpp"
#include "A2dp.hpp"
#include <Drivers/USB/Xhci.hpp>
#include <Drivers/USB/UsbDevice.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Libraries/Memory.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

namespace Drivers::USB::Bluetooth {

    // =========================================================================
    // State
    // =========================================================================

    static bool g_initialized = false;
    static uint8_t g_slotId = 0;
    static uint8_t g_bdAddr[6] = {};

    // Intel Bluetooth device IDs
    static bool IsIntelBt(uint16_t vid, uint16_t pid) {
        if (vid != 0x8087) return false;
        // Known Intel Bluetooth USB product IDs
        switch (pid) {
            case 0x0032:  // AX211 variant
            case 0x0033:  // AX211
            case 0x0036:  // AX211 variant
            case 0x0038:  // AX211 variant
            case 0x0AAA:  // AX200
            case 0x0026:  // AX201
            case 0x0029:  // AX201 variant
            case 0x0025:  // 9560
            case 0x0A2B:  // 8265
            case 0x0A2A:  // 8260
            case 0x07DC:  // 8265 variant
            case 0x0AA7:  // AX200 variant
                return true;
            default:
                return false;
        }
    }

    // =========================================================================
    // Intel Bluetooth firmware detection
    // =========================================================================

    static bool InitIntelBluetooth(uint8_t slotId) {
        KernelLogStream(INFO, "BT") << "Intel Bluetooth adapter detected";

        // Intel BT controllers require HCI Reset before they respond to
        // vendor-specific commands.  This mirrors the Linux btintel driver
        // sequence: Reset → Read Version → (firmware load) → Reset.
        if (!Hci::Reset()) {
            KernelLogStream(ERROR, "BT") << "Initial HCI Reset failed";
            return false;
        }

        // Read standard HCI version -- if this fails, the controller is likely
        // in bootloader mode where only vendor commands are accepted.
        Hci::LocalVersion lver = {};
        bool hciVersionOk = Hci::ReadLocalVersion(&lver);
        if (hciVersionOk) {
            KernelLogStream(INFO, "BT") << "HCI version=" << (uint64_t)lver.HciVersion
                << " rev=" << base::hex << (uint64_t)lver.HciRevision
                << " LMP=" << (uint64_t)lver.LmpVersion
                << " manufacturer=" << (uint64_t)lver.Manufacturer
                << " subver=" << (uint64_t)lver.LmpSubversion << base::dec;
        }

        // Read Intel version to check firmware state
        Hci::IntelVersion ver = {};
        if (!Hci::ReadIntelVersion(&ver)) {
            KernelLogStream(WARNING, "BT") << "Failed to read Intel BT version";
        } else {
            KernelLogStream(INFO, "BT") << "Intel BT: HW variant=" << (uint64_t)ver.HwVariant
                << " FW variant=" << base::hex << (uint64_t)ver.FwVariant
                << " FW rev=" << (uint64_t)ver.FwRevision << "."
                << (uint64_t)ver.FwBuildNum << base::dec;

            if (ver.FwVariant == 0x23) {
                KernelLogStream(OK, "BT") << "Intel BT firmware already loaded (operational mode)";
            } else if (ver.FwVariant == 0x06) {
                KernelLogStream(WARNING, "BT") << "Intel BT in bootloader mode, firmware not loaded";
                KernelLogStream(WARNING, "BT") << "Bluetooth will have limited functionality without firmware";
            } else if (!hciVersionOk) {
                // Standard HCI commands failed AND Intel version is zeros/unknown
                // -> controller is in bootloader mode, needs firmware download
                KernelLogStream(WARNING, "BT") << "Intel BT in bootloader mode (FW not loaded by UEFI)";
                KernelLogStream(WARNING, "BT") << "Bluetooth requires firmware download for full functionality";
            } else {
                KernelLogStream(INFO, "BT") << "Intel BT firmware variant: "
                    << base::hex << (uint64_t)ver.FwVariant;
            }
        }

        return true;
    }

    // =========================================================================
    // RegisterAdapter — entry point from USB enumeration
    // =========================================================================

    void RegisterAdapter(uint8_t slotId) {
        if (g_initialized) {
            KernelLogStream(WARNING, "BT") << "Bluetooth adapter already registered";
            return;
        }

        g_slotId = slotId;

        // Initialize HCI transport (allocates DMA buffers, registers callback)
        // NOTE: Does NOT queue receive transfers yet — device isn't ready
        Hci::Initialize(slotId);

        auto* dev = Xhci::GetDevice(slotId);
        if (!dev) return;

        // Wait for the USB device to be ready after SET_CONFIGURATION
        // Intel BT controllers need 200-500ms after config before accepting HCI
        uint64_t start = Timekeeping::GetMilliseconds();
        while (Timekeeping::GetMilliseconds() - start < 200) {
            Xhci::PollEvents();
            asm volatile("pause" ::: "memory");
        }

        // Start the event pipe BEFORE sending any HCI commands.
        // HCI command responses arrive as events on the interrupt IN endpoint,
        // so it must be queued to receive them.
        Hci::StartEventPipe();

        // Intel-specific initialization (includes HCI Reset)
        bool didReset = false;
        if (IsIntelBt(dev->VendorId, dev->ProductId)) {
            if (InitIntelBluetooth(slotId)) {
                didReset = true;  // InitIntelBluetooth already sent HCI Reset
            } else {
                KernelLogStream(WARNING, "BT") << "Intel BT init failed, continuing with basic HCI";
            }
        }

        // Standard HCI Reset (skip if Intel init already did one)
        if (!didReset) {
            if (!Hci::Reset()) {
                KernelLogStream(ERROR, "BT") << "HCI Reset failed";
                return;
            }
        }

        // Read BD_ADDR
        if (Hci::ReadBdAddr(g_bdAddr)) {
            KernelLogStream(OK, "BT") << "BD_ADDR: "
                << base::hex
                << (uint64_t)g_bdAddr[5] << ":" << (uint64_t)g_bdAddr[4] << ":"
                << (uint64_t)g_bdAddr[3] << ":" << (uint64_t)g_bdAddr[2] << ":"
                << (uint64_t)g_bdAddr[1] << ":" << (uint64_t)g_bdAddr[0] << base::dec;
        }

        // Read buffer size
        uint16_t aclLen = 0, aclNum = 0;
        uint8_t scoLen = 0;
        uint16_t scoNum = 0;
        if (Hci::ReadBufferSize(&aclLen, &scoLen, &aclNum, &scoNum)) {
            KernelLogStream(INFO, "BT") << "ACL buffer: " << (uint64_t)aclLen
                << " bytes x " << (uint64_t)aclNum;
        }

        // Set local name
        Hci::WriteLocalName("MontaukOS");

        // Set class of device: Audio (Major Service: Audio, Major Class: Audio/Video)
        // CoD: 0x240404 = Rendering | Audio | Wearable Headset (common for audio devices)
        // For a computer acting as audio source:
        // 0x200408 = Audio service | Audio/Video class | Portable Audio
        Hci::WriteClassOfDevice(0x200408);

        // Enable Simple Secure Pairing
        Hci::WriteSSPMode(1);

        // Set event mask to receive relevant events
        uint8_t eventMask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20};
        Hci::SendCommand(Hci::OP_SET_EVENT_MASK, eventMask, 8);
        Hci::WaitCommandComplete(Hci::OP_SET_EVENT_MASK);

        // Enable inquiry + page scan (discoverable and connectable)
        Hci::WriteScanEnable(0x03);

        g_initialized = true;
        KernelLogStream(OK, "BT") << "Bluetooth adapter initialized successfully";
    }

    // =========================================================================
    // Public queries
    // =========================================================================

    bool IsInitialized() {
        return g_initialized;
    }

    uint8_t GetSlotId() {
        return g_slotId;
    }

    const uint8_t* GetBdAddr() {
        return g_bdAddr;
    }

    // =========================================================================
    // Scan — blocking inquiry
    // =========================================================================

    int Scan(Hci::InquiryDevice* buf, int maxCount, uint32_t timeoutMs) {
        if (!g_initialized || !buf || maxCount <= 0) return -1;

        Hci::ClearInquiryResults();

        // Convert timeout to 1.28s units (min 1, max 30)
        uint8_t duration = (uint8_t)(timeoutMs / 1280);
        if (duration < 1) duration = 1;
        if (duration > 30) duration = 30;

        if (!Hci::StartInquiry(duration)) return -1;

        // Poll until inquiry completes or timeout
        uint64_t start = Timekeeping::GetMilliseconds();
        while (Hci::IsInquiryActive() && (Timekeeping::GetMilliseconds() - start < timeoutMs)) {
            Xhci::PollEvents();
            Hci::DrainEvents();

            for (int j = 0; j < 200; j++) {
                asm volatile("pause" ::: "memory");
            }
        }

        // Cancel if still running
        if (Hci::IsInquiryActive()) {
            Hci::CancelInquiry();
        }

        return Hci::GetInquiryResults(buf, maxCount);
    }

    // =========================================================================
    // Connect — initiate ACL connection
    // =========================================================================

    int Connect(const uint8_t* bdAddr, uint32_t timeoutMs) {
        if (!g_initialized || !bdAddr) return -1;

        if (!Hci::CreateConnection(bdAddr)) return -1;

        // Wait for Connection Complete event
        uint64_t start = Timekeeping::GetMilliseconds();
        while (Timekeeping::GetMilliseconds() - start < timeoutMs) {
            Xhci::PollEvents();
            Hci::DrainEvents();

            // Check connection table for matching BD_ADDR
            for (int i = 0; i < Hci::MAX_CONNECTIONS; i++) {
                auto* conn = Hci::GetConnectionByIndex(i);
                if (conn && conn->Active) {
                    bool match = true;
                    for (int j = 0; j < 6; j++) {
                        if (conn->BdAddr[j] != bdAddr[j]) { match = false; break; }
                    }
                    if (match) return 0;
                }
            }

            for (int j = 0; j < 200; j++) {
                asm volatile("pause" ::: "memory");
            }
        }

        return -1; // Timeout
    }

    // =========================================================================
    // Disconnect — disconnect a device by BD_ADDR
    // =========================================================================

    int Disconnect(const uint8_t* bdAddr) {
        if (!g_initialized || !bdAddr) return -1;

        // Find connection with matching BD_ADDR
        for (int i = 0; i < Hci::MAX_CONNECTIONS; i++) {
            auto* conn = Hci::GetConnectionByIndex(i);
            if (conn && conn->Active) {
                bool match = true;
                for (int j = 0; j < 6; j++) {
                    if (conn->BdAddr[j] != bdAddr[j]) { match = false; break; }
                }
                if (match) {
                    Hci::Disconnect(conn->Handle, 0x13); // 0x13 = Remote User Terminated
                    return 0;
                }
            }
        }

        return -1; // Not found
    }

    // =========================================================================
    // ListConnected — list active connections
    // =========================================================================

    int ListConnected(Hci::ConnectionInfo* buf, int maxCount) {
        if (!g_initialized || !buf || maxCount <= 0) return 0;

        int count = 0;
        for (int i = 0; i < Hci::MAX_CONNECTIONS && count < maxCount; i++) {
            auto* conn = Hci::GetConnectionByIndex(i);
            if (conn && conn->Active) {
                buf[count] = *conn;
                count++;
            }
        }
        return count;
    }

}
