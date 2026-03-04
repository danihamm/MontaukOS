/*
    * E1000.cpp
    * Intel 82540EM (E1000) Ethernet driver
    * Copyright (c) 2025 Daniel Hammer
*/

#include "E1000.hpp"
#include <Pci/Pci.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Libraries/Memory.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Hal/Apic/IoApic.hpp>

using namespace Kt;

namespace Drivers::Net::E1000 {

    // PCI vendor/device IDs for the Intel 82540EM
    static constexpr uint16_t VendorIntel = 0x8086;
    static constexpr uint16_t DeviceE1000 = 0x100E;

    // Driver state
    static bool g_initialized = false;
    static volatile uint8_t* g_mmioBase = nullptr;
    static uint8_t g_macAddress[6] = {};
    static uint8_t g_irqLine = 0;

    // Descriptor rings (physical addresses for DMA, virtual for CPU access)
    static RxDescriptor* g_rxDescs = nullptr;
    static TxDescriptor* g_txDescs = nullptr;
    static uint64_t g_rxDescsPhys = 0;
    static uint64_t g_txDescsPhys = 0;

    // Packet buffers (virtual addresses)
    static uint8_t* g_rxBuffers[RX_DESC_COUNT] = {};
    static uint8_t* g_txBuffers[TX_DESC_COUNT] = {};
    static uint64_t g_rxBuffersPhys[RX_DESC_COUNT] = {};
    static uint64_t g_txBuffersPhys[TX_DESC_COUNT] = {};

    // Current descriptor indices
    static uint32_t g_rxTail = 0;
    static uint32_t g_txTail = 0;

    // Statistics
    static uint64_t g_rxPacketCount = 0;
    static uint64_t g_txPacketCount = 0;

    // RX callback
    static RxCallback g_rxCallback = nullptr;

    // -------------------------------------------------------------------------
    // Register access helpers
    // -------------------------------------------------------------------------

    static void WriteReg(uint32_t reg, uint32_t value) {
        *(volatile uint32_t*)(g_mmioBase + reg) = value;
    }

    static uint32_t ReadReg(uint32_t reg) {
        return *(volatile uint32_t*)(g_mmioBase + reg);
    }

    // -------------------------------------------------------------------------
    // EEPROM access (fallback for MAC address)
    // -------------------------------------------------------------------------

    static uint16_t EepromRead(uint8_t address) {
        // Write the address and start bit to EERD
        WriteReg(REG_EERD, ((uint32_t)address << 8) | 1);

        // Poll for completion (bit 4 = done)
        uint32_t value;
        for (int i = 0; i < 10000; i++) {
            value = ReadReg(REG_EERD);
            if (value & (1 << 4)) {
                return (uint16_t)(value >> 16);
            }
        }

        KernelLogStream(WARNING, "E1000") << "EEPROM read timeout for address " << base::hex << (uint64_t)address;
        return 0;
    }

    // -------------------------------------------------------------------------
    // MAC address
    // -------------------------------------------------------------------------

    static void ReadMacAddress() {
        // Try reading from RAL/RAH first (QEMU usually has it here)
        uint32_t ral = ReadReg(REG_RAL);
        uint32_t rah = ReadReg(REG_RAH);

        if (ral != 0) {
            g_macAddress[0] = (uint8_t)(ral);
            g_macAddress[1] = (uint8_t)(ral >> 8);
            g_macAddress[2] = (uint8_t)(ral >> 16);
            g_macAddress[3] = (uint8_t)(ral >> 24);
            g_macAddress[4] = (uint8_t)(rah);
            g_macAddress[5] = (uint8_t)(rah >> 8);
        } else {
            // Fallback: read from EEPROM
            uint16_t word0 = EepromRead(0);
            uint16_t word1 = EepromRead(1);
            uint16_t word2 = EepromRead(2);

            g_macAddress[0] = (uint8_t)(word0);
            g_macAddress[1] = (uint8_t)(word0 >> 8);
            g_macAddress[2] = (uint8_t)(word1);
            g_macAddress[3] = (uint8_t)(word1 >> 8);
            g_macAddress[4] = (uint8_t)(word2);
            g_macAddress[5] = (uint8_t)(word2 >> 8);
        }

        // Write MAC back to RAL/RAH to ensure the filter is set
        WriteReg(REG_RAL,
            (uint32_t)g_macAddress[0] |
            ((uint32_t)g_macAddress[1] << 8) |
            ((uint32_t)g_macAddress[2] << 16) |
            ((uint32_t)g_macAddress[3] << 24));
        WriteReg(REG_RAH,
            (uint32_t)g_macAddress[4] |
            ((uint32_t)g_macAddress[5] << 8) |
            (1u << 31)); // AV (Address Valid) bit
    }

