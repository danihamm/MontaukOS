/*
    * E1000.hpp
    * Intel 82540EM (E1000) Ethernet driver
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Pci/Pci.hpp>

namespace Drivers::Net::E1000 {

    // E1000 register offsets (memory-mapped via BAR0)
    constexpr uint32_t REG_CTRL   = 0x0000;  // Device Control
    constexpr uint32_t REG_STATUS = 0x0008;  // Device Status
    constexpr uint32_t REG_EERD   = 0x0014;  // EEPROM Read
    constexpr uint32_t REG_ICR    = 0x00C0;  // Interrupt Cause Read
    constexpr uint32_t REG_IMS    = 0x00D0;  // Interrupt Mask Set
    constexpr uint32_t REG_IMC    = 0x00D8;  // Interrupt Mask Clear
    constexpr uint32_t REG_RCTL   = 0x0100;  // Receive Control
    constexpr uint32_t REG_TCTL   = 0x0400;  // Transmit Control
    constexpr uint32_t REG_TIPG   = 0x0410;  // Transmit IPG
    constexpr uint32_t REG_RDBAL  = 0x2800;  // RX Descriptor Base Low
    constexpr uint32_t REG_RDBAH  = 0x2804;  // RX Descriptor Base High
    constexpr uint32_t REG_RDLEN  = 0x2808;  // RX Descriptor Length
    constexpr uint32_t REG_RDH    = 0x2810;  // RX Descriptor Head
    constexpr uint32_t REG_RDT    = 0x2818;  // RX Descriptor Tail
    constexpr uint32_t REG_TDBAL  = 0x3800;  // TX Descriptor Base Low
    constexpr uint32_t REG_TDBAH  = 0x3804;  // TX Descriptor Base High
    constexpr uint32_t REG_TDLEN  = 0x3808;  // TX Descriptor Length
    constexpr uint32_t REG_TDH    = 0x3810;  // TX Descriptor Head
    constexpr uint32_t REG_TDT    = 0x3818;  // TX Descriptor Tail
    constexpr uint32_t REG_MTA    = 0x5200;  // Multicast Table Array (128 entries)
    constexpr uint32_t REG_RAL    = 0x5400;  // Receive Address Low
    constexpr uint32_t REG_RAH    = 0x5404;  // Receive Address High

    // CTRL register bits
    constexpr uint32_t CTRL_SLU   = (1 << 6);   // Set Link Up
    constexpr uint32_t CTRL_RST   = (1 << 26);  // Device Reset

    // RCTL register bits
    constexpr uint32_t RCTL_EN    = (1 << 1);   // Receiver Enable
    constexpr uint32_t RCTL_SBP   = (1 << 2);   // Store Bad Packets
    constexpr uint32_t RCTL_UPE   = (1 << 3);   // Unicast Promiscuous
    constexpr uint32_t RCTL_MPE   = (1 << 4);   // Multicast Promiscuous
    constexpr uint32_t RCTL_BAM   = (1 << 15);  // Broadcast Accept Mode
    constexpr uint32_t RCTL_BSIZE_4096 = (3 << 16); // Buffer Size 4096 (with BSEX)
    constexpr uint32_t RCTL_BSEX  = (1 << 25);  // Buffer Size Extension
    constexpr uint32_t RCTL_SECRC = (1 << 26);  // Strip Ethernet CRC

    // TCTL register bits
    constexpr uint32_t TCTL_EN    = (1 << 1);   // Transmit Enable
    constexpr uint32_t TCTL_PSP   = (1 << 3);   // Pad Short Packets
    constexpr uint32_t TCTL_CT_SHIFT  = 4;      // Collision Threshold shift
    constexpr uint32_t TCTL_COLD_SHIFT = 12;    // Collision Distance shift

    // ICR (interrupt cause) bits
    constexpr uint32_t ICR_TXDW   = (1 << 0);   // TX Descriptor Written Back
    constexpr uint32_t ICR_TXQE   = (1 << 1);   // TX Queue Empty
    constexpr uint32_t ICR_LSC    = (1 << 2);   // Link Status Change
    constexpr uint32_t ICR_RXDMT0 = (1 << 4);   // RX Descriptor Minimum Threshold
    constexpr uint32_t ICR_RXO    = (1 << 6);   // Receiver Overrun
    constexpr uint32_t ICR_RXT0   = (1 << 7);   // Receiver Timer Interrupt

    // TX descriptor command bits
    constexpr uint8_t TXCMD_EOP   = (1 << 0);   // End Of Packet
    constexpr uint8_t TXCMD_IFCS  = (1 << 1);   // Insert FCS/CRC
    constexpr uint8_t TXCMD_RS    = (1 << 3);   // Report Status

    // TX descriptor status bits
    constexpr uint8_t TXSTA_DD    = (1 << 0);   // Descriptor Done

    // RX descriptor status bits
    constexpr uint8_t RXSTA_DD    = (1 << 0);   // Descriptor Done
    constexpr uint8_t RXSTA_EOP   = (1 << 1);   // End Of Packet

    // Descriptor ring sizes
    constexpr uint32_t RX_DESC_COUNT = 32;
    constexpr uint32_t TX_DESC_COUNT = 32;
    constexpr uint32_t PACKET_BUFFER_SIZE = 8192;

    // RX descriptor (legacy format, 16 bytes)
    struct RxDescriptor {
        uint64_t BufferAddress;
        uint16_t Length;
        uint16_t Checksum;
        uint8_t  Status;
        uint8_t  Errors;
        uint16_t Special;
    } __attribute__((packed));

    // TX descriptor (legacy format, 16 bytes)
    struct TxDescriptor {
        uint64_t BufferAddress;
        uint16_t Length;
        uint8_t  ChecksumOffset;
        uint8_t  Command;
        uint8_t  Status;
        uint8_t  ChecksumStart;
        uint16_t Special;
    } __attribute__((packed));

    // Initialize the E1000 driver (scans PCI for the device)
    void Initialize();

    // Probe a specific PCI device (called by PCI driver matching framework)
    bool Probe(const Pci::PciDevice& dev);

    // Send a raw Ethernet frame
    bool SendPacket(const uint8_t* data, uint16_t length);

    // Get the MAC address (6 bytes)
    const uint8_t* GetMacAddress();

    // Check if the device was found and initialized
    bool IsInitialized();

    // RX callback type: called with (packet data, length)
    using RxCallback = void(*)(const uint8_t* data, uint16_t length);

    // Register a callback for received packets
    void SetRxCallback(RxCallback callback);

};