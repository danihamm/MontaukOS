/*
    * IntelGPU.cpp
    * Intel integrated graphics (i915) modesetting driver
    * Scans PCI for Intel display controllers, maps MMIO, initializes GTT,
    * and sets up a framebuffer using the firmware's existing display timings.
    * Copyright (c) 2025 Daniel Hammer
*/

#include "IntelGPU.hpp"
#include <Pci/Pci.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Io/IoPort.hpp>
#include <Graphics/Cursor.hpp>

using namespace Kt;

namespace Drivers::Graphics::IntelGPU {

    // =========================================================================
    // Driver state
    // =========================================================================

    static bool g_initialized = false;

    static GpuInfo g_gpuInfo = {};

    static volatile uint8_t* g_mmioBase = nullptr;

    // GTT state
    static volatile void* g_gttBase = nullptr;    // Virtual address of GTT entries
    static uint64_t g_gttEntryCount = 0;           // Number of GTT entries
    static uint64_t g_scratchPagePhys = 0;         // Physical address of scratch page
    static uint8_t  g_gpuGen = 0;                  // Cached generation number

    // Framebuffer state
    static uint32_t* g_fbBase = nullptr;           // HHDM virtual address of first FB page
    static uint64_t  g_fbPhysBase = 0;             // Physical address of first FB page
    static uint64_t  g_fbWidth = 0;
    static uint64_t  g_fbHeight = 0;
    static uint64_t  g_fbPitch = 0;                // Stride in bytes
    static uint64_t  g_fbSize = 0;                 // Total framebuffer size in bytes
    static uint64_t  g_fbGttOffset = 0;            // GTT offset where FB starts (in bytes)

    // =========================================================================
    // Register access helpers
    // =========================================================================

    static void WriteReg(uint32_t reg, uint32_t val) {
        *(volatile uint32_t*)(g_mmioBase + reg) = val;
    }

    static uint32_t ReadReg(uint32_t reg) {
        return *(volatile uint32_t*)(g_mmioBase + reg);
    }

    // =========================================================================
    // PCI Detection
    // =========================================================================

    static bool DetectGpu() {
        auto& devices = Pci::GetDevices();
        const Pci::PciDevice* found = nullptr;
        const DeviceInfo* matchedInfo = nullptr;

        for (uint64_t i = 0; i < devices.size(); i++) {
            if (devices[i].VendorId != VendorIntel) {
                continue;
            }
            if (devices[i].ClassCode != ClassDisplay) {
                continue;
            }

            // Found an Intel display controller; try to match device ID
            found = &devices[i];

            for (int j = 0; j < SupportedDeviceCount; j++) {
                if (devices[i].DeviceId == SupportedDevices[j].deviceId) {
                    matchedInfo = &SupportedDevices[j];
                    break;
                }
            }

            // Stop at first Intel display controller
            break;
        }

        if (found == nullptr) {
            KernelLogStream(WARNING, "IntelGPU") << "No Intel display controller found";
            return false;
        }

        g_gpuInfo.pciBus      = found->Bus;
        g_gpuInfo.pciDevice   = found->Device;
        g_gpuInfo.pciFunction = found->Function;
        g_gpuInfo.deviceId    = found->DeviceId;

        if (matchedInfo != nullptr) {
            g_gpuInfo.gen  = matchedInfo->gen;
            g_gpuInfo.name = matchedInfo->name;

            KernelLogStream(OK, "IntelGPU") << "Found " << matchedInfo->name
                << " (device " << base::hex << (uint64_t)found->DeviceId << ")"
                << " at PCI " << (uint64_t)found->Bus << ":"
                << (uint64_t)found->Device << "." << (uint64_t)found->Function;
        } else {
            // Unknown device ID - accept generically but warn
            g_gpuInfo.gen  = 7; // Assume gen 7 as a safe default
            g_gpuInfo.name = "Intel GPU";

            KernelLogStream(WARNING, "IntelGPU") << "Unknown Intel display controller "
                << "(device " << base::hex << (uint64_t)found->DeviceId << ")"
                << " at PCI " << (uint64_t)found->Bus << ":"
                << (uint64_t)found->Device << "." << (uint64_t)found->Function
                << " - attempting generic initialization";
        }

        g_gpuGen = g_gpuInfo.gen;
        return true;
    }

