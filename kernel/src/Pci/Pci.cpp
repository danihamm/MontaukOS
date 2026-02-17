/*
    * Pci.cpp
    * PCI Express enumeration and configuration space access
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Pci.hpp"
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Io/IoPort.hpp>

using namespace Kt;

namespace Pci {

    // Legacy PCI config I/O ports
    static constexpr uint16_t ConfigAddressPort = 0xCF8;
    static constexpr uint16_t ConfigDataPort    = 0xCFC;

    // PCI config space register offsets
    static constexpr uint16_t RegVendorId   = 0x00;
    static constexpr uint16_t RegDeviceId   = 0x02;
    static constexpr uint16_t RegCommand    = 0x04;
    static constexpr uint16_t RegStatus     = 0x06;
    static constexpr uint16_t RegRevisionId = 0x08;
    static constexpr uint16_t RegProgIf     = 0x09;
    static constexpr uint16_t RegSubClass   = 0x0A;
    static constexpr uint16_t RegClassCode  = 0x0B;
    static constexpr uint16_t RegHeaderType = 0x0E;

    static kcp::vector<PciDevice> g_devices{};

    // State: ECAM base (0 if not available, use legacy)
    static uint64_t g_ecamBase = 0;
    static uint8_t g_ecamStartBus = 0;
    static uint8_t g_ecamEndBus = 0;
    static bool g_useEcam = false;

    // -------------------------------------------------------------------------
    // MCFG table discovery (same pattern as MADT)
    // -------------------------------------------------------------------------

    static bool SignatureMatch(const char* sig, const char* target, int len) {
        for (int i = 0; i < len; i++) {
            if (sig[i] != target[i]) return false;
        }
        return true;
    }

    static Hal::ACPI::CommonSDTHeader* FindMcfgInXsdt(Hal::ACPI::CommonSDTHeader* xsdt) {
        uint32_t entryCount = (xsdt->Length - sizeof(Hal::ACPI::CommonSDTHeader)) / 8;
        uint64_t* entries = (uint64_t*)((uint64_t)xsdt + sizeof(Hal::ACPI::CommonSDTHeader));

        for (uint32_t i = 0; i < entryCount; i++) {
            auto* header = (Hal::ACPI::CommonSDTHeader*)Memory::HHDM(entries[i]);
            if (SignatureMatch(header->Signature, "MCFG", 4)) {
                return header;
            }
        }

        return nullptr;
    }

    static bool ParseMcfg(Hal::ACPI::CommonSDTHeader* xsdt) {
        auto* mcfgHeader = FindMcfgInXsdt(xsdt);
        if (mcfgHeader == nullptr) {
            KernelLogStream(WARNING, "PCI") << "MCFG table not found, falling back to legacy config access";
            return false;
        }

        if (!Hal::ACPI::TestChecksum(mcfgHeader)) {
            KernelLogStream(ERROR, "PCI") << "MCFG checksum failed";
            return false;
        }

        KernelLogStream(OK, "PCI") << "Found MCFG table";

        auto* mcfg = (McfgHeader*)mcfgHeader;
        uint32_t entriesSize = mcfg->SDTHeader.Length - sizeof(McfgHeader);
        uint32_t entryCount = entriesSize / sizeof(McfgEntry);

        if (entryCount == 0) {
            KernelLogStream(WARNING, "PCI") << "MCFG contains no entries";
            return false;
        }

        auto* entries = (McfgEntry*)((uint64_t)mcfg + sizeof(McfgHeader));

        // Use the first MCFG entry (segment group 0)
        g_ecamBase = entries[0].BaseAddress;
        g_ecamStartBus = entries[0].StartBus;
        g_ecamEndBus = entries[0].EndBus;

        KernelLogStream(INFO, "PCI") << "ECAM base: " << base::hex << g_ecamBase
            << " buses " << base::dec << (uint64_t)g_ecamStartBus
            << "-" << (uint64_t)g_ecamEndBus;

        // Map the ECAM MMIO region
        // Size = (endBus - startBus + 1) * 32 devices * 8 functions * 4096 bytes
        uint64_t busCount = (uint64_t)(g_ecamEndBus - g_ecamStartBus + 1);
        uint64_t ecamSize = busCount * 32 * 8 * 4096;

        if (Memory::VMM::g_paging) {
            for (uint64_t offset = 0; offset < ecamSize; offset += 0x1000) {
                uint64_t phys = g_ecamBase + offset;
                Memory::VMM::g_paging->MapMMIO(phys, Memory::HHDM(phys));
            }
            KernelLogStream(DEBUG, "PCI") << "Mapped ECAM region: " << base::hex << ecamSize << " bytes";
        }

        g_useEcam = true;
        return true;
    }

    // -------------------------------------------------------------------------
    // ECAM (memory-mapped) configuration space access
    // -------------------------------------------------------------------------

    static volatile uint8_t* EcamAddress(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
        uint64_t phys = ecamBase
            + ((uint64_t)bus << 20)
            + ((uint64_t)device << 15)
            + ((uint64_t)function << 12)
            + offset;
        return (volatile uint8_t*)Memory::HHDM(phys);
    }

    uint8_t EcamRead8(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
        return *EcamAddress(ecamBase, bus, device, function, offset);
    }

    uint16_t EcamRead16(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
        return *(volatile uint16_t*)EcamAddress(ecamBase, bus, device, function, offset);
    }

    uint32_t EcamRead32(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
        return *(volatile uint32_t*)EcamAddress(ecamBase, bus, device, function, offset);
    }

    void EcamWrite8(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value) {
        *EcamAddress(ecamBase, bus, device, function, offset) = value;
    }

    void EcamWrite16(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value) {
        *(volatile uint16_t*)EcamAddress(ecamBase, bus, device, function, offset) = value;
    }

    void EcamWrite32(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value) {
        *(volatile uint32_t*)EcamAddress(ecamBase, bus, device, function, offset) = value;
    }

    // -------------------------------------------------------------------------
    // Legacy PCI configuration space access (I/O ports 0xCF8/0xCFC)
    // -------------------------------------------------------------------------

    static uint32_t LegacyBuildAddress(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
        return (1u << 31)                       // Enable bit
             | ((uint32_t)bus << 16)
             | ((uint32_t)(device & 0x1F) << 11)
             | ((uint32_t)(function & 0x07) << 8)
             | (offset & 0xFC);
    }

    uint32_t LegacyRead32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
        Io::Out32(LegacyBuildAddress(bus, device, function, offset), ConfigAddressPort);
        return Io::In32(ConfigDataPort);
    }

    uint16_t LegacyRead16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
        uint32_t val = LegacyRead32(bus, device, function, offset & 0xFC);
        return (uint16_t)(val >> ((offset & 2) * 8));
    }

    uint8_t LegacyRead8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
        uint32_t val = LegacyRead32(bus, device, function, offset & 0xFC);
        return (uint8_t)(val >> ((offset & 3) * 8));
    }

    void LegacyWrite32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
        Io::Out32(LegacyBuildAddress(bus, device, function, offset), ConfigAddressPort);
        Io::Out32(value, ConfigDataPort);
    }

    void LegacyWrite16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
        uint32_t addr = LegacyBuildAddress(bus, device, function, offset & 0xFC);
        Io::Out32(addr, ConfigAddressPort);
        uint32_t tmp = Io::In32(ConfigDataPort);
        int shift = (offset & 2) * 8;
        tmp &= ~(0xFFFF << shift);
        tmp |= ((uint32_t)value << shift);
        Io::Out32(addr, ConfigAddressPort);
        Io::Out32(tmp, ConfigDataPort);
    }

    void LegacyWrite8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value) {
        uint32_t addr = LegacyBuildAddress(bus, device, function, offset & 0xFC);
        Io::Out32(addr, ConfigAddressPort);
        uint32_t tmp = Io::In32(ConfigDataPort);
        int shift = (offset & 3) * 8;
        tmp &= ~(0xFF << shift);
        tmp |= ((uint32_t)value << shift);
        Io::Out32(addr, ConfigAddressPort);
        Io::Out32(tmp, ConfigDataPort);
    }

    // -------------------------------------------------------------------------
    // Unified read helpers (use ECAM if available, else legacy)
    // -------------------------------------------------------------------------

    static uint16_t ReadConfig16(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
        if (g_useEcam) {
            return EcamRead16(g_ecamBase, bus, device, function, offset);
        }
        return LegacyRead16(bus, device, function, (uint8_t)offset);
    }

    static uint8_t ReadConfig8(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
        if (g_useEcam) {
            return EcamRead8(g_ecamBase, bus, device, function, offset);
        }
        return LegacyRead8(bus, device, function, (uint8_t)offset);
    }

    // -------------------------------------------------------------------------
    // PCI class code names
    // -------------------------------------------------------------------------

    const char* GetClassName(uint8_t classCode, uint8_t subClass) {
        switch (classCode) {
            case 0x00:
                return "Unclassified";
            case 0x01:
                switch (subClass) {
                    case 0x00: return "SCSI Bus Controller";
                    case 0x01: return "IDE Controller";
                    case 0x02: return "Floppy Disk Controller";
                    case 0x05: return "ATA Controller";
                    case 0x06: return "SATA Controller";
                    case 0x08: return "NVM Controller";
                    default:   return "Mass Storage Controller";
                }
            case 0x02:
                switch (subClass) {
                    case 0x00: return "Ethernet Controller";
                    case 0x80: return "Other Network Controller";
                    default:   return "Network Controller";
                }
            case 0x03:
                switch (subClass) {
                    case 0x00: return "VGA Compatible Controller";
                    case 0x01: return "XGA Controller";
                    case 0x02: return "3D Controller";
                    default:   return "Display Controller";
                }
            case 0x04:
                return "Multimedia Controller";
            case 0x05:
                return "Memory Controller";
            case 0x06:
                switch (subClass) {
                    case 0x00: return "Host Bridge";
                    case 0x01: return "ISA Bridge";
                    case 0x04: return "PCI-to-PCI Bridge";
                    case 0x80: return "Other Bridge";
                    default:   return "Bridge Device";
                }
            case 0x07:
                return "Simple Communication Controller";
            case 0x08:
                return "Base System Peripheral";
            case 0x09:
                return "Input Device Controller";
            case 0x0A:
                return "Docking Station";
            case 0x0B:
                return "Processor";
            case 0x0C:
                switch (subClass) {
                    case 0x03: return "USB Controller";
                    case 0x05: return "SMBus Controller";
                    default:   return "Serial Bus Controller";
                }
            case 0x0D:
                return "Wireless Controller";
            case 0x0E:
                return "Intelligent Controller";
            case 0x0F:
                return "Satellite Communication Controller";
            case 0x10:
                return "Encryption Controller";
            case 0x11:
                return "Signal Processing Controller";
            case 0xFF:
                return "Unassigned";
            default:
                return "Unknown";
        }
    }

    // -------------------------------------------------------------------------
    // Device enumeration
    // -------------------------------------------------------------------------

    static void EnumerateFunction(uint8_t bus, uint8_t device, uint8_t function) {
        uint16_t vendorId = ReadConfig16(bus, device, function, RegVendorId);
        if (vendorId == 0xFFFF) {
            return;
        }

        PciDevice dev{};
        dev.Segment = 0;
        dev.Bus = bus;
        dev.Device = device;
        dev.Function = function;
        dev.VendorId = vendorId;
        dev.DeviceId = ReadConfig16(bus, device, function, RegDeviceId);
        dev.ClassCode = ReadConfig8(bus, device, function, RegClassCode);
        dev.SubClass = ReadConfig8(bus, device, function, RegSubClass);
        dev.ProgIf = ReadConfig8(bus, device, function, RegProgIf);
        dev.RevisionId = ReadConfig8(bus, device, function, RegRevisionId);
        dev.HeaderType = ReadConfig8(bus, device, function, RegHeaderType);

        g_devices.push_back(dev);

        KernelLogStream(DEBUG, "PCI") << base::hex
            << (uint64_t)bus << ":" << (uint64_t)device << "." << (uint64_t)function
            << " " << (uint64_t)vendorId << ":" << (uint64_t)dev.DeviceId
            << " " << GetClassName(dev.ClassCode, dev.SubClass)
            << " (class " << (uint64_t)dev.ClassCode << "." << (uint64_t)dev.SubClass << ")";
    }

    static void EnumerateDevice(uint8_t bus, uint8_t device) {
        uint16_t vendorId = ReadConfig16(bus, device, 0, RegVendorId);
        if (vendorId == 0xFFFF) {
            return;
        }

        // Always check function 0
        EnumerateFunction(bus, device, 0);

        // Check if multi-function device (bit 7 of header type)
        uint8_t headerType = ReadConfig8(bus, device, 0, RegHeaderType);
        if (headerType & 0x80) {
            for (uint8_t func = 1; func < 8; func++) {
                EnumerateFunction(bus, device, func);
            }
        }
    }

    static void EnumerateBus(uint8_t bus) {
        for (uint8_t device = 0; device < 32; device++) {
            EnumerateDevice(bus, device);
        }
    }

    static void EnumerateAll() {
        uint8_t startBus = g_useEcam ? g_ecamStartBus : 0;
        uint8_t endBus = g_useEcam ? g_ecamEndBus : 255;

        for (uint32_t bus = startBus; bus <= (uint32_t)endBus; bus++) {
            EnumerateBus((uint8_t)bus);
        }
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    kcp::vector<PciDevice>& GetDevices() {
        return g_devices;
    }

    void Initialize(Hal::ACPI::CommonSDTHeader* xsdt) {
        KernelLogStream(INFO, "PCI") << "Initializing PCI subsystem";

        // Try to parse MCFG for ECAM access; fall back to legacy if unavailable
        ParseMcfg(xsdt);

        if (g_useEcam) {
            KernelLogStream(OK, "PCI") << "Using ECAM (memory-mapped) config access";
        } else {
            KernelLogStream(INFO, "PCI") << "Using legacy I/O port config access";
        }

        // Enumerate all PCI devices
        EnumerateAll();

        KernelLogStream(OK, "PCI") << "Enumeration complete: " << base::dec << (uint64_t)g_devices.size() << " devices found";
    }
};
