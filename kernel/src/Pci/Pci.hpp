/*
    * Pci.hpp
    * PCI Express enumeration and configuration space access
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <ACPI/ACPI.hpp>
#include <CppLib/Vector.hpp>

namespace Pci {

    // MCFG table structures
    struct McfgEntry {
        uint64_t BaseAddress;
        uint16_t SegmentGroup;
        uint8_t StartBus;
        uint8_t EndBus;
        uint32_t Reserved;
    }__attribute__((packed));

    struct McfgHeader {
        Hal::ACPI::CommonSDTHeader SDTHeader;
        uint64_t Reserved;
    }__attribute__((packed));

    // PCI device information
    struct PciDevice {
        uint16_t Segment;
        uint8_t Bus;
        uint8_t Device;
        uint8_t Function;

        uint16_t VendorId;
        uint16_t DeviceId;

        uint8_t ClassCode;
        uint8_t SubClass;
        uint8_t ProgIf;
        uint8_t RevisionId;
        uint8_t HeaderType;
    };

    // Configuration space access (ECAM / memory-mapped)
    uint8_t EcamRead8(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
    uint16_t EcamRead16(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
    uint32_t EcamRead32(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

    void EcamWrite8(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value);
    void EcamWrite16(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value);
    void EcamWrite32(uint64_t ecamBase, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value);

    // Configuration space access (legacy I/O ports 0xCF8/0xCFC)
    uint8_t LegacyRead8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    uint16_t LegacyRead16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    uint32_t LegacyRead32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

    void LegacyWrite8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value);
    void LegacyWrite16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
    void LegacyWrite32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

    // PCI capability IDs
    constexpr uint8_t PCI_CAP_MSI = 0x05;

    // Walk the PCI capability linked list for a given device.
    // Returns the config-space offset of the capability, or 0 if not found.
    uint8_t FindCapability(uint8_t bus, uint8_t device, uint8_t function, uint8_t capId);

    // Class code name lookup
    const char* GetClassName(uint8_t classCode, uint8_t subClass);

    // Get the list of discovered devices
    kcp::vector<PciDevice>& GetDevices();

    // Initialize PCI subsystem: parse MCFG, enumerate devices
    // xsdt: pointer to the XSDT (already HHDM-mapped)
    void Initialize(Hal::ACPI::CommonSDTHeader* xsdt);

    // =========================================================================
    // Shared PCI constants
    // =========================================================================

    constexpr uint16_t PCI_REG_COMMAND      = 0x04;
    constexpr uint16_t PCI_REG_BAR0         = 0x10;
    constexpr uint16_t PCI_REG_BAR1         = 0x14;
    constexpr uint16_t PCI_REG_INTERRUPT    = 0x3C;

    constexpr uint16_t PCI_CMD_MEM_SPACE    = (1 << 1);
    constexpr uint16_t PCI_CMD_BUS_MASTER   = (1 << 2);
    constexpr uint16_t PCI_CMD_INTX_DISABLE = (1 << 10);

    // Read BAR0, handling 32/64-bit BARs. Returns physical base address.
    uint64_t ReadBar0(uint8_t bus, uint8_t device, uint8_t function);

    // Enable memory space access and bus mastering in PCI command register.
    void EnableBusMaster(uint8_t bus, uint8_t device, uint8_t function);

    // =========================================================================
    // PCI driver matching
    // =========================================================================

    enum class ProbePhase : uint8_t { Early = 0, Normal = 1 };

    using PciProbeFunc = bool(*)(const PciDevice& dev);

    struct PciDriverDesc {
        const char*     Name;
        uint16_t        VendorId;       // 0 = any
        uint8_t         ClassCode;      // 0xFF = any
        uint8_t         SubClass;       // 0xFF = any
        uint8_t         ProgIf;         // 0xFF = any
        const uint16_t* DeviceIds;      // optional device ID whitelist
        uint16_t        DeviceIdCount;  // 0 = skip device ID check
        ProbePhase      Phase;
        PciProbeFunc    Probe;
    };

    // Probe all PCI devices against a driver table for a given phase.
    // First driver whose Probe() returns true claims each device.
    void ProbeAll(const PciDriverDesc* table, uint16_t count, ProbePhase phase);
};