    // =========================================================================
    // BAR0 MMIO Mapping
    // =========================================================================

    static bool MapMmio() {
        uint8_t bus  = g_gpuInfo.pciBus;
        uint8_t dev  = g_gpuInfo.pciDevice;
        uint8_t func = g_gpuInfo.pciFunction;

        // Read BAR0
        uint64_t mmioPhys = Pci::ReadBar0(bus, dev, func);

        g_gpuInfo.mmioPhys = mmioPhys;
        g_gpuInfo.mmioSize = 0x200000; // Map 2MB of MMIO space

        KernelLogStream(INFO, "IntelGPU") << "BAR0 physical: " << base::hex << mmioPhys;

        // Map 2MB of MMIO space (0x200 pages)
        for (uint64_t offset = 0; offset < 0x200000; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }

        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        KernelLogStream(OK, "IntelGPU") << "MMIO mapped at virtual " << base::hex << (uint64_t)g_mmioBase;

        // Enable memory space and bus master
        Pci::EnableBusMaster(bus, dev, func);

        KernelLogStream(OK, "IntelGPU") << "PCI memory space and bus mastering enabled";

        return true;
    }

    // =========================================================================
    // VGA Disable
    // =========================================================================

    static void DisableVga() {
        // Check if VGA plane is already disabled (bit 31 of VGACNTRL).
        // On modern Intel iGPUs with eDP/LVDS panels, the firmware typically
        // disables VGA when it sets up GOP. Skip if already done.
        uint32_t vgaCtrl = ReadReg(VGACNTRL);
        if (vgaCtrl & VGACNTRL_DISABLE) {
            KernelLogStream(INFO, "IntelGPU") << "VGA plane already disabled by firmware";
            return;
        }

        // Step 1: Disable VGA screen via the VGA sequencer I/O ports
        // SR01 bit 5 must be set BEFORE disabling the VGA plane register
        Io::Out8(0x01, 0x3C4);
        uint8_t sr01 = Io::In8(0x3C5);
        sr01 |= (1 << 5);
        Io::Out8(0x01, 0x3C4);
        Io::Out8(sr01, 0x3C5);

        // Step 2: Set bit 31 of VGACNTRL MMIO register to disable VGA display plane
        vgaCtrl |= VGACNTRL_DISABLE;
        WriteReg(VGACNTRL, vgaCtrl);

        // Step 3: Read back to flush the write
        (void)ReadReg(VGACNTRL);

        KernelLogStream(OK, "IntelGPU") << "VGA plane disabled";
    }

    // =========================================================================
    // Read Current Display State
    // =========================================================================

