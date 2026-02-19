/*
    * E1000E.cpp
    * Intel I217/I218/I219 (E1000E) Ethernet driver
    * Copyright (c) 2025 Daniel Hammer
*/

#include "E1000E.hpp"
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

namespace Drivers::Net::E1000E {

    // Supported device entry
    struct DeviceEntry {
        uint16_t DeviceId;
        const char* Name;
    };

    // PCI vendor ID for Intel
    static constexpr uint16_t VendorIntel = 0x8086;

    // Device ID table for I217/I218/I219 series
    static constexpr DeviceEntry g_deviceTable[] = {
        // I217
        { 0x153A, "I217-LM" },
        { 0x153B, "I217-V" },
        // I218
        { 0x155A, "I218-LM" },
        { 0x1559, "I218-V" },
        { 0x15A0, "I218-LM (2)" },
        { 0x15A1, "I218-V (2)" },
        { 0x15A2, "I218-LM (3)" },
        { 0x15A3, "I218-V (3)" },
        // I219-LM variants
        { 0x156F, "I219-LM" },
        { 0x15B7, "I219-LM (2)" },
        { 0x15BB, "I219-LM (3)" },
        { 0x15BD, "I219-LM (4)" },
        { 0x15DF, "I219-LM (5)" },
        { 0x15E1, "I219-LM (6)" },
        { 0x15E3, "I219-LM (7)" },
        { 0x15D7, "I219-LM (8)" },
        { 0x0D4C, "I219-LM (9)" },
        { 0x0D4E, "I219-LM (10)" },
        { 0x0D53, "I219-LM (11)" },
        { 0x0D55, "I219-LM (12)" },
        { 0x0DC5, "I219-LM (13)" },
        { 0x0DC7, "I219-LM (14)" },
        { 0x1A1C, "I219-LM (15)" },
        { 0x1A1E, "I219-LM (16)" },
        // I219-V variants
        { 0x1570, "I219-V" },
        { 0x15B8, "I219-V (2)" },
        { 0x15BC, "I219-V (3)" },
        { 0x15BE, "I219-V (4)" },
        { 0x15E0, "I219-V (5)" },
        { 0x15E2, "I219-V (6)" },
        { 0x15D6, "I219-V (7)" },
        { 0x15D8, "I219-V (8)" },
        { 0x0D4D, "I219-V (9)" },
        { 0x0D4F, "I219-V (10)" },
        { 0x0D54, "I219-V (11)" },
        { 0x0DC6, "I219-V (13)" },
        { 0x0DC8, "I219-V (14)" },
        { 0x1A1D, "I219-V (15)" },
        { 0x1A1F, "I219-V (16)" },
    };

    static constexpr uint32_t g_deviceTableSize = sizeof(g_deviceTable) / sizeof(g_deviceTable[0]);

    // PCI config space offsets
    static constexpr uint8_t PCI_REG_BAR0       = 0x10;
    static constexpr uint8_t PCI_REG_BAR1       = 0x14;
    static constexpr uint8_t PCI_REG_COMMAND     = 0x04;
    static constexpr uint8_t PCI_REG_INTERRUPT   = 0x3C;

    // PCI command register bits
    static constexpr uint16_t PCI_CMD_BUS_MASTER    = (1 << 2);
    static constexpr uint16_t PCI_CMD_MEM_SPACE     = (1 << 1);
    static constexpr uint16_t PCI_CMD_INTX_DISABLE  = (1 << 10);

    // MSI configuration
    static constexpr uint8_t  MSI_IRQ      = 24;       // IRQ slot 24 = vector 56 (first MSI slot)
    static constexpr uint32_t MSI_VECTOR   = 56;       // IRQ_VECTOR_BASE + MSI_IRQ
    static constexpr uint32_t MSI_ADDR_BASE = 0xFEE00000; // Local APIC message address range

    // Driver state
    static bool g_initialized = false;
    static bool g_pollingMode = false;
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
    // SW/FW semaphore (prevents conflicts with Intel Management Engine)
    // -------------------------------------------------------------------------

