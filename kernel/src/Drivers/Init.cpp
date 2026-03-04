/*
    * Init.cpp
    * Driver initialization orchestration
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Init.hpp"
#include <Pci/Pci.hpp>
#include <Drivers/Graphics/IntelGPU.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Drivers/USB/Xhci.hpp>
#include <Graphics/Cursor.hpp>
#include <Net/Net.hpp>
#include <Terminal/Terminal.hpp>

namespace Drivers {

    // -------------------------------------------------------------------------
    // Device ID whitelists
    // -------------------------------------------------------------------------

    static constexpr uint16_t g_e1000Ids[] = {
        0x100E,
    };

    static constexpr uint16_t g_e1000eIds[] = {
        // I217
        0x153A, 0x153B,
        // I218
        0x155A, 0x1559, 0x15A0, 0x15A1, 0x15A2, 0x15A3,
        // I219-LM
        0x156F, 0x15B7, 0x15BB, 0x15BD, 0x15DF, 0x15E1, 0x15E3,
        0x15D7, 0x0D4C, 0x0D4E, 0x0D53, 0x0D55, 0x0DC5, 0x0DC7,
        0x1A1C, 0x1A1E,
        // I219-V
        0x1570, 0x15B8, 0x15BC, 0x15BE, 0x15E0, 0x15E2, 0x15D6,
        0x15D8, 0x0D4D, 0x0D4F, 0x0D54, 0x0DC6, 0x0DC8, 0x1A1D,
        0x1A1F,
    };

    // -------------------------------------------------------------------------
    // Probe wrappers (adapt namespace::Probe to PciProbeFunc signature)
    // -------------------------------------------------------------------------

    static bool ProbeIntelGPU(const Pci::PciDevice& dev) {
        return Graphics::IntelGPU::Probe(dev);
    }

    static bool ProbeXhci(const Pci::PciDevice& dev) {
        return USB::Xhci::Probe(dev);
    }

    static bool ProbeE1000(const Pci::PciDevice& dev) {
        return Net::E1000::Probe(dev);
    }

    static bool ProbeE1000E(const Pci::PciDevice& dev) {
        return Net::E1000E::Probe(dev);
    }

    // -------------------------------------------------------------------------
    // Driver table
    // -------------------------------------------------------------------------

    static constexpr Pci::PciDriverDesc g_driverTable[] = {
        // Order 1: Intel GPU — Early phase, match vendor=0x8086 + class=0x03
        {
            "IntelGPU",
            0x8086,                         // VendorId
            0x03,                           // ClassCode (Display)
            0xFF,                           // SubClass (any)
            0xFF,                           // ProgIf (any)
            nullptr,                        // DeviceIds (internal gen lookup)
            0,                              // DeviceIdCount
            Pci::ProbePhase::Early,
            ProbeIntelGPU,
        },
        // Order 2: xHCI — Normal phase, match class=0x0C/0x03/0x30
        {
            "xHCI",
            0,                              // VendorId (any)
            0x0C,                           // ClassCode (Serial Bus)
            0x03,                           // SubClass (USB)
            0x30,                           // ProgIf (xHCI)
            nullptr,
            0,
            Pci::ProbePhase::Normal,
            ProbeXhci,
        },
        // Order 3: E1000 — Normal phase, vendor=0x8086 + deviceId={0x100E}
        {
            "E1000",
            0x8086,
            0xFF, 0xFF, 0xFF,
            g_e1000Ids,
            sizeof(g_e1000Ids) / sizeof(g_e1000Ids[0]),
            Pci::ProbePhase::Normal,
            ProbeE1000,
        },
        // Order 4: E1000E — Normal phase, vendor=0x8086 + deviceIds list
        {
            "E1000E",
            0x8086,
            0xFF, 0xFF, 0xFF,
            g_e1000eIds,
            sizeof(g_e1000eIds) / sizeof(g_e1000eIds[0]),
            Pci::ProbePhase::Normal,
            ProbeE1000E,
        },
    };

    static constexpr uint16_t g_driverTableCount = sizeof(g_driverTable) / sizeof(g_driverTable[0]);

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void ProbeEarly() {
        Pci::ProbeAll(g_driverTable, g_driverTableCount, Pci::ProbePhase::Early);
    }

    void InitializeGraphics() {
        if (Graphics::IntelGPU::IsInitialized()) {
            ::Graphics::Cursor::SetFramebuffer(
                Graphics::IntelGPU::GetFramebufferBase(),
                Graphics::IntelGPU::GetWidth(),
                Graphics::IntelGPU::GetHeight(),
                Graphics::IntelGPU::GetPitch()
            );
        }
    }

    void ProbeNormal() {
        Pci::ProbeAll(g_driverTable, g_driverTableCount, Pci::ProbePhase::Normal);
    }

    void InitializeNetwork() {
        ::Net::Initialize();
    }

}