    static bool ReadDisplayState() {
        // Read pipe A configuration
        uint32_t pipeConf = ReadReg(PIPEACONF);
        bool pipeEnabled = (pipeConf & PIPECONF_ENABLE) != 0;

        // Read plane A control
        uint32_t dspaCntr = ReadReg(DSPACNTR);
        bool planeEnabled = (dspaCntr & DISP_ENABLE) != 0;

        // Read current surface address and stride
        uint32_t dspaSurf   = ReadReg(DSPASURF);
        uint32_t dspaStride = ReadReg(DSPASTRIDE);

        // Read timing registers
        uint32_t htotal  = ReadReg(HTOTAL_A);
        uint32_t vtotal  = ReadReg(VTOTAL_A);
        uint32_t pipeSrc = ReadReg(PIPEASRC);

        KernelLogStream(INFO, "IntelGPU") << "Pipe A: "
            << (pipeEnabled ? "ENABLED" : "DISABLED")
            << ", Plane A: " << (planeEnabled ? "ENABLED" : "DISABLED");
        KernelLogStream(INFO, "IntelGPU") << "DSPASURF: " << base::hex << (uint64_t)dspaSurf
            << ", DSPASTRIDE: " << (uint64_t)dspaStride;
        KernelLogStream(INFO, "IntelGPU") << "HTOTAL_A: " << base::hex << (uint64_t)htotal
            << ", VTOTAL_A: " << (uint64_t)vtotal
            << ", PIPEASRC: " << (uint64_t)pipeSrc;

        // Extract resolution from PIPEASRC (preferred) or timing registers
        if (pipeSrc != 0) {
            // PIPEASRC: bits [31:16] = horizontal size - 1, bits [15:0] = vertical size - 1
            g_fbWidth  = ((pipeSrc >> 16) & 0xFFFF) + 1;
            g_fbHeight = (pipeSrc & 0xFFFF) + 1;
        } else if (pipeEnabled) {
            // Fallback to timing registers
            g_fbWidth  = (htotal & 0xFFF) + 1;
            g_fbHeight = (vtotal & 0xFFF) + 1;
        }

        // Read stride from hardware.
        // On Gen 9+ (Skylake and later), PLANE_STRIDE (same offset as DSPASTRIDE)
        // stores the stride in 64-byte units, not bytes. Detect this by checking
        // whether the raw value is too small to be a byte stride.
        if (dspaStride != 0) {
            g_fbPitch = dspaStride;
            if (g_fbWidth > 0 && g_fbPitch < g_fbWidth * 4) {
                KernelLogStream(INFO, "IntelGPU") << "DSPASTRIDE=" << base::dec << g_fbPitch
                    << " is in 64-byte units (Gen 9+), converting to bytes";
                g_fbPitch *= 64;
            }
        }

        // If we still don't have valid dimensions, fall back to the firmware framebuffer
        if (g_fbWidth == 0 || g_fbHeight == 0 || g_fbPitch == 0) {
            g_fbWidth  = ::Graphics::Cursor::GetFramebufferWidth();
            g_fbHeight = ::Graphics::Cursor::GetFramebufferHeight();
            g_fbPitch  = ::Graphics::Cursor::GetFramebufferPitch();

            KernelLogStream(INFO, "IntelGPU") << "Using firmware framebuffer dimensions: "
                << base::dec << g_fbWidth << "x" << g_fbHeight
                << " pitch=" << g_fbPitch;
        } else {
            KernelLogStream(INFO, "IntelGPU") << "Detected resolution: "
                << base::dec << g_fbWidth << "x" << g_fbHeight
                << " pitch=" << g_fbPitch;
        }

        if (g_fbWidth == 0 || g_fbHeight == 0) {
            KernelLogStream(ERROR, "IntelGPU") << "Could not determine display resolution";
            return false;
        }

        // Ensure pitch is at least width * 4 (BGRX8888)
        if (g_fbPitch == 0) {
            g_fbPitch = g_fbWidth * 4;
            KernelLogStream(WARNING, "IntelGPU") << "Stride not available, assuming "
                << base::dec << g_fbPitch << " bytes";
        }

        g_fbSize = g_fbHeight * g_fbPitch;

        KernelLogStream(OK, "IntelGPU") << "Display state: "
            << base::dec << g_fbWidth << "x" << g_fbHeight
            << ", stride=" << g_fbPitch
            << ", FB size=" << g_fbSize << " bytes";

        return true;
    }

    // =========================================================================
    // GTT Initialization
    // =========================================================================