    static bool AcquireSwFwSync() {
        // Step 1: Acquire the software semaphore (SWSM.SMBI)
        bool gotSmbi = false;
        for (int i = 0; i < 2000; i++) {
            uint32_t swsm = ReadReg(REG_SWSM);
            if (!(swsm & SWSM_SMBI)) {
                WriteReg(REG_SWSM, swsm | SWSM_SMBI);
                if (ReadReg(REG_SWSM) & SWSM_SMBI) {
                    gotSmbi = true;
                    break;
                }
            }
        }

        if (!gotSmbi) {
            KernelLogStream(WARNING, "E1000E") << "Could not acquire SMBI, proceeding anyway";
            return false;
        }

        // Step 2: Acquire the SW/FW semaphore (EXTCNF_CTRL.SW_OWN)
        for (int i = 0; i < 2000; i++) {
            uint32_t extcnf = ReadReg(REG_EXTCNF_CTRL);
            if (!(extcnf & EXTCNF_CTRL_SWFLAG)) {
                WriteReg(REG_EXTCNF_CTRL, extcnf | EXTCNF_CTRL_SWFLAG);
                if (ReadReg(REG_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) {
                    return true;
                }
            }
        }

        // Failed to acquire SWFLAG — release SMBI
        uint32_t swsm = ReadReg(REG_SWSM);
        WriteReg(REG_SWSM, swsm & ~SWSM_SMBI);
        KernelLogStream(WARNING, "E1000E") << "Could not acquire SWFLAG, proceeding anyway";
        return false;
    }

    static void ReleaseSwFwSync() {
        uint32_t extcnf = ReadReg(REG_EXTCNF_CTRL);
        WriteReg(REG_EXTCNF_CTRL, extcnf & ~EXTCNF_CTRL_SWFLAG);

        uint32_t swsm = ReadReg(REG_SWSM);
        WriteReg(REG_SWSM, swsm & ~SWSM_SMBI);
    }

    // -------------------------------------------------------------------------
    // PHY access via MDIC register
    // -------------------------------------------------------------------------

    static uint16_t PhyRead(uint32_t phyReg) {
        // PHY address = 1 for internal PHY
        uint32_t mdic = ((phyReg & 0x1F) << MDIC_REG_SHIFT)
                       | (1u << MDIC_PHY_SHIFT)
                       | MDIC_OP_READ;

        WriteReg(REG_MDIC, mdic);

        for (int i = 0; i < 200000; i++) {
            mdic = ReadReg(REG_MDIC);
            if (mdic & MDIC_READY) {
                if (mdic & MDIC_ERROR) {
                    KernelLogStream(WARNING, "E1000E") << "PHY read error for reg " << base::hex << (uint64_t)phyReg;
                    return 0;
                }
                return (uint16_t)(mdic & MDIC_DATA_MASK);
            }
        }

        KernelLogStream(WARNING, "E1000E") << "PHY read timeout for reg " << base::hex << (uint64_t)phyReg;
        return 0;
    }

    static void PhyWrite(uint32_t phyReg, uint16_t value) {
        uint32_t mdic = (uint32_t)value
                       | ((phyReg & 0x1F) << MDIC_REG_SHIFT)
                       | (1u << MDIC_PHY_SHIFT)
                       | MDIC_OP_WRITE;

        WriteReg(REG_MDIC, mdic);

        for (int i = 0; i < 200000; i++) {
            mdic = ReadReg(REG_MDIC);
            if (mdic & MDIC_READY) {
                if (mdic & MDIC_ERROR) {
                    KernelLogStream(WARNING, "E1000E") << "PHY write error for reg " << base::hex << (uint64_t)phyReg;
                }
                return;
            }
        }

        KernelLogStream(WARNING, "E1000E") << "PHY write timeout for reg " << base::hex << (uint64_t)phyReg;
    }

    // -------------------------------------------------------------------------
    // PHY initialization
    // -------------------------------------------------------------------------

    static void InitPhy() {
        // Reset the PHY
        PhyWrite(PHY_CONTROL, PHY_CTRL_RESET);

        // Wait for reset to complete
        for (int i = 0; i < 100000; i++) {
            uint16_t ctrl = PhyRead(PHY_CONTROL);
            if (!(ctrl & PHY_CTRL_RESET)) {
                break;
            }
        }

        // Advertise 10/100/1000 Mbps capabilities
        uint16_t anar = PhyRead(PHY_AUTONEG_ADV);
        anar |= (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8); // 10/100 HD/FD
        PhyWrite(PHY_AUTONEG_ADV, anar);

        // Advertise 1000BASE-T
        uint16_t gbcr = PhyRead(PHY_1000T_CTRL);
        gbcr |= (1 << 8) | (1 << 9); // 1000BASE-T HD/FD
        PhyWrite(PHY_1000T_CTRL, gbcr);

        // Enable and restart auto-negotiation
        PhyWrite(PHY_CONTROL, PHY_CTRL_AUTONEG_EN | PHY_CTRL_RESTART_AN);

        KernelLogStream(OK, "E1000E") << "PHY initialized, auto-negotiation started";
    }

    // -------------------------------------------------------------------------
    // EEPROM access (e1000e encoding differs from e1000)
    // -------------------------------------------------------------------------

    static uint16_t EepromRead(uint8_t address) {
        // E1000E: address shifted left by 2 (not 8), done bit at position 1 (not 4)
        WriteReg(REG_EERD, ((uint32_t)address << 2) | 1);

        uint32_t value;
        for (int i = 0; i < 10000; i++) {
            value = ReadReg(REG_EERD);
            if (value & (1 << 1)) {
                return (uint16_t)(value >> 16);
            }
        }

        KernelLogStream(WARNING, "E1000E") << "EEPROM read timeout for address " << base::hex << (uint64_t)address;
        return 0;
    }

    // -------------------------------------------------------------------------
    // MAC address
    // -------------------------------------------------------------------------

    static void ReadMacAddress() {
        // Try reading from RAL/RAH first
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
        uint64_t descPhys;
        g_rxDescs = (RxDescriptor*)AllocateDmaBuffer(descPhys);
        g_rxDescsPhys = descPhys;

        for (uint32_t i = 0; i < RX_DESC_COUNT; i++) {
            g_rxBuffers[i] = AllocateDmaBuffer(g_rxBuffersPhys[i]);

            // Allocate second page for larger buffer support
            uint64_t secondPhys;
            AllocateDmaBuffer(secondPhys);

            g_rxDescs[i].BufferAddress = g_rxBuffersPhys[i];
            g_rxDescs[i].Status = 0;
            g_rxDescs[i].Length = 0;
            g_rxDescs[i].Checksum = 0;
            g_rxDescs[i].Errors = 0;
            g_rxDescs[i].Special = 0;
        }

        WriteReg(REG_RDBAL, (uint32_t)(g_rxDescsPhys & 0xFFFFFFFF));
        WriteReg(REG_RDBAH, (uint32_t)(g_rxDescsPhys >> 32));
        WriteReg(REG_RDLEN, RX_DESC_COUNT * sizeof(RxDescriptor));
        WriteReg(REG_RDH, 0);
        WriteReg(REG_RDT, RX_DESC_COUNT - 1);

        g_rxTail = RX_DESC_COUNT - 1;

        uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_4096 | RCTL_BSEX;
        WriteReg(REG_RCTL, rctl);

        KernelLogStream(OK, "E1000E") << "RX ring configured: " << base::dec << (uint64_t)RX_DESC_COUNT << " descriptors";
    }

    // -------------------------------------------------------------------------
    // TX setup
    // -------------------------------------------------------------------------

    static void SetupTx() {
        uint64_t descPhys;
        g_txDescs = (TxDescriptor*)AllocateDmaBuffer(descPhys);
        g_txDescsPhys = descPhys;

        for (uint32_t i = 0; i < TX_DESC_COUNT; i++) {
            g_txBuffers[i] = AllocateDmaBuffer(g_txBuffersPhys[i]);

            g_txDescs[i].BufferAddress = g_txBuffersPhys[i];
            g_txDescs[i].Length = 0;
            g_txDescs[i].Command = 0;
            g_txDescs[i].Status = TXSTA_DD;
            g_txDescs[i].ChecksumOffset = 0;
            g_txDescs[i].ChecksumStart = 0;
            g_txDescs[i].Special = 0;
        }

        WriteReg(REG_TDBAL, (uint32_t)(g_txDescsPhys & 0xFFFFFFFF));
        WriteReg(REG_TDBAH, (uint32_t)(g_txDescsPhys >> 32));
        WriteReg(REG_TDLEN, TX_DESC_COUNT * sizeof(TxDescriptor));
        WriteReg(REG_TDH, 0);
        WriteReg(REG_TDT, 0);

        g_txTail = 0;

        uint32_t tctl = TCTL_EN | TCTL_PSP
                      | (15u << TCTL_CT_SHIFT)
                      | (64u << TCTL_COLD_SHIFT);
        WriteReg(REG_TCTL, tctl);

        WriteReg(REG_TIPG, 10 | (10 << 10) | (10 << 20));

        KernelLogStream(OK, "E1000E") << "TX ring configured: " << base::dec << (uint64_t)TX_DESC_COUNT << " descriptors";
    }

    // -------------------------------------------------------------------------
    // MSI setup
    // -------------------------------------------------------------------------

    static void HandleInterrupt(uint8_t irq); // forward declaration

    static bool SetupMsi(uint8_t bus, uint8_t dev, uint8_t func) {
        uint8_t cap = Pci::FindCapability(bus, dev, func, Pci::PCI_CAP_MSI);
        if (cap == 0) {
            KernelLogStream(INFO, "E1000E") << "MSI capability not found";
            return false;
        }

        KernelLogStream(INFO, "E1000E") << "MSI capability at offset " << base::hex << (uint64_t)cap;

        // Read Message Control (cap+2)
        uint16_t msgCtrl = Pci::LegacyRead16(bus, dev, func, cap + 2);
        bool is64bit = (msgCtrl & (1 << 7)) != 0;

        // Write Message Address (cap+4): BSP APIC ID 0, physical destination, fixed delivery
        Pci::LegacyWrite32(bus, dev, func, cap + 4, MSI_ADDR_BASE);

        // Write Message Data (vector number, edge-triggered, fixed delivery)
        if (is64bit) {
            // 64-bit: Upper Address at cap+8, Data at cap+12
            Pci::LegacyWrite32(bus, dev, func, cap + 8, 0);
            Pci::LegacyWrite16(bus, dev, func, cap + 12, MSI_VECTOR);
        } else {
            // 32-bit: Data at cap+8
            Pci::LegacyWrite16(bus, dev, func, cap + 8, MSI_VECTOR);
        }

        // Enable MSI: set bit 0 (MSI Enable), clear bits 6:4 (single message)
        msgCtrl &= ~(0x70);  // Clear Multiple Message Enable (bits 6:4)
        msgCtrl |= (1 << 0); // MSI Enable
        Pci::LegacyWrite16(bus, dev, func, cap + 2, msgCtrl);

        // Disable legacy INTx in PCI command register
        uint16_t pciCmd = Pci::LegacyRead16(bus, dev, func, PCI_REG_COMMAND);
        pciCmd |= PCI_CMD_INTX_DISABLE;
        Pci::LegacyWrite16(bus, dev, func, PCI_REG_COMMAND, pciCmd);

        // Register the interrupt handler for MSI vector
        Hal::RegisterIrqHandler(MSI_IRQ, HandleInterrupt);

        KernelLogStream(OK, "E1000E") << "MSI enabled: vector " << base::dec << (uint64_t)MSI_VECTOR
            << " (IRQ slot " << (uint64_t)MSI_IRQ << ")" << (is64bit ? " [64-bit]" : " [32-bit]");

        return true;
    }

    // -------------------------------------------------------------------------
    // Interrupt handler
    // -------------------------------------------------------------------------

    static void HandleInterrupt(uint8_t irq) {
        (void)irq;

        uint32_t icr = ReadReg(REG_ICR);

        // Spurious IRQ guard
        if (icr == 0) {
            return;
        }

        if (icr & ICR_LSC) {
            uint32_t status = ReadReg(REG_STATUS);
            bool linkUp = (status & (1 << 1)) != 0;
            KernelLogStream(INFO, "E1000E") << "Link status change: " << (linkUp ? "UP" : "DOWN");
        }

        if (icr & ICR_RXT0) {
            while (true) {
                uint32_t nextIdx = (g_rxTail + 1) % RX_DESC_COUNT;
                RxDescriptor& desc = g_rxDescs[nextIdx];

                if (!(desc.Status & RXSTA_DD)) {
                    break;
                }

                uint16_t length = desc.Length;
                g_rxPacketCount++;

                if (g_rxCallback != nullptr) {
                    g_rxCallback(g_rxBuffers[nextIdx], length);
                }

                desc.Status = 0;
                desc.Length = 0;
                desc.Errors = 0;

                g_rxTail = nextIdx;
                WriteReg(REG_RDT, g_rxTail);
            }
        }

        if (icr & (ICR_TXDW | ICR_TXQE)) {
            // TX completion — nothing to do for now
        }
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void Initialize() {
        KernelLogStream(INFO, "E1000E") << "Scanning for Intel e1000e NIC...";

        auto& devices = Pci::GetDevices();
        const Pci::PciDevice* foundDev = nullptr;
        const char* foundName = nullptr;

        for (uint64_t i = 0; i < devices.size(); i++) {
            if (devices[i].VendorId != VendorIntel) {
                continue;
            }
            for (uint32_t j = 0; j < g_deviceTableSize; j++) {
                if (devices[i].DeviceId == g_deviceTable[j].DeviceId) {
                    foundDev = &devices[i];
                    foundName = g_deviceTable[j].Name;
                    break;
                }
            }
            if (foundDev != nullptr) {
                break;
            }
        }

        if (foundDev == nullptr) {
            KernelLogStream(WARNING, "E1000E") << "No e1000e NIC found";
            return;
        }

        KernelLogStream(OK, "E1000E") << "Found " << foundName << " at PCI "
            << base::hex << (uint64_t)foundDev->Bus << ":"
            << (uint64_t)foundDev->Device << "." << (uint64_t)foundDev->Function;

        // Read BAR0 (MMIO base address), check for 64-bit BAR
        uint32_t bar0 = Pci::LegacyRead32(foundDev->Bus, foundDev->Device, foundDev->Function, PCI_REG_BAR0);
        uint64_t mmioPhys = bar0 & 0xFFFFFFF0;

        // Check if 64-bit BAR (type field bits 2:1 == 0b10)
        if ((bar0 & 0x06) == 0x04) {
            uint32_t bar1 = Pci::LegacyRead32(foundDev->Bus, foundDev->Device, foundDev->Function, PCI_REG_BAR1);
            mmioPhys |= ((uint64_t)bar1 << 32);
        }

        KernelLogStream(INFO, "E1000E") << "BAR0 physical: " << base::hex << mmioPhys;

        // Map the MMIO region (128KB = 32 pages)
        constexpr uint64_t MmioSize = 0x20000;
        for (uint64_t offset = 0; offset < MmioSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }

        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        // Enable bus mastering and memory space in PCI command register
        uint16_t pciCmd = Pci::LegacyRead16(foundDev->Bus, foundDev->Device, foundDev->Function, PCI_REG_COMMAND);
        pciCmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
        Pci::LegacyWrite16(foundDev->Bus, foundDev->Device, foundDev->Function, PCI_REG_COMMAND, pciCmd);

        KernelLogStream(OK, "E1000E") << "Bus mastering enabled";

        // Read interrupt line from PCI config (used for legacy IRQ fallback)
        g_irqLine = Pci::LegacyRead8(foundDev->Bus, foundDev->Device, foundDev->Function, PCI_REG_INTERRUPT);
        KernelLogStream(INFO, "E1000E") << "PCI IRQ line: " << base::dec << (uint64_t)g_irqLine;

        // --- ICH/PCH reset sequence ---

        // 1. Disable interrupts, flush ICR
        KernelLogStream(INFO, "E1000E") << "Disabling interrupts...";
        WriteReg(REG_IMC, 0xFFFFFFFF);
        ReadReg(REG_ICR);

        // 2. Acquire SW/FW semaphore (non-fatal if it fails)
        KernelLogStream(INFO, "E1000E") << "Acquiring semaphore...";
        AcquireSwFwSync();

        // 3. Reset the device
        KernelLogStream(INFO, "E1000E") << "Resetting device...";
        uint32_t ctrl = ReadReg(REG_CTRL);
        WriteReg(REG_CTRL, ctrl | CTRL_RST);

        // Post-reset settling delay (give hardware time before polling)
        for (int i = 0; i < 100000; i++) {
            asm volatile("" ::: "memory");
        }

        // Poll for reset completion
        for (int i = 0; i < 10000; i++) {
            if (!(ReadReg(REG_CTRL) & CTRL_RST)) {
                break;
            }
        }

        // 4. Release semaphore
        ReleaseSwFwSync();

        // 5. Disable interrupts again after reset
        WriteReg(REG_IMC, 0xFFFFFFFF);
        ReadReg(REG_ICR);

        KernelLogStream(OK, "E1000E") << "Reset complete";

        // Set link up: SLU on, but let auto-negotiation decide speed/duplex
        ctrl = ReadReg(REG_CTRL);
        ctrl |= CTRL_SLU;
        ctrl &= ~CTRL_FRCSPD;
        ctrl &= ~CTRL_FRCDPLX;
        ctrl &= ~(1u << 3);  // Clear LRST
        ctrl &= ~(1u << 31); // Clear PHY_RST
        ctrl &= ~(1u << 7);  // Clear ILOS
        WriteReg(REG_CTRL, ctrl);

        // Initialize PHY
        InitPhy();

        // Read MAC address
        ReadMacAddress();

        KernelLogStream(OK, "E1000E") << "MAC: "
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

        // Three-tier interrupt strategy: MSI → legacy IRQ → polling
        if (SetupMsi(foundDev->Bus, foundDev->Device, foundDev->Function)) {
            // MSI configured — enable NIC interrupt causes
            WriteReg(REG_IMS, ICR_RXT0 | ICR_TXDW | ICR_TXQE | ICR_LSC | ICR_RXDMT0);
        } else if (g_irqLine != 0xFF) {
            // Legacy IRQ fallback
            KernelLogStream(INFO, "E1000E") << "Falling back to legacy IRQ " << base::dec << (uint64_t)g_irqLine;
            Hal::RegisterIrqHandler(g_irqLine, HandleInterrupt);
            Hal::IoApic::UnmaskIrq(Hal::IoApic::GetGsiForIrq(g_irqLine));
            WriteReg(REG_IMS, ICR_RXT0 | ICR_TXDW | ICR_TXQE | ICR_LSC | ICR_RXDMT0);
        } else {
            // Polling as last resort
            KernelLogStream(WARNING, "E1000E") << "No MSI or legacy IRQ available, using polling mode";
            g_pollingMode = true;
        }

        g_initialized = true;

        uint32_t status = ReadReg(REG_STATUS);
        bool linkUp = (status & (1 << 1)) != 0;
        KernelLogStream(OK, "E1000E") << "Initialization complete, link: " << (linkUp ? "UP" : "DOWN");
    }

    // -------------------------------------------------------------------------
    // Polling: process received packets (with reentrancy guard)
    // -------------------------------------------------------------------------

    static bool g_polling = false;

    static void PollRx() {
        // Guard against reentrancy: RX callback can trigger ARP reply →
        // Ethernet::Send → SendPacket → PollRx(). Two concurrent callers
        // modifying g_rxTail / RDT would corrupt the descriptor ring.
        if (g_polling) {
            return;
        }
        g_polling = true;

        while (true) {
            uint32_t nextIdx = (g_rxTail + 1) % RX_DESC_COUNT;
            RxDescriptor& desc = g_rxDescs[nextIdx];

            if (!(desc.Status & RXSTA_DD)) {
                break;
            }

            uint16_t length = desc.Length;
            g_rxPacketCount++;

            if (g_rxCallback != nullptr) {
                g_rxCallback(g_rxBuffers[nextIdx], length);
            }

            desc.Status = 0;
            desc.Length = 0;
            desc.Errors = 0;

            g_rxTail = nextIdx;
            WriteReg(REG_RDT, g_rxTail);
        }

        g_polling = false;
    }

    bool SendPacket(const uint8_t* data, uint16_t length) {
        if (!g_initialized || data == nullptr || length == 0 || length > 1518) {
            return false;
        }

        TxDescriptor& desc = g_txDescs[g_txTail];
        if (!(desc.Status & TXSTA_DD)) {
            KernelLogStream(WARNING, "E1000E") << "TX ring full";
            return false;
        }

        memcpy(g_txBuffers[g_txTail], data, length);

        desc.BufferAddress = g_txBuffersPhys[g_txTail];
        desc.Length = length;
        desc.Command = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
        desc.Status = 0;

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

    void Poll() {
        if (!g_initialized) {
            return;
        }
        PollRx();
    }

};