    // -------------------------------------------------------------------------
    // Allocate page-aligned DMA buffer, returns virtual address
    // -------------------------------------------------------------------------

    static uint8_t* AllocateDmaBuffer(uint64_t& outPhysAddr) {
        void* virt = Memory::g_pfa->AllocateZeroed();
        outPhysAddr = Memory::SubHHDM(virt);
        return (uint8_t*)virt;
    }

    // -------------------------------------------------------------------------
    // RX setup
    // -------------------------------------------------------------------------

    static void SetupRx() {
        // Allocate RX descriptor ring (needs to be 128-byte aligned, page-aligned is fine)
        uint64_t descPhys;
        g_rxDescs = (RxDescriptor*)AllocateDmaBuffer(descPhys);
        g_rxDescsPhys = descPhys;

        // Allocate packet buffers for each descriptor
        for (uint32_t i = 0; i < RX_DESC_COUNT; i++) {
            // Each buffer is one page (4096 bytes), sufficient for standard Ethernet frames
            g_rxBuffers[i] = AllocateDmaBuffer(g_rxBuffersPhys[i]);

            // For larger buffers (8192), allocate a second page
            uint64_t secondPhys;
            AllocateDmaBuffer(secondPhys);

            g_rxDescs[i].BufferAddress = g_rxBuffersPhys[i];
            g_rxDescs[i].Status = 0;
            g_rxDescs[i].Length = 0;
            g_rxDescs[i].Checksum = 0;
            g_rxDescs[i].Errors = 0;
            g_rxDescs[i].Special = 0;
        }

        // Program the descriptor ring base address
        WriteReg(REG_RDBAL, (uint32_t)(g_rxDescsPhys & 0xFFFFFFFF));
        WriteReg(REG_RDBAH, (uint32_t)(g_rxDescsPhys >> 32));

        // Set descriptor ring length (in bytes)
        WriteReg(REG_RDLEN, RX_DESC_COUNT * sizeof(RxDescriptor));

        // Set head and tail pointers
        WriteReg(REG_RDH, 0);
        WriteReg(REG_RDT, RX_DESC_COUNT - 1);

        g_rxTail = RX_DESC_COUNT - 1;

        // Configure RCTL: enable receiver, accept broadcast, strip CRC, 4096 byte buffers
        uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_4096 | RCTL_BSEX;
        WriteReg(REG_RCTL, rctl);

        KernelLogStream(OK, "E1000") << "RX ring configured: " << base::dec << (uint64_t)RX_DESC_COUNT << " descriptors";
    }

    // -------------------------------------------------------------------------
    // TX setup
    // -------------------------------------------------------------------------