    static bool InitializeGtt() {
        uint8_t bus  = g_gpuInfo.pciBus;
        uint8_t dev  = g_gpuInfo.pciDevice;
        uint8_t func = g_gpuInfo.pciFunction;

        // Read GMCH_CTL to determine GTT size
        uint16_t gmchCtl = Pci::LegacyRead16(bus, dev, func, (uint8_t)PCI_REG_GMCH_CTL);
        uint8_t gttSizeBits = (gmchCtl >> 8) & 0x3;

        uint64_t gttSizeBytes = 0;

        if (g_gpuGen >= 8) {
            // Gen 8+ has a different encoding for GTT size
            switch (gttSizeBits) {
                case 0: gttSizeBytes = 0;          break; // No GTT
                case 1: gttSizeBytes = 2 * 1024 * 1024; break; // 2MB
                case 2: gttSizeBytes = 4 * 1024 * 1024; break; // 4MB
                case 3: gttSizeBytes = 8 * 1024 * 1024; break; // 8MB
                default: gttSizeBytes = 2 * 1024 * 1024; break;
            }
        } else {
            // Gen 5-7
            switch (gttSizeBits) {
                case 0: gttSizeBytes = 0;          break; // No GTT
                case 1: gttSizeBytes = 1024 * 1024; break; // 1MB
                case 2: gttSizeBytes = 2 * 1024 * 1024; break; // 2MB
                case 3: gttSizeBytes = 2 * 1024 * 1024; break; // Depends on gen, default 2MB
                default: gttSizeBytes = 1024 * 1024; break;
            }
        }

        if (gttSizeBytes == 0) {
            // If hardware reports no GTT, assume 1MB as a safe fallback
            gttSizeBytes = 1024 * 1024;
            KernelLogStream(WARNING, "IntelGPU") << "GMCH_CTL reports no GTT, assuming 1MB";
        }

        g_gpuInfo.gttSize = gttSizeBytes;

        KernelLogStream(INFO, "IntelGPU") << "GMCH_CTL: " << base::hex << (uint64_t)gmchCtl
            << ", GTT size: " << base::dec << (gttSizeBytes / 1024) << " KB";

        // The GTT entries reside at BAR0 + 2MB (offset 0x200000)
        // This is correct for most Intel generations
        uint64_t gttPhys = g_gpuInfo.mmioPhys + 0x200000;

        // Map the GTT region (it may overlap with already-mapped MMIO, but we map
        // additional pages beyond the initial 2MB MMIO mapping)
        uint64_t gttMapSize = gttSizeBytes;
        // Ensure we map at least up to the GTT region end
        for (uint64_t offset = 0; offset < gttMapSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(gttPhys + offset, Memory::HHDM(gttPhys + offset));
        }

        g_gttBase = (volatile void*)Memory::HHDM(gttPhys);

        // Calculate number of GTT entries
        if (g_gpuGen >= 8) {
            // 64-bit PTEs
            g_gttEntryCount = gttSizeBytes / sizeof(uint64_t);
        } else {
            // 32-bit PTEs
            g_gttEntryCount = gttSizeBytes / sizeof(uint32_t);
        }

        KernelLogStream(INFO, "IntelGPU") << "GTT at physical " << base::hex << gttPhys
            << ", " << base::dec << g_gttEntryCount << " entries"
            << (g_gpuGen >= 8 ? " (64-bit PTEs)" : " (32-bit PTEs)");

        // Allocate a scratch page (zeroed) for future use
        void* scratchPageVirt = Memory::g_pfa->AllocateZeroed();
        g_scratchPagePhys = Memory::SubHHDM(scratchPageVirt);

        // Do NOT clear the entire GTT here. The firmware has active GTT mappings
        // that the display engine is currently scanning out from. Clearing them
        // would cause the display to go black (or worse) before we remap.
        // Instead, we only write the entries we need in SetupFramebuffer().

        KernelLogStream(OK, "IntelGPU") << "GTT ready: " << base::dec << g_gttEntryCount
            << " entries, scratch page at " << base::hex << g_scratchPagePhys;

        return true;
    }

    // =========================================================================
    // Framebuffer Allocation and Setup
    // =========================================================================

    static bool SetupFramebuffer() {
        // Map the firmware framebuffer's contiguous physical pages through our GTT.
        // This keeps the same physical memory that the firmware set up (contiguous
        // pages, already HHDM-mapped), so both kernel and userspace access continue
        // to work via the original virtual/physical addresses. No copy is needed.
        uint32_t* fwFb = ::Graphics::Cursor::GetFramebufferBase();
        if (fwFb == nullptr) {
            KernelLogStream(ERROR, "IntelGPU") << "No firmware framebuffer available";
            return false;
        }

        uint64_t fwFbPhys = Memory::SubHHDM((uint64_t)fwFb);
        uint64_t pageCount = (g_fbSize + 0xFFF) / 0x1000;

        if (pageCount > g_gttEntryCount) {
            KernelLogStream(ERROR, "IntelGPU") << "Framebuffer requires " << base::dec << pageCount
                << " pages but GTT only has " << g_gttEntryCount << " entries";
            return false;
        }

        KernelLogStream(INFO, "IntelGPU") << "Mapping " << base::dec << pageCount
            << " firmware FB pages through GTT (phys base " << base::hex << fwFbPhys << ")";

        // Program GTT entries to point to the firmware FB's contiguous physical pages
        if (g_gpuGen >= 8) {
            volatile uint64_t* gtt64 = (volatile uint64_t*)g_gttBase;
            for (uint64_t i = 0; i < pageCount; i++) {
                gtt64[i] = MakeGttPte64(fwFbPhys + i * 0x1000);
            }
            // Flush GTT writes
            (void)gtt64[pageCount - 1];
        } else {
            volatile uint32_t* gtt32 = (volatile uint32_t*)g_gttBase;
            for (uint64_t i = 0; i < pageCount; i++) {
                gtt32[i] = MakeGttPte32(fwFbPhys + i * 0x1000);
            }
            // Flush GTT writes
            (void)gtt32[pageCount - 1];
        }

        // Keep using the same framebuffer memory
        g_fbBase = fwFb;
        g_fbPhysBase = fwFbPhys;
        g_fbGttOffset = 0; // Starting at GTT entry 0 => offset 0

        KernelLogStream(OK, "IntelGPU") << "Framebuffer mapped through GTT: " << base::dec << pageCount
            << " pages, phys=" << base::hex << fwFbPhys;

        return true;
    }

