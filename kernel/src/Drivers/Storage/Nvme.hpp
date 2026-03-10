/*
    * Nvme.hpp
    * NVM Express (NVMe) storage driver
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Pci/Pci.hpp>

namespace Drivers::Storage::Nvme {

    // =========================================================================
    // NVMe controller registers (memory-mapped via BAR0)
    // Ref: NVM Express Base Specification 2.0
    // =========================================================================

    constexpr uint32_t REG_CAP       = 0x00;  // Controller Capabilities (64-bit)
    constexpr uint32_t REG_VS        = 0x08;  // Version
    constexpr uint32_t REG_INTMS     = 0x0C;  // Interrupt Mask Set
    constexpr uint32_t REG_INTMC     = 0x10;  // Interrupt Mask Clear
    constexpr uint32_t REG_CC        = 0x14;  // Controller Configuration
    constexpr uint32_t REG_CSTS      = 0x1C;  // Controller Status
    constexpr uint32_t REG_AQA       = 0x24;  // Admin Queue Attributes
    constexpr uint32_t REG_ASQ       = 0x28;  // Admin SQ Base Address (64-bit)
    constexpr uint32_t REG_ACQ       = 0x30;  // Admin CQ Base Address (64-bit)

    // CAP register fields (64-bit)
    constexpr uint64_t CAP_MQES_MASK = 0xFFFF;           // Max Queue Entries Supported (0-based)
    constexpr int      CAP_DSTRD_SHIFT = 32;             // Doorbell Stride (2 ^ (2 + DSTRD))
    constexpr uint64_t CAP_DSTRD_MASK = 0xFULL << 32;
    constexpr int      CAP_MPSMIN_SHIFT = 48;            // Memory Page Size Minimum
    constexpr uint64_t CAP_MPSMIN_MASK = 0xFULL << 48;
    constexpr int      CAP_MPSMAX_SHIFT = 52;            // Memory Page Size Maximum
    constexpr uint64_t CAP_MPSMAX_MASK = 0xFULL << 52;
    constexpr uint64_t CAP_CSS_NVM  = (1ULL << 37);      // NVM Command Set supported

    // CC register fields
    constexpr uint32_t CC_EN         = (1u << 0);   // Enable
    constexpr uint32_t CC_CSS_NVM    = (0u << 4);   // NVM Command Set
    constexpr uint32_t CC_MPS_SHIFT  = 7;            // Memory Page Size (2 ^ (12 + MPS))
    constexpr uint32_t CC_AMS_RR     = (0u << 11);  // Arbitration: Round Robin
    constexpr uint32_t CC_SHN_NONE   = (0u << 14);  // No shutdown notification
    constexpr uint32_t CC_SHN_NORMAL = (1u << 14);  // Normal shutdown
    constexpr uint32_t CC_IOSQES_SHIFT = 16;         // I/O SQ Entry Size (2^n)
    constexpr uint32_t CC_IOCQES_SHIFT = 20;         // I/O CQ Entry Size (2^n)

    // CSTS register fields
    constexpr uint32_t CSTS_RDY      = (1u << 0);   // Ready
    constexpr uint32_t CSTS_CFS      = (1u << 1);   // Controller Fatal Status
    constexpr uint32_t CSTS_SHST_MASK = (3u << 2);  // Shutdown Status
    constexpr uint32_t CSTS_SHST_NORMAL = (0u << 2);
    constexpr uint32_t CSTS_SHST_COMPLETE = (2u << 2);

    // =========================================================================
    // Submission Queue Entry (64 bytes)
    // =========================================================================

    struct SqEntry {
        // Dword 0: Command Dword 0
        uint8_t  Opcode;
        uint8_t  Flags;         // Fused (1:0), PSDT (7:6)
        uint16_t CommandId;

        // Dword 1
        uint32_t Nsid;          // Namespace ID

        // Dwords 2-3
        uint64_t Reserved;

        // Dwords 4-5: Metadata Pointer
        uint64_t Mptr;

        // Dwords 6-9: Data Pointer (PRP1, PRP2)
        uint64_t Prp1;
        uint64_t Prp2;

        // Dwords 10-15: Command specific
        uint32_t Cdw10;
        uint32_t Cdw11;
        uint32_t Cdw12;
        uint32_t Cdw13;
        uint32_t Cdw14;
        uint32_t Cdw15;
    } __attribute__((packed));

    static_assert(sizeof(SqEntry) == 64, "SqEntry must be 64 bytes");

    // =========================================================================
    // Completion Queue Entry (16 bytes)
    // =========================================================================

    struct CqEntry {
        uint32_t Result;        // Command-specific result (DW0)
        uint32_t Reserved;
        uint16_t SqHead;        // SQ Head Pointer
        uint16_t SqId;          // SQ Identifier
        uint16_t CommandId;     // Command Identifier
        uint16_t Status;        // Status Field (bit 0 = Phase Tag)
    } __attribute__((packed));

    static_assert(sizeof(CqEntry) == 16, "CqEntry must be 16 bytes");

    // Status field: Phase bit is bit 0; status code is bits 15:1
    constexpr uint16_t CQE_PHASE_BIT = (1u << 0);
    constexpr uint16_t CQE_STATUS_MASK = 0xFFFE;  // bits 15:1

    // =========================================================================
    // Admin opcodes
    // =========================================================================

    constexpr uint8_t ADMIN_DELETE_IO_SQ = 0x00;
    constexpr uint8_t ADMIN_CREATE_IO_SQ = 0x01;
    constexpr uint8_t ADMIN_DELETE_IO_CQ = 0x04;
    constexpr uint8_t ADMIN_CREATE_IO_CQ = 0x05;
    constexpr uint8_t ADMIN_IDENTIFY     = 0x06;
    constexpr uint8_t ADMIN_SET_FEATURES = 0x09;

    // =========================================================================
    // NVM I/O opcodes
    // =========================================================================

    constexpr uint8_t IO_CMD_READ  = 0x02;
    constexpr uint8_t IO_CMD_WRITE = 0x01;

    // =========================================================================
    // Identify CNS values
    // =========================================================================

    constexpr uint32_t IDENTIFY_CNS_NAMESPACE  = 0x00;
    constexpr uint32_t IDENTIFY_CNS_CONTROLLER = 0x01;

    // =========================================================================
    // Feature identifiers
    // =========================================================================

    constexpr uint32_t FEATURE_NUM_QUEUES = 0x07;

    // =========================================================================
    // Queue parameters
    // =========================================================================

    constexpr int ADMIN_QUEUE_DEPTH = 32;    // Admin queue entries
    constexpr int IO_QUEUE_DEPTH    = 64;    // I/O queue entries
    constexpr int MAX_NAMESPACES    = 8;

    // =========================================================================
    // Namespace info
    // =========================================================================

    struct NamespaceInfo {
        bool     Active;
        uint32_t Nsid;
        uint64_t SectorCount;       // Total LBAs (NSZE)
        uint32_t SectorSize;        // Bytes per LBA
        uint32_t MaxTransferBlocks;  // MDTS in blocks
        char     Model[41];
    };

    // =========================================================================
    // MSI configuration (use a different IRQ slot than AHCI)
    // =========================================================================

    constexpr uint8_t  MSI_IRQ       = 26;    // IRQ slot 26 = vector 58
    constexpr uint32_t MSI_VECTOR    = 58;
    constexpr uint32_t MSI_ADDR_BASE = 0xFEE00000;

    // =========================================================================
    // Public API
    // =========================================================================

    // Probe a PCI device (called by driver matching framework)
    bool Probe(const Pci::PciDevice& dev);

    // Check if the driver was initialized
    bool IsInitialized();

    // Get number of active namespaces
    int GetNamespaceCount();

    // Read sectors from an NVMe namespace
    // ns: namespace index, lba: starting LBA, count: sector count (max 128)
    // buffer: destination buffer
    // Returns true on success
    bool ReadSectors(int ns, uint64_t lba, uint32_t count, void* buffer);

    // Write sectors to an NVMe namespace
    bool WriteSectors(int ns, uint64_t lba, uint32_t count, const void* buffer);

    // Get info about a specific namespace
    const NamespaceInfo* GetNamespaceInfo(int ns);

};