    static void SetupTx() {
        // Allocate TX descriptor ring
        uint64_t descPhys;
        g_txDescs = (TxDescriptor*)AllocateDmaBuffer(descPhys);
        g_txDescsPhys = descPhys;

        // Allocate packet buffers for each descriptor
        for (uint32_t i = 0; i < TX_DESC_COUNT; i++) {
            g_txBuffers[i] = AllocateDmaBuffer(g_txBuffersPhys[i]);

            g_txDescs[i].BufferAddress = g_txBuffersPhys[i];
            g_txDescs[i].Length = 0;
            g_txDescs[i].Command = 0;
            g_txDescs[i].Status = TXSTA_DD; // Mark as done (available for use)
            g_txDescs[i].ChecksumOffset = 0;
            g_txDescs[i].ChecksumStart = 0;
            g_txDescs[i].Special = 0;
        }

        // Program the descriptor ring base address
        WriteReg(REG_TDBAL, (uint32_t)(g_txDescsPhys & 0xFFFFFFFF));
        WriteReg(REG_TDBAH, (uint32_t)(g_txDescsPhys >> 32));

        // Set descriptor ring length (in bytes)
        WriteReg(REG_TDLEN, TX_DESC_COUNT * sizeof(TxDescriptor));

        // Set head and tail pointers
        WriteReg(REG_TDH, 0);
        WriteReg(REG_TDT, 0);

        g_txTail = 0;

        // Configure TCTL: enable transmitter, pad short packets
        // Collision Threshold = 15, Collision Distance = 64
        uint32_t tctl = TCTL_EN | TCTL_PSP
                      | (15u << TCTL_CT_SHIFT)
                      | (64u << TCTL_COLD_SHIFT);
        WriteReg(REG_TCTL, tctl);

        // Set Inter Packet Gap (recommended values for IEEE 802.3)
        // IPGT=10, IPGR1=10, IPGR2=10
        WriteReg(REG_TIPG, 10 | (10 << 10) | (10 << 20));

        KernelLogStream(OK, "E1000") << "TX ring configured: " << base::dec << (uint64_t)TX_DESC_COUNT << " descriptors";
    }

    // -------------------------------------------------------------------------
    // Interrupt handler
    // -------------------------------------------------------------------------