    static void ProgramDisplayPlane() {
        // Preserve the firmware's DSPACNTR value entirely. The firmware already
        // configured the correct pixel format, pipe assignment, and tiling mode.
        // We only need to point DSPASURF to our GTT-mapped framebuffer.
        uint32_t dspaCntr = ReadReg(DSPACNTR);
        uint32_t fmtBits = (dspaCntr & DISP_FORMAT_MASK) >> DISP_FORMAT_SHIFT;
        uint32_t oldDspaSurf = ReadReg(DSPASURF);

        KernelLogStream(INFO, "IntelGPU") << "Preserving firmware DSPACNTR: " << base::hex
            << (uint64_t)dspaCntr << " (format=" << (uint64_t)fmtBits << ")";
        KernelLogStream(INFO, "IntelGPU") << "Firmware DSPASURF was: " << base::hex
            << (uint64_t)oldDspaSurf;

        // Ensure the plane is enabled (it should already be)
        if (!(dspaCntr & DISP_ENABLE)) {
            dspaCntr |= DISP_ENABLE;
            WriteReg(DSPACNTR, dspaCntr);
        }

        // Do NOT write DSPASTRIDE — the firmware already set the correct value
        // in the hardware's native format (bytes on Gen <9, 64-byte units on Gen 9+).
        // Writing our byte-converted g_fbPitch would corrupt it on Gen 9+.

        // Write the GTT base offset to DSPASURF - this triggers the plane update
        // Since we mapped at GTT entry 0, the offset is 0
        uint32_t surfAddr = (uint32_t)g_fbGttOffset;
        WriteReg(DSPASURF, surfAddr);

        // Read back to flush
        (void)ReadReg(DSPASURF);

        KernelLogStream(OK, "IntelGPU") << "Display plane A: DSPASURF="
            << base::hex << (uint64_t)surfAddr
            << " (was " << (uint64_t)oldDspaSurf << ")"
            << ", stride=" << base::dec << g_fbPitch;
    }

    // =========================================================================
    // Public API
    // =========================================================================

    // =========================================================================
    // Probe (called by PCI driver matching framework)
    // =========================================================================

    bool Probe(const Pci::PciDevice& dev) {
        if (g_initialized) return false;

        // Fill in PCI location
        g_gpuInfo.pciBus      = dev.Bus;
        g_gpuInfo.pciDevice   = dev.Device;
        g_gpuInfo.pciFunction = dev.Function;
        g_gpuInfo.deviceId    = dev.DeviceId;

        // Match device ID against supported table
        const DeviceInfo* matchedInfo = nullptr;
        for (int j = 0; j < SupportedDeviceCount; j++) {
            if (dev.DeviceId == SupportedDevices[j].deviceId) {
                matchedInfo = &SupportedDevices[j];
                break;
            }
        }

        if (matchedInfo != nullptr) {
            g_gpuInfo.gen  = matchedInfo->gen;
            g_gpuInfo.name = matchedInfo->name;
            KernelLogStream(OK, "IntelGPU") << "Found " << matchedInfo->name
                << " (device " << base::hex << (uint64_t)dev.DeviceId << ")"
                << " at PCI " << (uint64_t)dev.Bus << ":"
                << (uint64_t)dev.Device << "." << (uint64_t)dev.Function;
        } else {
            g_gpuInfo.gen  = 7;
            g_gpuInfo.name = "Intel GPU";
            KernelLogStream(WARNING, "IntelGPU") << "Unknown Intel display controller "
                << "(device " << base::hex << (uint64_t)dev.DeviceId << ")"
                << " at PCI " << (uint64_t)dev.Bus << ":"
                << (uint64_t)dev.Device << "." << (uint64_t)dev.Function
                << " - attempting generic initialization";
        }

        g_gpuGen = g_gpuInfo.gen;

        if (!MapMmio()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to map MMIO region";
            return false;
        }

        DisableVga();

        if (!ReadDisplayState()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to read display state";
            return false;
        }

        if (!InitializeGtt()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to initialize GTT";
            return false;
        }

        if (!SetupFramebuffer()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to set up framebuffer";
            return false;
        }

        ProgramDisplayPlane();
        g_initialized = true;

        uint64_t fwWidth  = ::Graphics::Cursor::GetFramebufferWidth();
        uint64_t fwHeight = ::Graphics::Cursor::GetFramebufferHeight();
        uint64_t fwPitch  = ::Graphics::Cursor::GetFramebufferPitch();

        if (g_fbWidth != fwWidth || g_fbHeight != fwHeight || g_fbPitch != fwPitch) {
            KernelLogStream(WARNING, "IntelGPU") << "GPU dimensions differ from firmware!";
            KernelLogStream(WARNING, "IntelGPU") << "  GPU:      "
                << base::dec << g_fbWidth << "x" << g_fbHeight << " pitch=" << g_fbPitch;
            KernelLogStream(WARNING, "IntelGPU") << "  Firmware: "
                << base::dec << fwWidth << "x" << fwHeight << " pitch=" << fwPitch;
        }

        KernelLogStream(OK, "IntelGPU") << "Initialization complete: "
            << base::dec << g_fbWidth << "x" << g_fbHeight
            << " @ " << base::hex << (uint64_t)g_fbBase;
        return true;
    }

