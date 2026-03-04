/*
    * IntelGPU.hpp
    * Intel integrated graphics (i915) modesetting driver
    * Supports Gen 5 (Ironlake) through Gen 12 (Tiger Lake / Alder Lake)
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Pci/Pci.hpp>

namespace Drivers::Graphics::IntelGPU {

    // =========================================================================
    // PCI identification
    // =========================================================================

    static constexpr uint16_t VendorIntel = 0x8086;
    static constexpr uint8_t  ClassDisplay = 0x03;
    static constexpr uint8_t  SubclassVGA  = 0x00;

    // PCI config space offsets (GPU-specific, not in shared Pci::)
    static constexpr uint16_t PCI_REG_BAR2    = 0x18;
    static constexpr uint16_t PCI_REG_GMCH_CTL = 0x50;

    // Supported Intel GPU device IDs (representative subset)
    struct DeviceInfo {
        uint16_t deviceId;
        uint8_t  gen;       // Intel graphics generation (5-12)
        const char* name;
    };

    static constexpr DeviceInfo SupportedDevices[] = {
        // Gen 5 - Ironlake
        {0x0042, 5, "Ironlake Desktop"},
        {0x0046, 5, "Ironlake Mobile"},

        // Gen 6 - Sandy Bridge
        {0x0102, 6, "Sandy Bridge GT1 Desktop"},
        {0x0112, 6, "Sandy Bridge GT2 Desktop"},
        {0x0122, 6, "Sandy Bridge GT2 Desktop"},
        {0x0106, 6, "Sandy Bridge GT1 Mobile"},
        {0x0116, 6, "Sandy Bridge GT2 Mobile"},
        {0x0126, 6, "Sandy Bridge GT2 Mobile"},
        {0x010A, 6, "Sandy Bridge GT1 Server"},

        // Gen 7 - Ivy Bridge
        {0x0152, 7, "Ivy Bridge GT1 Desktop"},
        {0x0162, 7, "Ivy Bridge GT2 Desktop"},
        {0x0156, 7, "Ivy Bridge GT1 Mobile"},
        {0x0166, 7, "Ivy Bridge GT2 Mobile"},
        {0x015A, 7, "Ivy Bridge GT1 Server"},
        {0x016A, 7, "Ivy Bridge GT2 Server"},

        // Gen 7.5 - Haswell
        {0x0402, 7, "Haswell GT1 Desktop"},
        {0x0412, 7, "Haswell GT2 Desktop"},
        {0x0422, 7, "Haswell GT3 Desktop"},
        {0x0406, 7, "Haswell GT1 Mobile"},
        {0x0416, 7, "Haswell GT2 Mobile"},
        {0x0426, 7, "Haswell GT3 Mobile"},
        {0x0A06, 7, "Haswell ULT GT1"},
        {0x0A16, 7, "Haswell ULT GT2"},
        {0x0A26, 7, "Haswell ULT GT3"},
        {0x0D12, 7, "Haswell CRW GT2"},
        {0x0D22, 7, "Haswell CRW GT3"},

        // Gen 8 - Broadwell
        {0x1602, 8, "Broadwell GT1"},
        {0x1612, 8, "Broadwell GT2"},
        {0x1616, 8, "Broadwell GT2 Mobile"},
        {0x1622, 8, "Broadwell GT3"},
        {0x1626, 8, "Broadwell GT3 Mobile"},
        {0x162A, 8, "Broadwell GT3 Server"},

        // Gen 9 - Skylake
        {0x1902, 9, "Skylake GT1 Desktop"},
        {0x1906, 9, "Skylake GT1 Mobile"},
        {0x1912, 9, "Skylake GT2 Desktop"},
        {0x1916, 9, "Skylake GT2 Mobile"},
        {0x191E, 9, "Skylake GT2 Mobile"},
        {0x1926, 9, "Skylake GT3 Mobile"},
        {0x1932, 9, "Skylake GT4 Desktop"},

        // Gen 9.5 - Kaby Lake / Coffee Lake
        {0x5902, 9, "Kaby Lake GT1 Desktop"},
        {0x5912, 9, "Kaby Lake GT2 Desktop"},
        {0x5916, 9, "Kaby Lake GT2 Mobile"},
        {0x5926, 9, "Kaby Lake GT3 Mobile"},
        {0x3E90, 9, "Coffee Lake GT1 Desktop"},
        {0x3E92, 9, "Coffee Lake GT2 Desktop"},
        {0x3EA0, 9, "Coffee Lake GT3"},
        {0x3E91, 9, "Coffee Lake GT2 Desktop"},
        {0x3E98, 9, "Coffee Lake GT2 Desktop"},
        {0x9B41, 9, "Comet Lake GT2"},
        {0x9BA5, 9, "Comet Lake GT2 Mobile"},

        // Gen 11 - Ice Lake
        {0x8A52, 11, "Ice Lake GT2"},
        {0x8A56, 11, "Ice Lake GT2 Mobile"},
        {0x8A5A, 11, "Ice Lake GT1.5"},
        {0x8A5C, 11, "Ice Lake GT1"},

        // Gen 12 - Tiger Lake
        {0x9A49, 12, "Tiger Lake GT2"},
        {0x9A78, 12, "Tiger Lake GT2"},
        {0x9A40, 12, "Tiger Lake GT2"},

        // Gen 12 - Alder Lake
        {0x4626, 12, "Alder Lake GT2"},
        {0x4680, 12, "Alder Lake-S GT1"},
        {0x4692, 12, "Alder Lake-S GT1"},
        {0x46A6, 12, "Alder Lake-P GT2"},
    };

    static constexpr int SupportedDeviceCount = sizeof(SupportedDevices) / sizeof(SupportedDevices[0]);

    // =========================================================================
    // MMIO register offsets (relative to BAR0)
    // =========================================================================

    // --- VGA control ---
    static constexpr uint32_t VGACNTRL = 0x71400;
    static constexpr uint32_t VGACNTRL_DISABLE = (1u << 31);

    // --- DPLL (Display PLL) ---
    static constexpr uint32_t DPLL_A      = 0x06014;
    static constexpr uint32_t DPLL_B      = 0x06018;
    static constexpr uint32_t FPA0        = 0x06040;
    static constexpr uint32_t FPA1        = 0x06044;
    static constexpr uint32_t FPB0        = 0x06048;
    static constexpr uint32_t FPB1        = 0x0604C;

    // DPLL control bits
    static constexpr uint32_t DPLL_VCO_ENABLE   = (1u << 31);
    static constexpr uint32_t DPLL_VGA_MODE_DIS  = (1u << 28);
    static constexpr uint32_t DPLL_MODE_DAC_SDVO = (1u << 26);
    static constexpr uint32_t DPLL_MODE_LVDS     = (2u << 26);

    // FP register fields
    static constexpr uint32_t FP_N_DIV_SHIFT  = 16;
    static constexpr uint32_t FP_N_DIV_MASK   = 0x3F0000;
    static constexpr uint32_t FP_M1_DIV_SHIFT = 8;
    static constexpr uint32_t FP_M1_DIV_MASK  = 0x003F00;
    static constexpr uint32_t FP_M2_DIV_SHIFT = 0;
    static constexpr uint32_t FP_M2_DIV_MASK  = 0x00003F;

    // --- Display timing registers (Pipe A) ---
    static constexpr uint32_t HTOTAL_A   = 0x60000;
    static constexpr uint32_t HBLANK_A   = 0x60004;
    static constexpr uint32_t HSYNC_A    = 0x60008;
    static constexpr uint32_t VTOTAL_A   = 0x6000C;
    static constexpr uint32_t VBLANK_A   = 0x60010;
    static constexpr uint32_t VSYNC_A    = 0x60014;
    static constexpr uint32_t PIPEASRC   = 0x6001C;

    // --- Display timing registers (Pipe B) ---
    static constexpr uint32_t HTOTAL_B   = 0x61000;
    static constexpr uint32_t HBLANK_B   = 0x61004;
    static constexpr uint32_t HSYNC_B    = 0x61008;
    static constexpr uint32_t VTOTAL_B   = 0x6100C;
    static constexpr uint32_t VBLANK_B   = 0x61010;
    static constexpr uint32_t VSYNC_B    = 0x61014;
    static constexpr uint32_t PIPEBSRC   = 0x6101C;

    // --- Pipe configuration ---
    static constexpr uint32_t PIPEACONF  = 0x70008;
    static constexpr uint32_t PIPEBCONF  = 0x71008;

    static constexpr uint32_t PIPECONF_ENABLE   = (1u << 31);
    static constexpr uint32_t PIPECONF_STATE    = (1u << 30);
    static constexpr uint32_t PIPECONF_8BPC     = (0u << 5);
    static constexpr uint32_t PIPECONF_10BPC    = (1u << 5);
    static constexpr uint32_t PIPECONF_6BPC     = (2u << 5);
    static constexpr uint32_t PIPECONF_12BPC    = (3u << 5);

    // --- Display plane control (Plane A, pre-Skylake i9xx-style) ---
    static constexpr uint32_t DSPACNTR   = 0x70180;
    static constexpr uint32_t DSPALINOFF = 0x70184;
    static constexpr uint32_t DSPASTRIDE = 0x70188;
    static constexpr uint32_t DSPAPOS    = 0x7018C;
    static constexpr uint32_t DSPASIZE   = 0x70190;
    static constexpr uint32_t DSPASURF   = 0x7019C;
    static constexpr uint32_t DSPATILEOFF = 0x701A4;

    // --- Display plane control (Plane B) ---
    static constexpr uint32_t DSPBCNTR   = 0x71180;
    static constexpr uint32_t DSPBLINOFF = 0x71184;
    static constexpr uint32_t DSPBSTRIDE = 0x71188;
    static constexpr uint32_t DSPBPOS    = 0x7118C;
    static constexpr uint32_t DSPBSIZE   = 0x71190;
    static constexpr uint32_t DSPBSURF   = 0x7119C;
    static constexpr uint32_t DSPBTILEOFF = 0x711A4;

    // DSPCNTR bits
    static constexpr uint32_t DISP_ENABLE         = (1u << 31);
    static constexpr uint32_t DISP_GAMMA_ENABLE   = (1u << 30);
    static constexpr uint32_t DISP_FORMAT_SHIFT   = 26;
    static constexpr uint32_t DISP_FORMAT_MASK    = (0xFu << 26);
    static constexpr uint32_t DISP_FORMAT_BGRX8888 = (0x6u << 26);  // 32bpp BGRX (no alpha)
    static constexpr uint32_t DISP_FORMAT_BGRA8888 = (0x7u << 26);  // 32bpp BGRA (with alpha)
    static constexpr uint32_t DISP_FORMAT_RGBX8888 = (0xEu << 26);  // 32bpp RGBX
    static constexpr uint32_t DISP_FORMAT_BGR565   = (0x5u << 26);  // 16bpp BGR 5:6:5
    static constexpr uint32_t DISP_FORMAT_BGRX1010102 = (0xAu << 26);
    static constexpr uint32_t DISP_PIPE_B_SELECT   = (1u << 24);
    static constexpr uint32_t DISP_TILED           = (1u << 10);

    // --- Cursor plane (Pipe A) ---
    static constexpr uint32_t CURACNTR   = 0x70080;
    static constexpr uint32_t CURABASE   = 0x70084;
    static constexpr uint32_t CURAPOS    = 0x70088;

    // --- Output connectors ---
    static constexpr uint32_t ADPA       = 0x61100;  // Analog Display Port (VGA/CRT)
    static constexpr uint32_t DVOB       = 0x61140;  // DVO-B
    static constexpr uint32_t DVOC       = 0x61160;  // DVO-C
    static constexpr uint32_t LVDS       = 0x61180;  // LVDS panel
    static constexpr uint32_t DP_B       = 0x64100;  // DisplayPort B
    static constexpr uint32_t DP_C       = 0x64200;  // DisplayPort C
    static constexpr uint32_t DP_D       = 0x64300;  // DisplayPort D
    static constexpr uint32_t HDMI_B     = 0x61140;  // HDMI-B (same as DVOB on some gens)
    static constexpr uint32_t HDMI_C     = 0x61160;  // HDMI-C (same as DVOC on some gens)

    // ADPA bits
    static constexpr uint32_t ADPA_DAC_ENABLE    = (1u << 31);
    static constexpr uint32_t ADPA_PIPE_B_SELECT = (1u << 30);
    static constexpr uint32_t ADPA_HSYNC_ACTIVE_LOW = (1u << 3);
    static constexpr uint32_t ADPA_VSYNC_ACTIVE_LOW = (1u << 4);

    // LVDS bits
    static constexpr uint32_t LVDS_PORT_ENABLE   = (1u << 31);
    static constexpr uint32_t LVDS_PIPE_B_SELECT = (1u << 30);

    // --- GMBUS (I2C for EDID) ---
    static constexpr uint32_t GMBUS0     = 0x5100;   // Clock/port select
    static constexpr uint32_t GMBUS1     = 0x5104;   // Command/status
    static constexpr uint32_t GMBUS2     = 0x5108;   // Status
    static constexpr uint32_t GMBUS3     = 0x510C;   // Data buffer
    static constexpr uint32_t GMBUS4     = 0x5110;   // Interrupt mask
    static constexpr uint32_t GMBUS5     = 0x5120;   // 2-byte index

    // --- Hardware status page ---
    static constexpr uint32_t HWS_PGA    = 0x02080;

    // --- Fence registers (tiling) ---
    static constexpr uint32_t FENCE_REG_BASE = 0x02000;  // Gen 2-3
    static constexpr uint32_t FENCE_REG_965_BASE = 0x03000;  // Gen 4+

    // =========================================================================
    // GTT (Graphics Translation Table)
    // =========================================================================

    // GTT PTE format for Gen 6/7 (Sandy Bridge through Haswell) - 32-bit entries
    // Bits [31:12] = physical page address [31:12]
    // Bits [10:4]  = physical page address [38:32]
    // Bits [3:1]   = cacheability
    // Bit  [0]     = valid
    static constexpr uint32_t GTT_PTE_VALID     = (1u << 0);
    static constexpr uint32_t GTT_PTE_WB_LLC    = (3u << 1);  // Write-back LLC cache
    static constexpr uint32_t GTT_PTE_UNCACHED  = (0u << 1);

    // Gen 8+ uses 64-bit GTT PTEs
    static constexpr uint64_t GTT_PTE64_VALID   = (1ULL << 0);

    // Helper: Build a Gen 6/7 GTT PTE from a physical address
    static inline uint32_t MakeGttPte32(uint64_t physAddr) {
        uint32_t pte = (uint32_t)(physAddr & 0xFFFFF000u);       // bits [31:12]
        pte |= (uint32_t)((physAddr >> 28) & 0x7F0u);           // bits [38:32] -> PTE [10:4]
        pte |= GTT_PTE_VALID;
        return pte;
    }

    // Helper: Build a Gen 8+ GTT PTE from a physical address
    static inline uint64_t MakeGttPte64(uint64_t physAddr) {
        return (physAddr & ~0xFFFULL) | GTT_PTE64_VALID;
    }

    // =========================================================================
    // DPLL clock calculation structures
    // =========================================================================

    struct DpllParams {
        uint32_t n;
        uint32_t m1;
        uint32_t m2;
        uint32_t p1;
        uint32_t p2;
    };

    // DPLL parameter limits (Gen 6 / Sandy Bridge)
    struct DpllLimits {
        uint32_t nMin, nMax;
        uint32_t m1Min, m1Max;
        uint32_t m2Min, m2Max;
        uint32_t p1Min, p1Max;
        uint32_t p2Slow, p2Fast;    // P2 values for slow/fast pixel clocks
        uint32_t p2Threshold;        // kHz threshold between slow/fast P2
        uint32_t vcoMin, vcoMax;     // VCO range in kHz
        uint32_t refClock;           // Reference clock in kHz
    };

    // Sandy Bridge / Ivy Bridge DAC/SDVO limits
    static constexpr DpllLimits SNB_DAC_LIMITS = {
        .nMin = 1, .nMax = 5,
        .m1Min = 12, .m1Max = 22,
        .m2Min = 5, .m2Max = 9,
        .p1Min = 1, .p1Max = 8,
        .p2Slow = 10, .p2Fast = 5,
        .p2Threshold = 225000,
        .vcoMin = 1750000, .vcoMax = 3500000,
        .refClock = 120000,
    };

    // Sandy Bridge / Ivy Bridge LVDS limits
    static constexpr DpllLimits SNB_LVDS_LIMITS = {
        .nMin = 1, .nMax = 3,
        .m1Min = 12, .m1Max = 22,
        .m2Min = 5, .m2Max = 9,
        .p1Min = 1, .p1Max = 8,
        .p2Slow = 14, .p2Fast = 7,
        .p2Threshold = 225000,
        .vcoMin = 1750000, .vcoMax = 3500000,
        .refClock = 120000,
    };

    // =========================================================================
    // Display mode timing
    // =========================================================================

    struct DisplayMode {
        uint32_t hdisplay;          // Horizontal active pixels
        uint32_t hsyncStart;
        uint32_t hsyncEnd;
        uint32_t htotal;
        uint32_t vdisplay;          // Vertical active lines
        uint32_t vsyncStart;
        uint32_t vsyncEnd;
        uint32_t vtotal;
        uint32_t pixelClock;        // in kHz
        bool     hsyncPositive;
        bool     vsyncPositive;
    };

    // Common mode timings (CVT/DMT standard)
    static constexpr DisplayMode MODE_1920x1080_60 = {
        .hdisplay = 1920, .hsyncStart = 2008, .hsyncEnd = 2052, .htotal = 2200,
        .vdisplay = 1080, .vsyncStart = 1084, .vsyncEnd = 1089, .vtotal = 1125,
        .pixelClock = 148500,
        .hsyncPositive = true, .vsyncPositive = true,
    };

    static constexpr DisplayMode MODE_1280x720_60 = {
        .hdisplay = 1280, .hsyncStart = 1390, .hsyncEnd = 1430, .htotal = 1650,
        .vdisplay = 720, .vsyncStart = 725, .vsyncEnd = 730, .vtotal = 750,
        .pixelClock = 74250,
        .hsyncPositive = true, .vsyncPositive = true,
    };

    static constexpr DisplayMode MODE_1024x768_60 = {
        .hdisplay = 1024, .hsyncStart = 1048, .hsyncEnd = 1184, .htotal = 1344,
        .vdisplay = 768, .vsyncStart = 771, .vsyncEnd = 777, .vtotal = 806,
        .pixelClock = 65000,
        .hsyncPositive = false, .vsyncPositive = false,
    };

    static constexpr DisplayMode MODE_800x600_60 = {
        .hdisplay = 800, .hsyncStart = 840, .hsyncEnd = 968, .htotal = 1056,
        .vdisplay = 600, .vsyncStart = 601, .vsyncEnd = 605, .vtotal = 628,
        .pixelClock = 40000,
        .hsyncPositive = true, .vsyncPositive = true,
    };

    // =========================================================================
    // Detected GPU information
    // =========================================================================

    struct GpuInfo {
        uint16_t deviceId;
        uint8_t  gen;
        const char* name;
        uint8_t  pciBus;
        uint8_t  pciDevice;
        uint8_t  pciFunction;
        uint64_t mmioPhys;          // BAR0 physical address
        uint64_t mmioSize;          // BAR0 region size
        uint64_t gmadrPhys;         // BAR2 physical (aperture)
        uint64_t gttSize;           // GTT size in bytes
        uint32_t stolenMb;          // Stolen memory in MB
    };

    // =========================================================================
    // Public API
    // =========================================================================

    // Initialize the Intel GPU driver (scans PCI, maps MMIO, sets up GTT + FB)
    void Initialize();

    // Probe a specific PCI device (called by PCI driver matching framework)
    bool Probe(const Pci::PciDevice& dev);

    // Check if an Intel GPU was found and initialized
    bool IsInitialized();

    // Get detected GPU information
    const GpuInfo* GetGpuInfo();

    // Framebuffer access (returns HHDM virtual address)
    uint32_t* GetFramebufferBase();

    // Framebuffer physical base (for userspace mapping)
    uint64_t GetFramebufferPhysBase();

    // Framebuffer dimensions
    uint64_t GetWidth();
    uint64_t GetHeight();
    uint64_t GetPitch();

};
