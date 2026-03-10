/*
    * Nvme.cpp
    * NVM Express (NVMe) storage driver
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Nvme.hpp"
#include "BlockDevice.hpp"
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

namespace Drivers::Storage::Nvme {

    // -------------------------------------------------------------------------
    // Driver state
    // -------------------------------------------------------------------------

    static bool g_initialized = false;
    static volatile uint8_t* g_mmioBase = nullptr;
    static uint32_t g_doorbellStride = 0;   // in bytes (4 << DSTRD)
    static uint16_t g_maxQueueEntries = 0;  // CAP.MQES + 1

    // Admin queue state
    static SqEntry*  g_adminSq = nullptr;
    static CqEntry*  g_adminCq = nullptr;
    static uint64_t  g_adminSqPhys = 0;
    static uint64_t  g_adminCqPhys = 0;
    static uint16_t  g_adminSqTail = 0;
    static uint16_t  g_adminCqHead = 0;
    static uint8_t   g_adminCqPhase = 1;    // Expected phase bit starts at 1
    static uint16_t  g_adminCmdId = 0;

    // I/O queue state (single I/O queue pair)
    static SqEntry*  g_ioSq = nullptr;
    static CqEntry*  g_ioCq = nullptr;
    static uint64_t  g_ioSqPhys = 0;
    static uint64_t  g_ioCqPhys = 0;
    static uint16_t  g_ioSqTail = 0;
    static uint16_t  g_ioCqHead = 0;
    static uint8_t   g_ioCqPhase = 1;
    static uint16_t  g_ioCmdId = 0;
    static uint16_t  g_ioSqDepth = 0;
    static uint16_t  g_ioCqDepth = 0;

    // Namespace state
    static int g_nsCount = 0;
    static NamespaceInfo g_namespaces[MAX_NAMESPACES] = {};

    // Controller identify data
    static uint32_t g_mdts = 0;  // Max Data Transfer Size in pages (0 = unlimited)

    // -------------------------------------------------------------------------
    // Register access
    // -------------------------------------------------------------------------

    static void WriteReg32(uint32_t reg, uint32_t value) {
        *(volatile uint32_t*)(g_mmioBase + reg) = value;
    }

    static uint32_t ReadReg32(uint32_t reg) {
        return *(volatile uint32_t*)(g_mmioBase + reg);
    }

    static void WriteReg64(uint32_t reg, uint64_t value) {
        *(volatile uint32_t*)(g_mmioBase + reg) = (uint32_t)(value & 0xFFFFFFFF);
        *(volatile uint32_t*)(g_mmioBase + reg + 4) = (uint32_t)(value >> 32);
    }

    static uint64_t ReadReg64(uint32_t reg) {
        uint32_t lo = *(volatile uint32_t*)(g_mmioBase + reg);
        uint32_t hi = *(volatile uint32_t*)(g_mmioBase + reg + 4);
        return (uint64_t)lo | ((uint64_t)hi << 32);
    }

    // -------------------------------------------------------------------------
    // Doorbell registers
    // NVMe spec: SQ y Tail Doorbell offset = 0x1000 + (2y * doorbellStride)
    //            CQ y Head Doorbell offset = 0x1000 + ((2y+1) * doorbellStride)
    // -------------------------------------------------------------------------

    static void WriteSqTailDoorbell(uint16_t queueId, uint16_t value) {
        uint32_t offset = 0x1000 + (2 * queueId) * g_doorbellStride;
        *(volatile uint32_t*)(g_mmioBase + offset) = value;
    }

    static void WriteCqHeadDoorbell(uint16_t queueId, uint16_t value) {
        uint32_t offset = 0x1000 + (2 * queueId + 1) * g_doorbellStride;
        *(volatile uint32_t*)(g_mmioBase + offset) = value;
    }

    // -------------------------------------------------------------------------
    // DMA buffer allocation
    // -------------------------------------------------------------------------

    static void* AllocateDmaBuffer(uint64_t& outPhys, int pages = 1) {
        void* virt;
        if (pages == 1) {
            virt = Memory::g_pfa->AllocateZeroed();
        } else {
            virt = Memory::g_pfa->ReallocConsecutive(nullptr, pages);
            memset(virt, 0, pages * 0x1000);
        }
        outPhys = Memory::SubHHDM(virt);
        return virt;
    }

    // -------------------------------------------------------------------------
    // Admin command submission
    // -------------------------------------------------------------------------

    static void SubmitAdminCommand(SqEntry& cmd) {
        cmd.CommandId = g_adminCmdId++;
        g_adminSq[g_adminSqTail] = cmd;
        g_adminSqTail = (g_adminSqTail + 1) % ADMIN_QUEUE_DEPTH;
        WriteSqTailDoorbell(0, g_adminSqTail);
    }

    static bool WaitAdminCompletion(CqEntry& out) {
        for (int i = 0; i < 5000000; i++) {
            CqEntry* cqe = &g_adminCq[g_adminCqHead];
            uint16_t status = cqe->Status;

            // Check phase bit matches expected
            if ((status & CQE_PHASE_BIT) == g_adminCqPhase) {
                out = *cqe;

                // Advance CQ head
                g_adminCqHead++;
                if (g_adminCqHead >= ADMIN_QUEUE_DEPTH) {
                    g_adminCqHead = 0;
                    g_adminCqPhase ^= 1;  // Toggle expected phase
                }
                WriteCqHeadDoorbell(0, g_adminCqHead);

                // Check status code (bits 15:1, 0 = success)
                if (status & CQE_STATUS_MASK) {
                    KernelLogStream(ERROR, "NVMe") << "Admin command failed, status="
                        << base::hex << (uint64_t)(status >> 1);
                    return false;
                }
                return true;
            }
            asm volatile("" ::: "memory");
        }

        KernelLogStream(ERROR, "NVMe") << "Admin command timeout";
        return false;
    }

    // Submit an admin command and wait for completion
    static bool AdminCommand(SqEntry& cmd, CqEntry& cqe) {
        SubmitAdminCommand(cmd);
        return WaitAdminCompletion(cqe);
    }

    // -------------------------------------------------------------------------
    // I/O command submission
    // -------------------------------------------------------------------------

    static void SubmitIoCommand(SqEntry& cmd) {
        cmd.CommandId = g_ioCmdId++;
        g_ioSq[g_ioSqTail] = cmd;
        g_ioSqTail = (g_ioSqTail + 1) % g_ioSqDepth;
        WriteSqTailDoorbell(1, g_ioSqTail);
    }

    static bool WaitIoCompletion(CqEntry& out) {
        for (int i = 0; i < 5000000; i++) {
            CqEntry* cqe = &g_ioCq[g_ioCqHead];
            uint16_t status = cqe->Status;

            if ((status & CQE_PHASE_BIT) == g_ioCqPhase) {
                out = *cqe;

                g_ioCqHead++;
                if (g_ioCqHead >= g_ioCqDepth) {
                    g_ioCqHead = 0;
                    g_ioCqPhase ^= 1;
                }
                WriteCqHeadDoorbell(1, g_ioCqHead);

                if (status & CQE_STATUS_MASK) {
                    KernelLogStream(ERROR, "NVMe") << "I/O command failed, status="
                        << base::hex << (uint64_t)(status >> 1);
                    return false;
                }
                return true;
            }
            asm volatile("" ::: "memory");
        }

        KernelLogStream(ERROR, "NVMe") << "I/O command timeout";
        return false;
    }

    // -------------------------------------------------------------------------
    // Controller disable/enable
    // -------------------------------------------------------------------------

    static bool DisableController() {
        uint32_t cc = ReadReg32(REG_CC);
        cc &= ~CC_EN;
        WriteReg32(REG_CC, cc);

        // Wait for CSTS.RDY to become 0
        for (int i = 0; i < 5000000; i++) {
            uint32_t csts = ReadReg32(REG_CSTS);
            if (csts & CSTS_CFS) {
                KernelLogStream(ERROR, "NVMe") << "Controller fatal status during disable";
                return false;
            }
            if (!(csts & CSTS_RDY)) {
                return true;
            }
            asm volatile("" ::: "memory");
        }

        KernelLogStream(ERROR, "NVMe") << "Controller disable timeout";
        return false;
    }

    static bool EnableController() {
        // Configure CC: enable, NVM command set, 4 KiB pages,
        // SQ entry size = 64 bytes (2^6), CQ entry size = 16 bytes (2^4)
        uint32_t cc = CC_EN | CC_CSS_NVM | CC_AMS_RR | CC_SHN_NONE;
        cc |= (0u << CC_MPS_SHIFT);      // MPS = 0 => 4 KiB pages
        cc |= (6u << CC_IOSQES_SHIFT);   // SQ entry = 2^6 = 64 bytes
        cc |= (4u << CC_IOCQES_SHIFT);   // CQ entry = 2^4 = 16 bytes
        WriteReg32(REG_CC, cc);

        // Wait for CSTS.RDY
        for (int i = 0; i < 5000000; i++) {
            uint32_t csts = ReadReg32(REG_CSTS);
            if (csts & CSTS_CFS) {
                KernelLogStream(ERROR, "NVMe") << "Controller fatal status during enable";
                return false;
            }
            if (csts & CSTS_RDY) {
                return true;
            }
            asm volatile("" ::: "memory");
        }

        KernelLogStream(ERROR, "NVMe") << "Controller enable timeout";
        return false;
    }

    // -------------------------------------------------------------------------
    // Admin queue setup
    // -------------------------------------------------------------------------

    static bool SetupAdminQueues() {
        // Allocate Admin Submission Queue (ADMIN_QUEUE_DEPTH * 64 bytes)
        // At 32 entries * 64 bytes = 2048 bytes, fits in 1 page
        g_adminSq = (SqEntry*)AllocateDmaBuffer(g_adminSqPhys);

        // Allocate Admin Completion Queue (ADMIN_QUEUE_DEPTH * 16 bytes)
        // At 32 entries * 16 bytes = 512 bytes, fits in 1 page
        g_adminCq = (CqEntry*)AllocateDmaBuffer(g_adminCqPhys);

        g_adminSqTail = 0;
        g_adminCqHead = 0;
        g_adminCqPhase = 1;
        g_adminCmdId = 0;

        // Set Admin Queue Attributes
        // AQA: bits 27:16 = ACQS (CQ size - 1), bits 11:0 = ASQS (SQ size - 1)
        uint32_t aqa = ((ADMIN_QUEUE_DEPTH - 1) << 16) | (ADMIN_QUEUE_DEPTH - 1);
        WriteReg32(REG_AQA, aqa);

        // Set Admin SQ and CQ base addresses
        WriteReg64(REG_ASQ, g_adminSqPhys);
        WriteReg64(REG_ACQ, g_adminCqPhys);

        return true;
    }

    // -------------------------------------------------------------------------
    // Identify Controller
    // -------------------------------------------------------------------------

    static bool IdentifyController() {
        uint64_t identPhys;
        uint8_t* identData = (uint8_t*)AllocateDmaBuffer(identPhys);

        SqEntry cmd = {};
        cmd.Opcode = ADMIN_IDENTIFY;
        cmd.Nsid = 0;
        cmd.Prp1 = identPhys;
        cmd.Prp2 = 0;
        cmd.Cdw10 = IDENTIFY_CNS_CONTROLLER;

        CqEntry cqe;
        if (!AdminCommand(cmd, cqe)) {
            KernelLogStream(ERROR, "NVMe") << "Identify Controller failed";
            Memory::g_pfa->Free(identData);
            return false;
        }

        // Parse controller data (4096 bytes)
        // Bytes 24-63: Serial Number (20 bytes, space-padded ASCII)
        // Bytes 64-103: Model Number (40 bytes, space-padded ASCII)
        // Bytes 104-111: Firmware Revision (8 bytes)
        // Byte 77: MDTS (Maximum Data Transfer Size, in units of CAP.MPSMIN pages)

        // Model string
        char model[41];
        memcpy(model, identData + 24, 40);
        model[40] = '\0';
        // Trim trailing spaces
        for (int i = 39; i >= 0; i--) {
            if (model[i] == ' ' || model[i] == '\0') {
                model[i] = '\0';
            } else {
                break;
            }
        }

        // MDTS
        uint8_t mdtsPower = identData[77];
        if (mdtsPower > 0) {
            g_mdts = (1u << mdtsPower);  // In minimum page size units (4 KiB pages)
        } else {
            g_mdts = 0;  // No limit
        }

        // NN (Number of Namespaces): bytes 516-519
        uint32_t nn = *(uint32_t*)(identData + 516);

        KernelLogStream(OK, "NVMe") << "Controller: " << model;
        KernelLogStream(INFO, "NVMe") << "MDTS: " << (g_mdts ? (uint64_t)(g_mdts * 4) : 0)
            << " KiB, Namespaces: " << (uint64_t)nn;

        // Copy model to all namespaces (will be used during registration)
        for (int i = 0; i < MAX_NAMESPACES; i++) {
            memcpy(g_namespaces[i].Model, model, 41);
        }

        // Enumerate active namespaces
        if (nn > (uint32_t)MAX_NAMESPACES) nn = MAX_NAMESPACES;

        for (uint32_t nsid = 1; nsid <= nn; nsid++) {
            // Identify Namespace
            uint64_t nsIdentPhys;
            uint8_t* nsIdentData = (uint8_t*)AllocateDmaBuffer(nsIdentPhys);

            SqEntry nsCmd = {};
            nsCmd.Opcode = ADMIN_IDENTIFY;
            nsCmd.Nsid = nsid;
            nsCmd.Prp1 = nsIdentPhys;
            nsCmd.Prp2 = 0;
            nsCmd.Cdw10 = IDENTIFY_CNS_NAMESPACE;

            CqEntry nsCqe;
            if (!AdminCommand(nsCmd, nsCqe)) {
                KernelLogStream(WARNING, "NVMe") << "Identify Namespace " << (uint64_t)nsid << " failed";
                Memory::g_pfa->Free(nsIdentData);
                continue;
            }

            // Bytes 0-7: NSZE (Namespace Size in LBAs)
            uint64_t nsze = *(uint64_t*)(nsIdentData + 0);

            if (nsze == 0) {
                Memory::g_pfa->Free(nsIdentData);
                continue;
            }

            // Byte 26: NLBAF (Number of LBA Formats, 0-based)
            // Byte 25:24 - FLBAS (Formatted LBA Size)
            //   bits 3:0 = index of the LBA format in use
            uint8_t flbas = nsIdentData[26];
            uint8_t lbaFmtIdx = flbas & 0x0F;

            // LBA Format descriptors start at byte 128, each is 4 bytes
            // Bits 23:16 = LBADS (LBA Data Size as power of 2)
            uint32_t lbaFmt = *(uint32_t*)(nsIdentData + 128 + lbaFmtIdx * 4);
            uint8_t lbads = (lbaFmt >> 16) & 0xFF;
            uint32_t sectorSize = (1u << lbads);

            int idx = g_nsCount;
            g_namespaces[idx].Active = true;
            g_namespaces[idx].Nsid = nsid;
            g_namespaces[idx].SectorCount = nsze;
            g_namespaces[idx].SectorSize = sectorSize;

            // Compute max transfer in blocks
            if (g_mdts > 0) {
                g_namespaces[idx].MaxTransferBlocks = (g_mdts * 0x1000) / sectorSize;
            } else {
                // Conservative default: 128 sectors (64 KiB for 512-byte sectors)
                g_namespaces[idx].MaxTransferBlocks = 128;
            }

            g_nsCount++;

            uint64_t sizeBytes = nsze * sectorSize;
            uint64_t sizeMB = sizeBytes / (1024 * 1024);
            uint64_t sizeGB = sizeMB / 1024;

            if (sizeGB > 0) {
                KernelLogStream(OK, "NVMe") << "Namespace " << (uint64_t)nsid
                    << ": " << sizeGB << " GiB (" << (uint64_t)sectorSize << " B/sector)";
            } else {
                KernelLogStream(OK, "NVMe") << "Namespace " << (uint64_t)nsid
                    << ": " << sizeMB << " MiB (" << (uint64_t)sectorSize << " B/sector)";
            }

            Memory::g_pfa->Free(nsIdentData);
        }

        Memory::g_pfa->Free(identData);
        return g_nsCount > 0;
    }

    // -------------------------------------------------------------------------
    // Create I/O queues
    // -------------------------------------------------------------------------

    static bool SetNumberOfQueues(uint16_t& sqCount, uint16_t& cqCount) {
        SqEntry cmd = {};
        cmd.Opcode = ADMIN_SET_FEATURES;
        cmd.Cdw10 = FEATURE_NUM_QUEUES;
        // CDW11: bits 31:16 = number of CQs requested (0-based)
        //        bits 15:0  = number of SQs requested (0-based)
        cmd.Cdw11 = ((uint32_t)(cqCount - 1) << 16) | (sqCount - 1);

        CqEntry cqe;
        if (!AdminCommand(cmd, cqe)) {
            return false;
        }

        // Result DW0 contains allocated counts (0-based)
        sqCount = (cqe.Result & 0xFFFF) + 1;
        cqCount = ((cqe.Result >> 16) & 0xFFFF) + 1;
        return true;
    }

    static bool CreateIoQueues() {
        // Request 1 SQ + 1 CQ
        uint16_t sqCount = 1;
        uint16_t cqCount = 1;
        if (!SetNumberOfQueues(sqCount, cqCount)) {
            KernelLogStream(ERROR, "NVMe") << "Set Number of Queues failed";
            return false;
        }

        KernelLogStream(INFO, "NVMe") << "Allocated " << (uint64_t)sqCount
            << " SQ(s), " << (uint64_t)cqCount << " CQ(s)";

        // Determine queue depths (capped by controller max)
        g_ioSqDepth = IO_QUEUE_DEPTH;
        g_ioCqDepth = IO_QUEUE_DEPTH;
        if (g_ioSqDepth > g_maxQueueEntries) g_ioSqDepth = g_maxQueueEntries;
        if (g_ioCqDepth > g_maxQueueEntries) g_ioCqDepth = g_maxQueueEntries;

        // Allocate I/O CQ
        int cqPages = ((uint32_t)g_ioCqDepth * sizeof(CqEntry) + 0xFFF) / 0x1000;
        g_ioCq = (CqEntry*)AllocateDmaBuffer(g_ioCqPhys, cqPages);
        g_ioCqHead = 0;
        g_ioCqPhase = 1;

        // Create I/O Completion Queue (queue ID = 1)
        {
            SqEntry cmd = {};
            cmd.Opcode = ADMIN_CREATE_IO_CQ;
            cmd.Prp1 = g_ioCqPhys;
            // CDW10: bits 31:16 = queue size (0-based), bits 15:0 = queue ID
            cmd.Cdw10 = ((uint32_t)(g_ioCqDepth - 1) << 16) | 1;
            // CDW11: bit 0 = physically contiguous, bit 1 = interrupts enabled
            //        bits 31:16 = interrupt vector
            cmd.Cdw11 = (1u << 0) | (1u << 1) | (0u << 16);

            CqEntry cqe;
            if (!AdminCommand(cmd, cqe)) {
                KernelLogStream(ERROR, "NVMe") << "Create I/O CQ failed";
                return false;
            }
        }

        // Allocate I/O SQ
        int sqPages = ((uint32_t)g_ioSqDepth * sizeof(SqEntry) + 0xFFF) / 0x1000;
        g_ioSq = (SqEntry*)AllocateDmaBuffer(g_ioSqPhys, sqPages);
        g_ioSqTail = 0;
        g_ioCmdId = 0;

        // Create I/O Submission Queue (queue ID = 1, linked to CQ ID = 1)
        {
            SqEntry cmd = {};
            cmd.Opcode = ADMIN_CREATE_IO_SQ;
            cmd.Prp1 = g_ioSqPhys;
            // CDW10: bits 31:16 = queue size (0-based), bits 15:0 = queue ID
            cmd.Cdw10 = ((uint32_t)(g_ioSqDepth - 1) << 16) | 1;
            // CDW11: bit 0 = physically contiguous, bits 31:16 = CQ ID
            cmd.Cdw11 = (1u << 0) | (1u << 16);

            CqEntry cqe;
            if (!AdminCommand(cmd, cqe)) {
                KernelLogStream(ERROR, "NVMe") << "Create I/O SQ failed";
                return false;
            }
        }

        KernelLogStream(OK, "NVMe") << "I/O queues created (depth " << (uint64_t)g_ioSqDepth << ")";
        return true;
    }

    // -------------------------------------------------------------------------
    // Interrupt handler
    // -------------------------------------------------------------------------

    static void HandleInterrupt(uint8_t irq) {
        (void)irq;
        // NVMe uses polling-based completion in this driver.
        // The interrupt handler just acknowledges the interrupt.
        // Completions are consumed in the polling loops above.
    }

    // -------------------------------------------------------------------------
    // MSI setup
    // -------------------------------------------------------------------------

    static bool SetupMsi(uint8_t bus, uint8_t dev, uint8_t func) {
        uint8_t cap = Pci::FindCapability(bus, dev, func, Pci::PCI_CAP_MSI);
        if (cap == 0) {
            KernelLogStream(INFO, "NVMe") << "MSI capability not found";
            return false;
        }

        uint16_t msgCtrl = Pci::LegacyRead16(bus, dev, func, cap + 2);
        bool is64bit = (msgCtrl & (1 << 7)) != 0;

        Pci::LegacyWrite32(bus, dev, func, cap + 4, MSI_ADDR_BASE);

        if (is64bit) {
            Pci::LegacyWrite32(bus, dev, func, cap + 8, 0);
            Pci::LegacyWrite16(bus, dev, func, cap + 12, MSI_VECTOR);
        } else {
            Pci::LegacyWrite16(bus, dev, func, cap + 8, MSI_VECTOR);
        }

        msgCtrl &= ~(0x70);  // Single message
        msgCtrl |= (1 << 0); // MSI Enable
        Pci::LegacyWrite16(bus, dev, func, cap + 2, msgCtrl);

        uint16_t pciCmd = Pci::LegacyRead16(bus, dev, func, (uint8_t)Pci::PCI_REG_COMMAND);
        pciCmd |= Pci::PCI_CMD_INTX_DISABLE;
        Pci::LegacyWrite16(bus, dev, func, (uint8_t)Pci::PCI_REG_COMMAND, pciCmd);

        Hal::RegisterIrqHandler(MSI_IRQ, HandleInterrupt);

        KernelLogStream(OK, "NVMe") << "MSI enabled: vector " << base::dec << (uint64_t)MSI_VECTOR;
        return true;
    }

    // -------------------------------------------------------------------------
    // Probe (PCI driver entry point)
    // -------------------------------------------------------------------------

    bool Probe(const Pci::PciDevice& dev) {
        if (g_initialized) return false;

        KernelLogStream(OK, "NVMe") << "Found NVMe controller at PCI "
            << base::hex << (uint64_t)dev.Bus << ":"
            << (uint64_t)dev.Device << "." << (uint64_t)dev.Function
            << " (" << (uint64_t)dev.VendorId << ":" << (uint64_t)dev.DeviceId << ")";

        // NVMe uses BAR0 for MMIO registers
        uint64_t mmioPhys = Pci::ReadBar0(dev.Bus, dev.Device, dev.Function);
        if (mmioPhys == 0) {
            KernelLogStream(ERROR, "NVMe") << "BAR0 is zero";
            return false;
        }

        KernelLogStream(INFO, "NVMe") << "BAR0 physical: " << base::hex << mmioPhys;

        // Map MMIO region. NVMe requires at least the controller registers (0x1000)
        // plus doorbell registers. Map 16 KiB to cover admin + a few I/O doorbells.
        constexpr uint64_t MmioSize = 0x4000;
        for (uint64_t offset = 0; offset < MmioSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }

        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        // Enable bus mastering and memory space
        Pci::EnableBusMaster(dev.Bus, dev.Device, dev.Function);

        // Read capabilities
        uint64_t cap = ReadReg64(REG_CAP);
        g_maxQueueEntries = (uint16_t)((cap & CAP_MQES_MASK) + 1);
        uint32_t dstrd = (uint32_t)((cap & CAP_DSTRD_MASK) >> CAP_DSTRD_SHIFT);
        g_doorbellStride = 4u << dstrd;

        // Read version
        uint32_t vs = ReadReg32(REG_VS);
        uint32_t vsMajor = (vs >> 16) & 0xFFFF;
        uint32_t vsMinor = (vs >> 8) & 0xFF;
        uint32_t vsTertiary = vs & 0xFF;

        KernelLogStream(INFO, "NVMe") << "Version: " << base::dec
            << (uint64_t)vsMajor << "." << (uint64_t)vsMinor << "." << (uint64_t)vsTertiary;
        KernelLogStream(INFO, "NVMe") << "Max queue entries: " << (uint64_t)g_maxQueueEntries
            << ", Doorbell stride: " << (uint64_t)g_doorbellStride << " bytes";

        // Step 1: Disable controller
        if (!DisableController()) {
            KernelLogStream(ERROR, "NVMe") << "Failed to disable controller";
            return false;
        }

        // Step 2: Set up MSI
        bool hasMsi = SetupMsi(dev.Bus, dev.Device, dev.Function);
        if (!hasMsi) {
            uint8_t irqLine = Pci::LegacyRead8(dev.Bus, dev.Device, dev.Function,
                (uint8_t)Pci::PCI_REG_INTERRUPT);
            if (irqLine != 0xFF) {
                KernelLogStream(INFO, "NVMe") << "Using legacy IRQ " << base::dec << (uint64_t)irqLine;
                Hal::RegisterIrqHandler(irqLine, HandleInterrupt);
                Hal::IoApic::UnmaskIrq(Hal::IoApic::GetGsiForIrq(irqLine));
            }
        }

        // Step 3: Set up admin queues
        if (!SetupAdminQueues()) {
            KernelLogStream(ERROR, "NVMe") << "Failed to set up admin queues";
            return false;
        }

        // Step 4: Enable controller
        if (!EnableController()) {
            KernelLogStream(ERROR, "NVMe") << "Failed to enable controller";
            return false;
        }

        KernelLogStream(OK, "NVMe") << "Controller enabled and ready";

        // Step 5: Identify controller and namespaces
        if (!IdentifyController()) {
            KernelLogStream(ERROR, "NVMe") << "Failed to identify controller/namespaces";
            return false;
        }

        // Step 6: Create I/O queues
        if (!CreateIoQueues()) {
            KernelLogStream(ERROR, "NVMe") << "Failed to create I/O queues";
            return false;
        }

        // Step 7: Register namespaces as block devices
        for (int i = 0; i < g_nsCount; i++) {
            Storage::BlockDevice bdev = {};
            bdev.ReadSectors = [](void* ctx, uint64_t lba, uint32_t count, void* buffer) -> bool {
                return ReadSectors((int)(uintptr_t)ctx, lba, count, buffer);
            };
            bdev.WriteSectors = [](void* ctx, uint64_t lba, uint32_t count, const void* buffer) -> bool {
                return WriteSectors((int)(uintptr_t)ctx, lba, count, buffer);
            };
            bdev.Ctx = (void*)(uintptr_t)i;
            bdev.SectorCount = g_namespaces[i].SectorCount;
            bdev.SectorSize = (uint16_t)g_namespaces[i].SectorSize;
            memcpy(bdev.Model, g_namespaces[i].Model, 41);
            Storage::RegisterBlockDevice(bdev);
        }

        g_initialized = true;

        KernelLogStream(OK, "NVMe") << "Initialization complete: "
            << base::dec << (uint64_t)g_nsCount << " namespace(s) ready";

        return true;
    }

    // -------------------------------------------------------------------------
    // Public API: Read/Write
    // -------------------------------------------------------------------------

    bool IsInitialized() {
        return g_initialized;
    }

    int GetNamespaceCount() {
        return g_nsCount;
    }

    const NamespaceInfo* GetNamespaceInfo(int ns) {
        if (ns < 0 || ns >= g_nsCount || !g_namespaces[ns].Active) {
            return nullptr;
        }
        return &g_namespaces[ns];
    }

    bool ReadSectors(int ns, uint64_t lba, uint32_t count, void* buffer) {
        if (!g_initialized || ns < 0 || ns >= g_nsCount || !g_namespaces[ns].Active) {
            return false;
        }
        if (count == 0 || buffer == nullptr) return false;

        // Limit to max transfer size
        uint32_t maxBlocks = g_namespaces[ns].MaxTransferBlocks;
        if (count > maxBlocks) {
            KernelLogStream(ERROR, "NVMe") << "ReadSectors: count " << count
                << " exceeds max " << maxBlocks;
            return false;
        }

        uint32_t sectorSize = g_namespaces[ns].SectorSize;
        uint32_t totalBytes = count * sectorSize;
        int pagesNeeded = (totalBytes + 0xFFF) / 0x1000;

        uint64_t dmaPhys;
        void* dmaVirt = AllocateDmaBuffer(dmaPhys, pagesNeeded);

        // Build NVMe Read command
        SqEntry cmd = {};
        cmd.Opcode = IO_CMD_READ;
        cmd.Nsid = g_namespaces[ns].Nsid;
        cmd.Prp1 = dmaPhys;

        // PRP2: If transfer spans more than one page, set PRP2
        if (totalBytes > 0x1000) {
            // For transfers spanning exactly 2 pages, PRP2 = second page address
            // For transfers spanning more pages, PRP2 should point to a PRP list.
            // Since our DMA buffer is physically contiguous, we can point to the
            // second page directly for 2-page transfers. For larger transfers,
            // we build a PRP list.
            if (pagesNeeded == 2) {
                cmd.Prp2 = dmaPhys + 0x1000;
            } else {
                // Build a PRP list (array of physical page addresses)
                uint64_t prpListPhys;
                uint64_t* prpList = (uint64_t*)AllocateDmaBuffer(prpListPhys);
                for (int i = 1; i < pagesNeeded; i++) {
                    prpList[i - 1] = dmaPhys + (uint64_t)i * 0x1000;
                }
                cmd.Prp2 = prpListPhys;
            }
        }

        // CDW10-11: Starting LBA (64-bit)
        cmd.Cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
        cmd.Cdw11 = (uint32_t)(lba >> 32);
        // CDW12: bits 15:0 = Number of Logical Blocks (0-based)
        cmd.Cdw12 = count - 1;

        SubmitIoCommand(cmd);

        CqEntry cqe;
        bool ok = WaitIoCompletion(cqe);
        if (ok) {
            memcpy(buffer, dmaVirt, totalBytes);
        }

        // Free PRP list if we allocated one
        if (pagesNeeded > 2 && cmd.Prp2 != 0 && cmd.Prp2 != dmaPhys + 0x1000) {
            Memory::g_pfa->Free((void*)Memory::HHDM(cmd.Prp2));
        }

        Memory::g_pfa->Free(dmaVirt, pagesNeeded);
        return ok;
    }

    bool WriteSectors(int ns, uint64_t lba, uint32_t count, const void* buffer) {
        if (!g_initialized || ns < 0 || ns >= g_nsCount || !g_namespaces[ns].Active) {
            return false;
        }
        if (count == 0 || buffer == nullptr) return false;

        uint32_t maxBlocks = g_namespaces[ns].MaxTransferBlocks;
        if (count > maxBlocks) {
            KernelLogStream(ERROR, "NVMe") << "WriteSectors: count " << count
                << " exceeds max " << maxBlocks;
            return false;
        }

        uint32_t sectorSize = g_namespaces[ns].SectorSize;
        uint32_t totalBytes = count * sectorSize;
        int pagesNeeded = (totalBytes + 0xFFF) / 0x1000;

        uint64_t dmaPhys;
        void* dmaVirt = AllocateDmaBuffer(dmaPhys, pagesNeeded);
        memcpy(dmaVirt, buffer, totalBytes);

        // Build NVMe Write command
        SqEntry cmd = {};
        cmd.Opcode = IO_CMD_WRITE;
        cmd.Nsid = g_namespaces[ns].Nsid;
        cmd.Prp1 = dmaPhys;

        if (totalBytes > 0x1000) {
            if (pagesNeeded == 2) {
                cmd.Prp2 = dmaPhys + 0x1000;
            } else {
                uint64_t prpListPhys;
                uint64_t* prpList = (uint64_t*)AllocateDmaBuffer(prpListPhys);
                for (int i = 1; i < pagesNeeded; i++) {
                    prpList[i - 1] = dmaPhys + (uint64_t)i * 0x1000;
                }
                cmd.Prp2 = prpListPhys;
            }
        }

        cmd.Cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
        cmd.Cdw11 = (uint32_t)(lba >> 32);
        cmd.Cdw12 = count - 1;

        SubmitIoCommand(cmd);

        CqEntry cqe;
        bool ok = WaitIoCompletion(cqe);

        if (pagesNeeded > 2 && cmd.Prp2 != 0 && cmd.Prp2 != dmaPhys + 0x1000) {
            Memory::g_pfa->Free((void*)Memory::HHDM(cmd.Prp2));
        }

        Memory::g_pfa->Free(dmaVirt, pagesNeeded);
        return ok;
    }

};