    // =========================================================================
    // Initialize (standalone fallback path)
    // =========================================================================

    void Initialize() {
        KernelLogStream(INFO, "IntelGPU") << "Scanning for Intel integrated graphics...";

        // Step 1: Detect GPU on PCI bus
        if (!DetectGpu()) {
            return;
        }

        // Step 2: Map BAR0 MMIO region
        if (!MapMmio()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to map MMIO region";
            return;
        }

        // Step 3: Disable VGA plane
        DisableVga();

        // Step 4: Read current display state from firmware
        if (!ReadDisplayState()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to read display state";
            return;
        }

        // Step 5: Initialize GTT
        if (!InitializeGtt()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to initialize GTT";
            return;
        }

        // Step 6: Map firmware framebuffer pages through GTT
        if (!SetupFramebuffer()) {
            KernelLogStream(ERROR, "IntelGPU") << "Failed to set up framebuffer";
            return;
        }

        // Step 7: Program the display plane to use our GTT-mapped framebuffer
        ProgramDisplayPlane();

        g_initialized = true;

        // Diagnostic: compare GPU-detected values with firmware/Limine values
        uint64_t fwWidth  = ::Graphics::Cursor::GetFramebufferWidth();
        uint64_t fwHeight = ::Graphics::Cursor::GetFramebufferHeight();
        uint64_t fwPitch  = ::Graphics::Cursor::GetFramebufferPitch();

        if (g_fbWidth != fwWidth || g_fbHeight != fwHeight || g_fbPitch != fwPitch) {
            KernelLogStream(WARNING, "IntelGPU") << "GPU dimensions differ from firmware!";
            KernelLogStream(WARNING, "IntelGPU") << "  GPU:      "
                << base::dec << g_fbWidth << "x" << g_fbHeight
                << " pitch=" << g_fbPitch;
            KernelLogStream(WARNING, "IntelGPU") << "  Firmware: "
                << base::dec << fwWidth << "x" << fwHeight
                << " pitch=" << fwPitch;
        }

        KernelLogStream(OK, "IntelGPU") << "Initialization complete: "
            << base::dec << g_fbWidth << "x" << g_fbHeight
            << " @ " << base::hex << (uint64_t)g_fbBase;
    }

    bool IsInitialized() {
        return g_initialized;
    }

    const GpuInfo* GetGpuInfo() {
        return &g_gpuInfo;
    }

    uint32_t* GetFramebufferBase() {
        return g_fbBase;
    }

    uint64_t GetFramebufferPhysBase() {
        return g_fbPhysBase;
    }

    uint64_t GetWidth() {
        return g_fbWidth;
    }

    uint64_t GetHeight() {
        return g_fbHeight;
    }

    uint64_t GetPitch() {
        return g_fbPitch;
    }

};
