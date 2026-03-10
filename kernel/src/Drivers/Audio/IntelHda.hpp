/*
    * IntelHda.hpp
    * Intel High Definition Audio controller driver
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Pci/Pci.hpp>

namespace Drivers::Audio::IntelHda {

    // =========================================================================
    // HDA controller registers (memory-mapped via BAR0)
    // Ref: Intel High Definition Audio Specification, Rev 1.0a
    // =========================================================================

    // Global registers
    constexpr uint32_t REG_GCAP       = 0x00;  // Global Capabilities (16-bit)
    constexpr uint32_t REG_VMIN       = 0x02;  // Minor Version (8-bit)
    constexpr uint32_t REG_VMAJ       = 0x03;  // Major Version (8-bit)
    constexpr uint32_t REG_OUTPAY     = 0x04;  // Output Payload Capability (16-bit)
    constexpr uint32_t REG_INPAY      = 0x06;  // Input Payload Capability (16-bit)
    constexpr uint32_t REG_GCTL       = 0x08;  // Global Control (32-bit)
    constexpr uint32_t REG_WAKEEN     = 0x0C;  // Wake Enable (16-bit)
    constexpr uint32_t REG_STATESTS   = 0x0E;  // State Change Status (16-bit)
    constexpr uint32_t REG_GSTS       = 0x10;  // Global Status (16-bit)
    constexpr uint32_t REG_INTCTL     = 0x20;  // Interrupt Control (32-bit)
    constexpr uint32_t REG_INTSTS     = 0x24;  // Interrupt Status (32-bit)
    constexpr uint32_t REG_WALCLK     = 0x30;  // Wall Clock Counter (32-bit)
    constexpr uint32_t REG_SSYNC      = 0x38;  // Stream Synchronization (32-bit)

    // CORB registers
    constexpr uint32_t REG_CORBLBASE  = 0x40;  // CORB Lower Base Address (32-bit)
    constexpr uint32_t REG_CORBUBASE  = 0x44;  // CORB Upper Base Address (32-bit)
    constexpr uint32_t REG_CORBWP     = 0x48;  // CORB Write Pointer (16-bit)
    constexpr uint32_t REG_CORBRP     = 0x4A;  // CORB Read Pointer (16-bit)
    constexpr uint32_t REG_CORBCTL    = 0x4C;  // CORB Control (8-bit)
    constexpr uint32_t REG_CORBSTS    = 0x4D;  // CORB Status (8-bit)
    constexpr uint32_t REG_CORBSIZE   = 0x4E;  // CORB Size (8-bit)

    // RIRB registers
    constexpr uint32_t REG_RIRBLBASE  = 0x50;  // RIRB Lower Base Address (32-bit)
    constexpr uint32_t REG_RIRBUBASE  = 0x54;  // RIRB Upper Base Address (32-bit)
    constexpr uint32_t REG_RIRBWP     = 0x58;  // RIRB Write Pointer (16-bit)
    constexpr uint32_t REG_RINTCNT    = 0x5A;  // Response Interrupt Count (16-bit)
    constexpr uint32_t REG_RIRBCTL    = 0x5C;  // RIRB Control (8-bit)
    constexpr uint32_t REG_RIRBSTS    = 0x5D;  // RIRB Status (8-bit)
    constexpr uint32_t REG_RIRBSIZE   = 0x5E;  // RIRB Size (8-bit)

    // Immediate command interface
    constexpr uint32_t REG_ICW        = 0x60;  // Immediate Command Write (32-bit)
    constexpr uint32_t REG_IRR        = 0x64;  // Immediate Response Read (32-bit)
    constexpr uint32_t REG_ICS        = 0x68;  // Immediate Command Status (16-bit)

    // DMA Position Buffer
    constexpr uint32_t REG_DPIBLBASE  = 0x70;  // DMA Position Lower Base (32-bit)
    constexpr uint32_t REG_DPIBUBASE  = 0x74;  // DMA Position Upper Base (32-bit)

    // GCTL register bits
    constexpr uint32_t GCTL_CRST      = (1u << 0);   // Controller Reset
    constexpr uint32_t GCTL_FCNTRL    = (1u << 1);   // Flush Control
    constexpr uint32_t GCTL_UNSOL     = (1u << 8);   // Accept Unsolicited Responses

    // INTCTL register bits
    constexpr uint32_t INTCTL_GIE     = (1u << 31);  // Global Interrupt Enable
    constexpr uint32_t INTCTL_CIE     = (1u << 30);  // Controller Interrupt Enable

    // CORBCTL register bits
    constexpr uint8_t CORBCTL_RUN     = (1u << 1);   // CORB DMA Engine Run
    constexpr uint8_t CORBCTL_MEIE    = (1u << 0);   // Memory Error Interrupt Enable

    // RIRBCTL register bits
    constexpr uint8_t RIRBCTL_RUN     = (1u << 1);   // RIRB DMA Engine Run
    constexpr uint8_t RIRBCTL_RINTCTL = (1u << 0);   // Response Interrupt Control

    // RIRBSTS register bits
    constexpr uint8_t RIRBSTS_RINTFL  = (1u << 0);   // Response Interrupt

    // CORBRP register bits
    constexpr uint16_t CORBRP_RST     = (1u << 15);  // CORB Read Pointer Reset

    // RIRBWP register bits
    constexpr uint16_t RIRBWP_RST     = (1u << 15);  // RIRB Write Pointer Reset

    // =========================================================================
    // Stream Descriptor registers (offset = 0x80 + streamIndex * 0x20)
    // =========================================================================

    constexpr uint32_t SD_BASE        = 0x80;
    constexpr uint32_t SD_SIZE        = 0x20;

    // Stream descriptor register offsets (relative to stream base)
    constexpr uint32_t SD_CTL         = 0x00;  // Control (24-bit: bytes 0,1,2)
    constexpr uint32_t SD_STS         = 0x03;  // Status (8-bit)
    constexpr uint32_t SD_LPIB        = 0x04;  // Link Position in Current Buffer (32-bit)
    constexpr uint32_t SD_CBL         = 0x08;  // Cyclic Buffer Length (32-bit)
    constexpr uint32_t SD_LVI         = 0x0C;  // Last Valid Index (16-bit)
    constexpr uint32_t SD_FIFOS       = 0x10;  // FIFO Size (16-bit, read-only)
    constexpr uint32_t SD_FMT         = 0x12;  // Format (16-bit)
    constexpr uint32_t SD_BDPL        = 0x18;  // BDL Pointer Lower (32-bit)
    constexpr uint32_t SD_BDPU        = 0x1C;  // BDL Pointer Upper (32-bit)

    // Stream control bits (CTL is 24-bit, accessed as 3 bytes)
    constexpr uint8_t SD_CTL0_SRST    = (1u << 0);   // Stream Reset
    constexpr uint8_t SD_CTL0_RUN     = (1u << 1);   // Stream Run
    constexpr uint8_t SD_CTL0_IOCE    = (1u << 2);   // Interrupt On Completion Enable
    constexpr uint8_t SD_CTL0_FEIE    = (1u << 3);   // FIFO Error Interrupt Enable
    constexpr uint8_t SD_CTL0_DEIE    = (1u << 4);   // Descriptor Error Interrupt Enable
    // CTL byte 2 (offset +2): bits 23:20 = Stream Number (1-15)

    // Stream status bits
    constexpr uint8_t SD_STS_BCIS     = (1u << 2);   // Buffer Completion Interrupt Status
    constexpr uint8_t SD_STS_FIFOE    = (1u << 3);   // FIFO Error
    constexpr uint8_t SD_STS_DESE     = (1u << 4);   // Descriptor Error

    // =========================================================================
    // Stream format register encoding (SD_FMT, 16-bit)
    // =========================================================================

    // Bit 15: Stream Type (0=PCM, 1=non-PCM)
    // Bits 14: Base Rate (0=48kHz, 1=44.1kHz)
    // Bits 13:11: Sample Rate Multiplier (0=x1, 1=x2, 2=x3, 3=x4)
    // Bits 10:8: Sample Rate Divisor (0=/1, 1=/2, 2=/3, ..., 7=/8)
    // Bits 7:4: Bits Per Sample (000=8, 001=16, 010=20, 011=24, 100=32)
    // Bits 3:0: Number of Channels - 1

    constexpr uint16_t FMT_BASE_44K   = (1u << 14);
    constexpr uint16_t FMT_BASE_48K   = 0;

    // =========================================================================
    // Buffer Descriptor List Entry (16 bytes)
    // =========================================================================

    struct BdlEntry {
        uint64_t Address;     // Physical address of buffer
        uint32_t Length;      // Length in bytes
        uint32_t Ioc;        // Bit 0: Interrupt on Completion
    } __attribute__((packed));

    static_assert(sizeof(BdlEntry) == 16, "BDL entry must be 16 bytes");

    // =========================================================================
    // RIRB response entry (8 bytes)
    // =========================================================================

    struct RirbEntry {
        uint32_t Response;
        uint32_t ResponseEx;  // Bits 3:0 = Codec Address, Bit 4 = Unsolicited
    } __attribute__((packed));

    static_assert(sizeof(RirbEntry) == 8, "RIRB entry must be 8 bytes");

    // =========================================================================
    // HDA Codec verbs
    // =========================================================================

    // Verb construction: (codec << 28) | (nid << 20) | verb
    // Get Parameter: verb = 0xF0000 | paramId
    // Set Converter Format: verb = 0x20000 | format
    // Set Amp Gain/Mute: verb = 0x30000 | payload
    // Set Converter Stream/Channel: verb = 0x70600 | (stream << 4) | channel
    // Set Pin Widget Control: verb = 0x70700 | value
    // Set EAPD/BTL Enable: verb = 0x70C00 | value
    // Set Power State: verb = 0x70500 | state
    // Get Config Default: verb = 0xF1C00
    // Set Connection Select: verb = 0x70100 | index

    // Parameter IDs
    constexpr uint32_t PARAM_VENDOR_ID           = 0x00;
    constexpr uint32_t PARAM_REVISION_ID         = 0x02;
    constexpr uint32_t PARAM_SUB_NODE_COUNT      = 0x04;
    constexpr uint32_t PARAM_FN_GROUP_TYPE       = 0x05;
    constexpr uint32_t PARAM_AUDIO_WIDGET_CAP    = 0x09;
    constexpr uint32_t PARAM_PCM_RATES           = 0x0A;
    constexpr uint32_t PARAM_STREAM_FORMATS      = 0x0B;
    constexpr uint32_t PARAM_PIN_CAPS            = 0x0C;
    constexpr uint32_t PARAM_INPUT_AMP_CAP       = 0x0D;
    constexpr uint32_t PARAM_CONN_LIST_LEN       = 0x0E;
    constexpr uint32_t PARAM_POWER_STATES        = 0x0F;
    constexpr uint32_t PARAM_OUTPUT_AMP_CAP      = 0x12;

    // Widget types (bits 23:20 of Audio Widget Capabilities)
    constexpr uint8_t WIDGET_AUDIO_OUTPUT        = 0x0;
    constexpr uint8_t WIDGET_AUDIO_INPUT         = 0x1;
    constexpr uint8_t WIDGET_AUDIO_MIXER         = 0x2;
    constexpr uint8_t WIDGET_AUDIO_SELECTOR      = 0x3;
    constexpr uint8_t WIDGET_PIN_COMPLEX         = 0x4;
    constexpr uint8_t WIDGET_POWER               = 0x5;
    constexpr uint8_t WIDGET_VOLUME_KNOB         = 0x6;
    constexpr uint8_t WIDGET_BEEP_GEN            = 0x7;
    constexpr uint8_t WIDGET_VENDOR_DEFINED      = 0xF;

    // Pin default config: device type (bits 23:20)
    constexpr uint8_t PIN_DEV_LINE_OUT           = 0x0;
    constexpr uint8_t PIN_DEV_SPEAKER            = 0x1;
    constexpr uint8_t PIN_DEV_HP_OUT             = 0x2;
    constexpr uint8_t PIN_DEV_CD                 = 0x3;
    constexpr uint8_t PIN_DEV_SPDIF_OUT          = 0x4;
    constexpr uint8_t PIN_DEV_LINE_IN            = 0x8;
    constexpr uint8_t PIN_DEV_MIC_IN             = 0xA;

    // Pin widget control bits
    constexpr uint8_t PIN_CTL_ENABLE_OUTPUT      = (1u << 6);  // OUT Enable
    constexpr uint8_t PIN_CTL_ENABLE_INPUT       = (1u << 5);  // IN Enable
    constexpr uint8_t PIN_CTL_ENABLE_HP          = (1u << 7);  // Headphone enable

    // EAPD/BTL bits
    constexpr uint8_t EAPD_ENABLE                = (1u << 1);

    // Amp gain/mute verb payload bits
    constexpr uint16_t AMP_SET_OUTPUT            = (1u << 15);
    constexpr uint16_t AMP_SET_INPUT             = (1u << 14);
    constexpr uint16_t AMP_SET_LEFT              = (1u << 13);
    constexpr uint16_t AMP_SET_RIGHT             = (1u << 12);
    constexpr uint16_t AMP_MUTE                  = (1u << 7);

    // =========================================================================
    // Ring buffer sizes
    // =========================================================================

    constexpr int CORB_ENTRIES        = 256;   // 256 entries * 4 bytes = 1024 bytes
    constexpr int RIRB_ENTRIES        = 256;   // 256 entries * 8 bytes = 2048 bytes
    constexpr int BDL_MAX_ENTRIES     = 256;   // Max BDL entries per stream

    // =========================================================================
    // DMA buffer configuration
    // =========================================================================

    constexpr int BUFFER_COUNT        = 2;     // Double-buffered
    constexpr int BUFFER_SIZE         = 0x4000; // 16 KiB per buffer segment
    constexpr int TOTAL_BUFFER_SIZE   = BUFFER_COUNT * BUFFER_SIZE;

    // =========================================================================
    // MSI configuration
    // =========================================================================

    constexpr uint8_t  MSI_IRQ        = 27;    // IRQ slot 27 = vector 59
    constexpr uint32_t MSI_VECTOR     = 59;
    constexpr uint32_t MSI_ADDR_BASE  = 0xFEE00000;

    // =========================================================================
    // Audio stream state (one active output stream)
    // =========================================================================

    struct AudioStream {
        bool        Active;
        uint32_t    SampleRate;
        uint8_t     Channels;
        uint8_t     BitsPerSample;
        uint8_t     StreamIndex;     // HDA stream index
        uint8_t     StreamTag;       // HDA stream tag (1-15)
        volatile uint32_t WritePos;  // Write position in ring buffer (bytes)
    };

    // =========================================================================
    // Public API
    // =========================================================================

    bool Probe(const Pci::PciDevice& dev);
    bool IsInitialized();

    // Returns the codec vendor/device ID (vendor in upper 16 bits, device in lower 16).
    // Returns 0 if no codec was found.
    uint32_t GetCodecVendorId();

    // Open an output stream. Returns stream handle (0) or -1 on failure.
    int Open(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);

    // Close an output stream.
    void Close(int handle);

    // Write PCM sample data to the stream buffer.
    // Returns number of bytes written (may be less than requested if buffer full).
    int Write(int handle, const uint8_t* data, uint32_t size);

    // Control commands
    constexpr int AUDIO_CTL_SET_VOLUME = 0;   // value: 0-100
    constexpr int AUDIO_CTL_GET_VOLUME = 1;
    constexpr int AUDIO_CTL_GET_POS    = 2;   // returns playback position in bytes
    constexpr int AUDIO_CTL_PAUSE      = 3;   // value: 1=pause, 0=resume
    int Control(int handle, int cmd, int value);

};