    static void HandleInterrupt(uint8_t irq) {
        (void)irq;

        // Read and clear interrupt cause
        uint32_t icr = ReadReg(REG_ICR);

        if (icr & ICR_LSC) {
            uint32_t status = ReadReg(REG_STATUS);
            bool linkUp = (status & (1 << 1)) != 0;
            KernelLogStream(INFO, "E1000") << "Link status change: " << (linkUp ? "UP" : "DOWN");
        }

        if (icr & ICR_RXT0) {
            // Process received packets
            while (true) {
                uint32_t nextIdx = (g_rxTail + 1) % RX_DESC_COUNT;
                RxDescriptor& desc = g_rxDescs[nextIdx];

                if (!(desc.Status & RXSTA_DD)) {
                    break; // No more packets
                }

                uint16_t length = desc.Length;
                g_rxPacketCount++;

                // Dispatch to the network stack callback
                if (g_rxCallback != nullptr) {
                    g_rxCallback(g_rxBuffers[nextIdx], length);
                }

                // Reset descriptor for reuse
                desc.Status = 0;
                desc.Length = 0;
                desc.Errors = 0;

                g_rxTail = nextIdx;
                WriteReg(REG_RDT, g_rxTail);
            }
        }

        if (icr & (ICR_TXDW | ICR_TXQE)) {
            // TX completion - nothing to do for now
        }
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // Probe (called by PCI driver matching framework)
    // -------------------------------------------------------------------------

    bool Probe(const Pci::PciDevice& dev) {
        if (g_initialized) return false;

        KernelLogStream(OK, "E1000") << "Found E1000 at PCI "
            << base::hex << (uint64_t)dev.Bus << ":"
            << (uint64_t)dev.Device << "." << (uint64_t)dev.Function;

        // Read BAR0 (MMIO base address)
        uint64_t mmioPhys = Pci::ReadBar0(dev.Bus, dev.Device, dev.Function);
        KernelLogStream(INFO, "E1000") << "BAR0 physical: " << base::hex << mmioPhys;

        // Map the MMIO region (128KB = 32 pages)
        constexpr uint64_t MmioSize = 0x20000;
        for (uint64_t offset = 0; offset < MmioSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }
        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        // Enable bus mastering and memory space
        Pci::EnableBusMaster(dev.Bus, dev.Device, dev.Function);
        KernelLogStream(OK, "E1000") << "Bus mastering enabled";

        // Read interrupt line from PCI config
        g_irqLine = Pci::LegacyRead8(dev.Bus, dev.Device, dev.Function, (uint8_t)Pci::PCI_REG_INTERRUPT);
        KernelLogStream(INFO, "E1000") << "IRQ line: " << base::dec << (uint64_t)g_irqLine;

        // Reset the device
        uint32_t ctrl = ReadReg(REG_CTRL);
        WriteReg(REG_CTRL, ctrl | CTRL_RST);
        for (int i = 0; i < 100000; i++) {
            if (!(ReadReg(REG_CTRL) & CTRL_RST)) break;
        }

        WriteReg(REG_IMC, 0xFFFFFFFF);

        ctrl = ReadReg(REG_CTRL);
        ctrl |= CTRL_SLU;
        ctrl &= ~(1u << 3);
        ctrl &= ~(1u << 31);
        ctrl &= ~(1u << 7);
        WriteReg(REG_CTRL, ctrl);

        ReadMacAddress();
        KernelLogStream(OK, "E1000") << "MAC: " << base::hex
            << (uint64_t)g_macAddress[0] << ":" << (uint64_t)g_macAddress[1] << ":"
            << (uint64_t)g_macAddress[2] << ":" << (uint64_t)g_macAddress[3] << ":"
            << (uint64_t)g_macAddress[4] << ":" << (uint64_t)g_macAddress[5];

        for (uint32_t i = 0; i < 128; i++) {
            WriteReg(REG_MTA + (i * 4), 0);
        }

        SetupRx();
        SetupTx();

        Hal::RegisterIrqHandler(g_irqLine, HandleInterrupt);
        Hal::IoApic::UnmaskIrq(Hal::IoApic::GetGsiForIrq(g_irqLine));
        WriteReg(REG_IMS, ICR_RXT0 | ICR_TXDW | ICR_TXQE | ICR_LSC | ICR_RXDMT0);

        g_initialized = true;

        uint32_t status = ReadReg(REG_STATUS);
        bool linkUp = (status & (1 << 1)) != 0;
        KernelLogStream(OK, "E1000") << "Initialization complete, link: " << (linkUp ? "UP" : "DOWN");
        return true;
    }

    // -------------------------------------------------------------------------
    // Initialize (standalone fallback path)
    // -------------------------------------------------------------------------

    void Initialize() {
        KernelLogStream(INFO, "E1000") << "Scanning for Intel E1000 NIC...";

        // Find the E1000 in the PCI device list
        auto& devices = Pci::GetDevices();
        const Pci::PciDevice* e1000Dev = nullptr;

        for (uint64_t i = 0; i < devices.size(); i++) {
            if (devices[i].VendorId == VendorIntel && devices[i].DeviceId == DeviceE1000) {
                e1000Dev = &devices[i];
                break;
            }
        }

        if (e1000Dev == nullptr) {
            KernelLogStream(WARNING, "E1000") << "No Intel E1000 NIC found";
            return;
        }

        KernelLogStream(OK, "E1000") << "Found E1000 at PCI "
            << base::hex << (uint64_t)e1000Dev->Bus << ":"
            << (uint64_t)e1000Dev->Device << "." << (uint64_t)e1000Dev->Function;

        // Read BAR0 (MMIO base address)
        uint64_t mmioPhys = Pci::ReadBar0(e1000Dev->Bus, e1000Dev->Device, e1000Dev->Function);

        KernelLogStream(INFO, "E1000") << "BAR0 physical: " << base::hex << mmioPhys;

        // Map the MMIO region (128KB = 32 pages)
        constexpr uint64_t MmioSize = 0x20000; // 128KB
        for (uint64_t offset = 0; offset < MmioSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }

        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        // Enable bus mastering and memory space
        Pci::EnableBusMaster(e1000Dev->Bus, e1000Dev->Device, e1000Dev->Function);

        KernelLogStream(OK, "E1000") << "Bus mastering enabled";

        // Read interrupt line from PCI config
        g_irqLine = Pci::LegacyRead8(e1000Dev->Bus, e1000Dev->Device, e1000Dev->Function, (uint8_t)Pci::PCI_REG_INTERRUPT);
        KernelLogStream(INFO, "E1000") << "IRQ line: " << base::dec << (uint64_t)g_irqLine;

        // Reset the device
        uint32_t ctrl = ReadReg(REG_CTRL);
        WriteReg(REG_CTRL, ctrl | CTRL_RST);

        // Wait for reset to complete (RST bit auto-clears)
        for (int i = 0; i < 100000; i++) {
            if (!(ReadReg(REG_CTRL) & CTRL_RST)) {
                break;
            }
        }

        // Disable all interrupts during setup
        WriteReg(REG_IMC, 0xFFFFFFFF);

        // Set link up
        ctrl = ReadReg(REG_CTRL);
        ctrl |= CTRL_SLU;
        ctrl &= ~(1u << 3); // Clear LRST
        ctrl &= ~(1u << 31); // Clear PHY_RST
        ctrl &= ~(1u << 7); // Clear ILOS
        WriteReg(REG_CTRL, ctrl);

        // Read MAC address
        ReadMacAddress();

        KernelLogStream(OK, "E1000") << "MAC: "
            << base::hex
            << (uint64_t)g_macAddress[0] << ":"
            << (uint64_t)g_macAddress[1] << ":"
            << (uint64_t)g_macAddress[2] << ":"
            << (uint64_t)g_macAddress[3] << ":"
            << (uint64_t)g_macAddress[4] << ":"
            << (uint64_t)g_macAddress[5];

        // Zero out the Multicast Table Array (128 entries)
        for (uint32_t i = 0; i < 128; i++) {
            WriteReg(REG_MTA + (i * 4), 0);
        }

        // Set up RX and TX descriptor rings
        SetupRx();
        SetupTx();

        // Register interrupt handler
        Hal::RegisterIrqHandler(g_irqLine, HandleInterrupt);
        Hal::IoApic::UnmaskIrq(Hal::IoApic::GetGsiForIrq(g_irqLine));

        // Enable interrupts: RX, TX, Link Status Change
        WriteReg(REG_IMS, ICR_RXT0 | ICR_TXDW | ICR_TXQE | ICR_LSC | ICR_RXDMT0);

        g_initialized = true;

        // Report link status
        uint32_t status = ReadReg(REG_STATUS);
        bool linkUp = (status & (1 << 1)) != 0;
        KernelLogStream(OK, "E1000") << "Initialization complete, link: " << (linkUp ? "UP" : "DOWN");
    }

    bool SendPacket(const uint8_t* data, uint16_t length) {
        if (!g_initialized || data == nullptr || length == 0 || length > 1518) {
            return false;
        }

        // Check if the current TX descriptor is available
        TxDescriptor& desc = g_txDescs[g_txTail];
        if (!(desc.Status & TXSTA_DD)) {
            KernelLogStream(WARNING, "E1000") << "TX ring full";
            return false;
        }

        // Copy packet data into the TX buffer
        memcpy(g_txBuffers[g_txTail], data, length);

        // Set up the descriptor
        desc.BufferAddress = g_txBuffersPhys[g_txTail];
        desc.Length = length;
        desc.Command = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
        desc.Status = 0;

        // Advance the tail pointer (tells the NIC there's a new packet)
        g_txTail = (g_txTail + 1) % TX_DESC_COUNT;
        WriteReg(REG_TDT, g_txTail);

        g_txPacketCount++;
        return true;
    }

    const uint8_t* GetMacAddress() {
        return g_macAddress;
    }

    bool IsInitialized() {
        return g_initialized;
    }

    void SetRxCallback(RxCallback callback) {
        g_rxCallback = callback;
    }

};