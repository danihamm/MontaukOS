/*
    * IntelHda.cpp
    * Intel High Definition Audio controller driver
    * Copyright (c) 2026 Daniel Hammer
*/

#include "IntelHda.hpp"
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Hal/Apic/IoApic.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Libraries/Memory.hpp>

namespace Drivers::Audio::IntelHda {

    using namespace Kt;

    // =========================================================================
    // Driver state
    // =========================================================================

    static bool g_initialized = false;

    static volatile uint8_t* g_mmioBase = nullptr;
    static uint8_t g_bus, g_dev, g_func;

    // CORB/RIRB
    static volatile uint32_t* g_corb = nullptr;
    static volatile RirbEntry* g_rirb = nullptr;
    static uint64_t g_corbPhys = 0;
    static uint64_t g_rirbPhys = 0;
    static uint16_t g_corbSize = 0;    // Actual entries in use
    static uint16_t g_rirbSize = 0;
    static uint16_t g_rirbReadPtr = 0;

    // Codec info
    static uint8_t g_codecAddr = 0;    // First codec found
    static bool g_codecFound = false;
    static uint32_t g_codecVendorId = 0;  // Vendor (upper 16) | Device (lower 16)

    // Output path nodes discovered during codec enumeration
    static uint8_t g_dacNid = 0;       // DAC node
    static uint8_t g_speakerNid = 0;   // Speaker / Line Out pin node
    static uint8_t g_hpNid = 0;        // Headphone pin node (0 if none)
    static uint8_t g_pinNid = 0;       // Currently active output pin
    static uint8_t g_mixerNid = 0;     // Mixer node (0 if direct DAC→Pin)
    static bool g_hpPresenceDetect = false;  // HP pin supports presence detect
    static bool g_hpPlugged = false;         // Current jack state

    // Number of input/output streams from GCAP
    static uint8_t g_numInputStreams = 0;
    static uint8_t g_numOutputStreams = 0;

    // DMA buffers for output
    static volatile BdlEntry* g_bdl = nullptr;
    static uint64_t g_bdlPhys = 0;
    static uint8_t* g_dmaBuffer = nullptr;       // Virtual (HHDM)
    static uint64_t g_dmaBufferPhys = 0;

    // DMA position buffer
    static volatile uint32_t* g_dmaPos = nullptr;
    static uint64_t g_dmaPosPhys = 0;

    // Active stream
    static AudioStream g_stream = {};

    // Volume (0-127 for HDA, exposed as 0-100 to userspace)
    static int g_volume = 80;  // Default 80%

    // Set when an unsolicited response is detected (e.g. jack plug/unplug)
    static volatile bool g_jackEventPending = false;

    // Debounce: after a switch, ignore jack events for this many Write() calls.
    // Pin sense is unreliable while the pin widget is settling after reconfiguration.
    static uint32_t g_jackDebounce = 0;
    static constexpr uint32_t JACK_DEBOUNCE_COUNT = 512;

    // =========================================================================
    // MMIO register access
    // =========================================================================

    static uint8_t Read8(uint32_t reg) {
        return *(volatile uint8_t*)(g_mmioBase + reg);
    }

    static uint16_t Read16(uint32_t reg) {
        return *(volatile uint16_t*)(g_mmioBase + reg);
    }

    static uint32_t Read32(uint32_t reg) {
        return *(volatile uint32_t*)(g_mmioBase + reg);
    }

    static void Write8(uint32_t reg, uint8_t val) {
        *(volatile uint8_t*)(g_mmioBase + reg) = val;
    }

    static void Write16(uint32_t reg, uint16_t val) {
        *(volatile uint16_t*)(g_mmioBase + reg) = val;
    }

    static void Write32(uint32_t reg, uint32_t val) {
        *(volatile uint32_t*)(g_mmioBase + reg) = val;
    }

    // Stream descriptor register access
    static uint32_t StreamBase(uint8_t streamIndex) {
        return SD_BASE + (uint32_t)streamIndex * SD_SIZE;
    }

    static uint8_t ReadSD8(uint8_t stream, uint32_t offset) {
        return Read8(StreamBase(stream) + offset);
    }

    static uint16_t ReadSD16(uint8_t stream, uint32_t offset) {
        return Read16(StreamBase(stream) + offset);
    }

    static uint32_t ReadSD32(uint8_t stream, uint32_t offset) {
        return Read32(StreamBase(stream) + offset);
    }

    static void WriteSD8(uint8_t stream, uint32_t offset, uint8_t val) {
        Write8(StreamBase(stream) + offset, val);
    }

    static void WriteSD16(uint8_t stream, uint32_t offset, uint16_t val) {
        Write16(StreamBase(stream) + offset, val);
    }

    static void WriteSD32(uint8_t stream, uint32_t offset, uint32_t val) {
        Write32(StreamBase(stream) + offset, val);
    }

    // =========================================================================
    // Delay helper
    // =========================================================================

    static void MicroDelay(int us) {
        // Simple busy-wait; us is approximate
        for (volatile int i = 0; i < us * 100; i++) {
            asm volatile("pause");
        }
    }

    // =========================================================================
    // CORB/RIRB command interface
    // =========================================================================

    static bool SendVerb(uint32_t verb) {
        // Read current write pointer
        uint16_t wp = Read16(REG_CORBWP) & 0xFF;

        // Advance write pointer
        wp = (wp + 1) % g_corbSize;

        // Write verb to CORB
        g_corb[wp] = verb;

        // Update write pointer
        Write16(REG_CORBWP, wp);

        return true;
    }

    static bool ReadResponse(uint32_t* response, uint32_t* responseEx, int timeoutUs = 100000) {
        for (int i = 0; i < timeoutUs / 10; i++) {
            uint16_t wp = Read16(REG_RIRBWP) & 0xFF;

            if (g_rirbReadPtr != wp) {
                g_rirbReadPtr = (g_rirbReadPtr + 1) % g_rirbSize;

                uint32_t ex = g_rirb[g_rirbReadPtr].ResponseEx;

                // Bit 4 of ResponseEx = unsolicited response.
                // Skip it and flag for deferred jack detection.
                if (ex & (1u << 4)) {
                    g_jackEventPending = true;
                    continue;
                }

                if (response)
                    *response = g_rirb[g_rirbReadPtr].Response;
                if (responseEx)
                    *responseEx = ex;

                return true;
            }

            MicroDelay(10);
        }

        return false;
    }

    // Send a verb and wait for the response
    static uint32_t CodecCommand(uint8_t codec, uint8_t nid, uint32_t verb) {
        uint32_t fullVerb = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) | verb;
        SendVerb(fullVerb);

        uint32_t response = 0;
        if (!ReadResponse(&response, nullptr)) {
            KernelLogStream(WARNING, "HDA") << "Verb timeout: codec=" << base::dec
                << (uint64_t)codec << " nid=" << (uint64_t)nid
                << " verb=" << base::hex << (uint64_t)verb;
            return 0;
        }
        return response;
    }

    static uint32_t GetParameter(uint8_t codec, uint8_t nid, uint32_t paramId) {
        return CodecCommand(codec, nid, 0xF0000 | paramId);
    }

    // =========================================================================
    // Controller reset
    // =========================================================================

    static bool ResetController() {
        // Enter reset: clear CRST
        uint32_t gctl = Read32(REG_GCTL);
        gctl &= ~GCTL_CRST;
        Write32(REG_GCTL, gctl);

        // Wait for reset to take effect
        for (int i = 0; i < 1000; i++) {
            if ((Read32(REG_GCTL) & GCTL_CRST) == 0)
                break;
            MicroDelay(100);
        }
        if (Read32(REG_GCTL) & GCTL_CRST) {
            KernelLogStream(ERROR, "HDA") << "Controller failed to enter reset";
            return false;
        }

        MicroDelay(1000);

        // Exit reset: set CRST
        gctl = Read32(REG_GCTL);
        gctl |= GCTL_CRST;
        Write32(REG_GCTL, gctl);

        // Wait for controller to come out of reset
        for (int i = 0; i < 1000; i++) {
            if (Read32(REG_GCTL) & GCTL_CRST)
                break;
            MicroDelay(100);
        }
        if (!(Read32(REG_GCTL) & GCTL_CRST)) {
            KernelLogStream(ERROR, "HDA") << "Controller failed to exit reset";
            return false;
        }

        // Enable acceptance of unsolicited responses (for jack detect events)
        gctl = Read32(REG_GCTL);
        gctl |= GCTL_UNSOL;
        Write32(REG_GCTL, gctl);

        // Wait for codecs to initialize (spec says 521us minimum after CRST)
        MicroDelay(1000);

        return true;
    }

    // =========================================================================
    // CORB/RIRB initialization
    // =========================================================================

    static uint16_t DecodeSizeCapability(uint8_t sizeReg) {
        // Size register bits 7:4 = size capability, bits 1:0 = size select
        // Capability: bit 6 = 256 entries, bit 5 = 16 entries, bit 4 = 2 entries
        // We prefer 256 entries
        if (sizeReg & (1 << 6)) return 256;
        if (sizeReg & (1 << 5)) return 16;
        if (sizeReg & (1 << 4)) return 2;
        return 2;
    }

    static uint8_t SizeSelectBits(uint16_t entries) {
        if (entries == 256) return 0x02;
        if (entries == 16)  return 0x01;
        return 0x00;  // 2 entries
    }

    static bool InitCorbRirb() {
        // Stop CORB and RIRB
        Write8(REG_CORBCTL, 0);
        Write8(REG_RIRBCTL, 0);
        MicroDelay(100);

        // Determine CORB size
        uint8_t corbSizeReg = Read8(REG_CORBSIZE);
        g_corbSize = DecodeSizeCapability(corbSizeReg);

        // Allocate CORB buffer (aligned to 128 bytes, we allocate a full page)
        void* corbVirt = Memory::g_pfa->AllocateZeroed();
        g_corbPhys = Memory::SubHHDM(corbVirt);
        g_corb = (volatile uint32_t*)corbVirt;

        // Set CORB base address
        Write32(REG_CORBLBASE, (uint32_t)(g_corbPhys & 0xFFFFFFFF));
        Write32(REG_CORBUBASE, (uint32_t)(g_corbPhys >> 32));

        // Set CORB size
        Write8(REG_CORBSIZE, (corbSizeReg & 0xF0) | SizeSelectBits(g_corbSize));

        // Reset CORB read pointer
        Write16(REG_CORBRP, CORBRP_RST);
        MicroDelay(100);
        // Clear reset bit (some controllers need this)
        Write16(REG_CORBRP, 0);
        for (int i = 0; i < 1000; i++) {
            if ((Read16(REG_CORBRP) & CORBRP_RST) == 0)
                break;
            MicroDelay(10);
        }

        // Reset CORB write pointer
        Write16(REG_CORBWP, 0);

        // Determine RIRB size
        uint8_t rirbSizeReg = Read8(REG_RIRBSIZE);
        g_rirbSize = DecodeSizeCapability(rirbSizeReg);

        // Allocate RIRB buffer (8 bytes per entry)
        void* rirbVirt = Memory::g_pfa->AllocateZeroed();
        g_rirbPhys = Memory::SubHHDM(rirbVirt);
        g_rirb = (volatile RirbEntry*)rirbVirt;

        // Set RIRB base address
        Write32(REG_RIRBLBASE, (uint32_t)(g_rirbPhys & 0xFFFFFFFF));
        Write32(REG_RIRBUBASE, (uint32_t)(g_rirbPhys >> 32));

        // Set RIRB size
        Write8(REG_RIRBSIZE, (rirbSizeReg & 0xF0) | SizeSelectBits(g_rirbSize));

        // Reset RIRB write pointer
        Write16(REG_RIRBWP, RIRBWP_RST);

        // Set response interrupt count
        Write16(REG_RINTCNT, 1);

        // Initialize our read pointer to match HW state
        g_rirbReadPtr = 0;

        // Start CORB and RIRB
        Write8(REG_CORBCTL, CORBCTL_RUN);
        Write8(REG_RIRBCTL, RIRBCTL_RUN | RIRBCTL_RINTCTL);

        MicroDelay(100);

        KernelLogStream(OK, "HDA") << "CORB: " << base::dec << (uint64_t)g_corbSize
            << " entries, RIRB: " << (uint64_t)g_rirbSize << " entries";

        return true;
    }

    // =========================================================================
    // Codec discovery and output path enumeration
    // =========================================================================

    static bool DiscoverCodecs() {
        uint16_t statests = Read16(REG_STATESTS);
        KernelLogStream(INFO, "HDA") << "STATESTS=" << base::hex << (uint64_t)statests;

        for (uint8_t addr = 0; addr < 15; addr++) {
            if (statests & (1 << addr)) {
                uint32_t vendorId = GetParameter(addr, 0, PARAM_VENDOR_ID);
                if (vendorId == 0 || vendorId == 0xFFFFFFFF)
                    continue;

                g_codecAddr = addr;
                g_codecFound = true;
                g_codecVendorId = vendorId;

                KernelLogStream(OK, "HDA") << "Codec " << base::dec << (uint64_t)addr
                    << ": vendor=" << base::hex << (uint64_t)(vendorId >> 16)
                    << " device=" << (uint64_t)(vendorId & 0xFFFF);

                // Clear state change status
                Write16(REG_STATESTS, statests);
                return true;
            }
        }

        KernelLogStream(ERROR, "HDA") << "No codecs found";
        return false;
    }

    static bool EnumerateOutputPath() {
        // Get subordinate node count for root node (node 0)
        uint32_t subNodes = GetParameter(g_codecAddr, 0, PARAM_SUB_NODE_COUNT);
        uint8_t startNid = (subNodes >> 16) & 0xFF;
        uint8_t nodeCount = subNodes & 0xFF;

        KernelLogStream(INFO, "HDA") << "Root: startNid=" << base::dec << (uint64_t)startNid
            << " count=" << (uint64_t)nodeCount;

        // Find Audio Function Group
        uint8_t afg = 0;
        for (uint8_t i = 0; i < nodeCount; i++) {
            uint8_t nid = startNid + i;
            uint32_t fgType = GetParameter(g_codecAddr, nid, PARAM_FN_GROUP_TYPE);
            if ((fgType & 0xFF) == 0x01) {  // Audio Function Group
                afg = nid;
                KernelLogStream(INFO, "HDA") << "Audio Function Group at NID " << (uint64_t)nid;
                break;
            }
        }

        if (afg == 0) {
            KernelLogStream(ERROR, "HDA") << "No Audio Function Group found";
            return false;
        }

        // Power on the AFG
        CodecCommand(g_codecAddr, afg, 0x70500 | 0x00);  // D0 (fully on)

        // Enumerate widgets in the AFG
        subNodes = GetParameter(g_codecAddr, afg, PARAM_SUB_NODE_COUNT);
        startNid = (subNodes >> 16) & 0xFF;
        nodeCount = subNodes & 0xFF;

        KernelLogStream(INFO, "HDA") << "AFG widgets: startNid=" << base::dec
            << (uint64_t)startNid << " count=" << (uint64_t)nodeCount;

        // Find DAC, output pins, and mixer
        uint8_t firstDac = 0;
        uint8_t speakerPin = 0;   // Best speaker/line-out pin
        uint8_t speakerPri = 0xFF;
        uint8_t hpPin = 0;        // Best headphone pin

        for (uint8_t i = 0; i < nodeCount; i++) {
            uint8_t nid = startNid + i;
            uint32_t widgetCap = GetParameter(g_codecAddr, nid, PARAM_AUDIO_WIDGET_CAP);
            uint8_t widgetType = (widgetCap >> 20) & 0xF;

            if (widgetType == WIDGET_AUDIO_OUTPUT && firstDac == 0) {
                firstDac = nid;
                KernelLogStream(INFO, "HDA") << "DAC found at NID " << base::dec << (uint64_t)nid;
            }

            if (widgetType == WIDGET_PIN_COMPLEX) {
                uint32_t pinCfg = CodecCommand(g_codecAddr, nid, 0xF1C00);
                uint8_t connectivity = (pinCfg >> 30) & 0x03;
                uint8_t devType = (pinCfg >> 20) & 0x0F;

                if (connectivity == 1)  // No physical connection
                    continue;

                KernelLogStream(INFO, "HDA") << "Pin NID " << base::dec << (uint64_t)nid
                    << " type=" << (uint64_t)devType
                    << " connectivity=" << (uint64_t)connectivity;

                if (devType == PIN_DEV_HP_OUT) {
                    if (hpPin == 0) hpPin = nid;
                } else if (devType == PIN_DEV_SPEAKER || devType == PIN_DEV_LINE_OUT) {
                    uint8_t pri = (devType == PIN_DEV_SPEAKER) ? 0 : 1;
                    if (pri < speakerPri) {
                        speakerPin = nid;
                        speakerPri = pri;
                    }
                }
            }

            if (widgetType == WIDGET_AUDIO_MIXER) {
                if (g_mixerNid == 0) g_mixerNid = nid;
            }
        }

        if (firstDac == 0) {
            KernelLogStream(ERROR, "HDA") << "No DAC found";
            return false;
        }
        if (speakerPin == 0 && hpPin == 0) {
            KernelLogStream(ERROR, "HDA") << "No output pin found";
            return false;
        }

        g_dacNid = firstDac;
        g_speakerNid = speakerPin;
        g_hpNid = hpPin;

        // Default to speaker; if no speaker, use HP
        g_pinNid = g_speakerNid ? g_speakerNid : g_hpNid;

        // Check if HP pin supports presence detect
        if (g_hpNid) {
            uint32_t pinCaps = GetParameter(g_codecAddr, g_hpNid, PARAM_PIN_CAPS);
            g_hpPresenceDetect = (pinCaps & (1 << 2)) != 0;  // Bit 2 = Presence Detect Capable
            KernelLogStream(INFO, "HDA") << "HP pin NID " << base::dec << (uint64_t)g_hpNid
                << (g_hpPresenceDetect ? " (presence detect)" : " (no presence detect)");

            // Enable unsolicited responses on the HP pin for jack events
            if (g_hpPresenceDetect) {
                // Set Unsolicited Response: enable (bit 7) | tag 0x01
                CodecCommand(g_codecAddr, g_hpNid, 0x70800 | (1 << 7) | 0x01);
            }
        }

        KernelLogStream(OK, "HDA") << "Output path: DAC=" << base::dec << (uint64_t)g_dacNid
            << (g_mixerNid ? " Mixer=" : "") << (g_mixerNid ? (uint64_t)g_mixerNid : 0)
            << " Speaker=" << (uint64_t)g_speakerNid
            << " HP=" << (uint64_t)g_hpNid;

        return true;
    }

    // =========================================================================
    // Stream format encoding
    // =========================================================================

    static uint16_t EncodeFormat(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
        uint16_t fmt = 0;

        // Base rate and multiplier/divisor
        switch (sampleRate) {
            case 8000:   fmt = FMT_BASE_48K | (0 << 11) | (5 << 8); break;  // 48k / 6
            case 11025:  fmt = FMT_BASE_44K | (0 << 11) | (3 << 8); break;  // 44.1k / 4
            case 16000:  fmt = FMT_BASE_48K | (0 << 11) | (2 << 8); break;  // 48k / 3
            case 22050:  fmt = FMT_BASE_44K | (0 << 11) | (1 << 8); break;  // 44.1k / 2
            case 32000:  fmt = FMT_BASE_48K | (1 << 11) | (2 << 8); break;  // 48k * 2 / 3
            case 44100:  fmt = FMT_BASE_44K | (0 << 11) | (0 << 8); break;  // 44.1k
            case 48000:  fmt = FMT_BASE_48K | (0 << 11) | (0 << 8); break;  // 48k
            case 88200:  fmt = FMT_BASE_44K | (1 << 11) | (0 << 8); break;  // 44.1k * 2
            case 96000:  fmt = FMT_BASE_48K | (1 << 11) | (0 << 8); break;  // 48k * 2
            case 176400: fmt = FMT_BASE_44K | (3 << 11) | (0 << 8); break;  // 44.1k * 4
            case 192000: fmt = FMT_BASE_48K | (3 << 11) | (0 << 8); break;  // 48k * 4
            default:     fmt = FMT_BASE_48K | (0 << 11) | (0 << 8); break;  // Default 48k
        }

        // Bits per sample
        switch (bitsPerSample) {
            case 8:  fmt |= (0 << 4); break;
            case 16: fmt |= (1 << 4); break;
            case 20: fmt |= (2 << 4); break;
            case 24: fmt |= (3 << 4); break;
            case 32: fmt |= (4 << 4); break;
            default: fmt |= (1 << 4); break;  // Default 16-bit
        }

        // Channels (0-based)
        fmt |= (channels - 1) & 0x0F;

        return fmt;
    }

    // =========================================================================
    // Volume control
    // =========================================================================

    static void SetOutputVolume(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        g_volume = percent;

        // Convert 0-100 to HDA 0-127 gain (7-bit)
        uint8_t gain = (uint8_t)((percent * 127) / 100);
        uint16_t ampPayload = AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | gain;

        // Set DAC output amp
        CodecCommand(g_codecAddr, g_dacNid, 0x30000 | ampPayload);

        // Set mixer output amp if present
        if (g_mixerNid) {
            CodecCommand(g_codecAddr, g_mixerNid, 0x30000 | ampPayload);
        }
    }

    // =========================================================================
    // Pin enable / disable helpers
    // =========================================================================

    static void EnablePin(uint8_t nid) {
        if (nid == 0) return;

        // Power on
        CodecCommand(g_codecAddr, nid, 0x70500 | 0x00);  // D0

        // Read pin config to determine type
        uint32_t pinCfg = CodecCommand(g_codecAddr, nid, 0xF1C00);
        uint8_t devType = (pinCfg >> 20) & 0x0F;

        uint8_t pinCtl = PIN_CTL_ENABLE_OUTPUT;
        if (devType == PIN_DEV_HP_OUT)
            pinCtl |= PIN_CTL_ENABLE_HP;

        CodecCommand(g_codecAddr, nid, 0x70700 | pinCtl);

        // Enable EAPD if supported
        uint32_t pinCaps = GetParameter(g_codecAddr, nid, PARAM_PIN_CAPS);
        if (pinCaps & (1 << 16))
            CodecCommand(g_codecAddr, nid, 0x70C00 | EAPD_ENABLE);

        // Unmute output amp
        CodecCommand(g_codecAddr, nid,
            0x30000 | AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | 127);
    }

    static void DisablePin(uint8_t nid) {
        if (nid == 0) return;

        // Mute output amp
        CodecCommand(g_codecAddr, nid,
            0x30000 | AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | AMP_MUTE);

        // Disable pin output
        CodecCommand(g_codecAddr, nid, 0x70700 | 0x00);
    }

    // =========================================================================
    // Jack detection and output switching
    // =========================================================================

    static bool IsHpPlugged() {
        if (!g_hpNid || !g_hpPresenceDetect)
            return false;
        // Get Pin Sense (verb 0xF0900): bit 31 = presence detected
        uint32_t sense = CodecCommand(g_codecAddr, g_hpNid, 0xF0900);
        return (sense & (1u << 31)) != 0;
    }

    static void SwitchOutput(bool toHeadphones) {
        if (toHeadphones) {
            // Mute speaker, enable HP
            DisablePin(g_speakerNid);
            EnablePin(g_hpNid);
            g_pinNid = g_hpNid;
            KernelLogStream(INFO, "HDA") << "Switched to headphone output";
        } else {
            // Mute HP, enable speaker
            DisablePin(g_hpNid);
            EnablePin(g_speakerNid);
            g_pinNid = g_speakerNid;
            KernelLogStream(INFO, "HDA") << "Switched to speaker output";
        }
        // Re-apply volume on the now-active pin
        SetOutputVolume(g_volume);

        // Start debounce: pin sense is unreliable right after reconfiguration,
        // and the pin change itself may trigger spurious unsolicited responses.
        g_jackDebounce = JACK_DEBOUNCE_COUNT;
        g_jackEventPending = false;
    }

    // Drain any unsolicited responses sitting in the RIRB.
    // During normal playback no CodecCommands are sent, so ReadResponse()
    // never runs and unsolicited responses (jack events) pile up unread.
    // This consumes them and sets g_jackEventPending if any are found.
    static void DrainUnsolicitedResponses() {
        for (;;) {
            uint16_t wp = Read16(REG_RIRBWP) & 0xFF;
            if (g_rirbReadPtr == wp)
                break;

            uint16_t next = (g_rirbReadPtr + 1) % g_rirbSize;
            uint32_t ex = g_rirb[next].ResponseEx;

            if (!(ex & (1u << 4)))
                break;  // Solicited response — leave for ReadResponse()

            g_rirbReadPtr = next;
            g_jackEventPending = true;
        }
    }

    // Poll jack state; call when an unsolicited response fires.
    // Switches output if state changed.
    static void PollJackState() {
        if (!g_hpNid || !g_hpPresenceDetect || !g_speakerNid)
            return;

        bool plugged = IsHpPlugged();
        if (plugged != g_hpPlugged) {
            g_hpPlugged = plugged;
            SwitchOutput(plugged);
        }
    }

    // =========================================================================
    // Configure output path (DAC -> [Mixer ->] Pin)
    // =========================================================================

    static void ConfigureOutputPath(uint16_t streamFormat, uint8_t streamTag) {
        // Power on DAC and mixer
        CodecCommand(g_codecAddr, g_dacNid, 0x70500 | 0x00);  // D0
        if (g_mixerNid) CodecCommand(g_codecAddr, g_mixerNid, 0x70500 | 0x00);

        // Set converter stream/channel on DAC: stream tag in bits 7:4, channel 0 in bits 3:0
        CodecCommand(g_codecAddr, g_dacNid, 0x70600 | ((uint32_t)streamTag << 4) | 0);

        // Set converter format on DAC
        CodecCommand(g_codecAddr, g_dacNid, 0x20000 | streamFormat);

        // Unmute DAC output amp
        SetOutputVolume(g_volume);

        // If there's a mixer, unmute it
        if (g_mixerNid) {
            CodecCommand(g_codecAddr, g_mixerNid,
                0x30000 | AMP_SET_INPUT | AMP_SET_LEFT | AMP_SET_RIGHT | 127);
        }

        // Check jack state and enable the correct output pin
        if (g_hpNid && g_hpPresenceDetect && g_speakerNid) {
            g_hpPlugged = IsHpPlugged();
            if (g_hpPlugged) {
                EnablePin(g_hpNid);
                DisablePin(g_speakerNid);
                g_pinNid = g_hpNid;
            } else {
                EnablePin(g_speakerNid);
                DisablePin(g_hpNid);
                g_pinNid = g_speakerNid;
            }
        } else {
            // Only one output available, just enable it
            EnablePin(g_pinNid);
        }
    }

    // =========================================================================
    // Output stream setup
    // =========================================================================

    static bool SetupOutputStream(uint8_t streamIndex, uint16_t streamFormat) {
        // Reset stream
        WriteSD8(streamIndex, SD_CTL, SD_CTL0_SRST);
        for (int i = 0; i < 1000; i++) {
            if (ReadSD8(streamIndex, SD_CTL) & SD_CTL0_SRST)
                break;
            MicroDelay(10);
        }

        // Clear reset
        WriteSD8(streamIndex, SD_CTL, 0);
        for (int i = 0; i < 1000; i++) {
            if ((ReadSD8(streamIndex, SD_CTL) & SD_CTL0_SRST) == 0)
                break;
            MicroDelay(10);
        }

        // Clear status bits
        WriteSD8(streamIndex, SD_STS, SD_STS_BCIS | SD_STS_FIFOE | SD_STS_DESE);

        // Set up BDL (double-buffered)
        for (int i = 0; i < BUFFER_COUNT; i++) {
            g_bdl[i].Address = g_dmaBufferPhys + (uint64_t)(i * BUFFER_SIZE);
            g_bdl[i].Length = BUFFER_SIZE;
            g_bdl[i].Ioc = 1;  // Interrupt on completion for each buffer
        }

        // Set BDL address
        WriteSD32(streamIndex, SD_BDPL, (uint32_t)(g_bdlPhys & 0xFFFFFFFF));
        WriteSD32(streamIndex, SD_BDPU, (uint32_t)(g_bdlPhys >> 32));

        // Set cyclic buffer length (total bytes across all BDL entries)
        WriteSD32(streamIndex, SD_CBL, TOTAL_BUFFER_SIZE);

        // Set last valid index (0-based)
        WriteSD16(streamIndex, SD_LVI, BUFFER_COUNT - 1);

        // Set stream format
        WriteSD16(streamIndex, SD_FMT, streamFormat);

        // Set stream tag in CTL byte 2 (bits 23:20)
        uint8_t streamTag = 1;  // Use stream tag 1
        WriteSD8(streamIndex, SD_CTL + 2, (streamTag << 4));

        return true;
    }

    static void StartStream(uint8_t streamIndex) {
        // Enable interrupts and start running
        uint8_t ctl = ReadSD8(streamIndex, SD_CTL);
        ctl |= SD_CTL0_RUN | SD_CTL0_IOCE;
        WriteSD8(streamIndex, SD_CTL, ctl);
    }

    static void StopStream(uint8_t streamIndex) {
        uint8_t ctl = ReadSD8(streamIndex, SD_CTL);
        ctl &= ~(SD_CTL0_RUN | SD_CTL0_IOCE);
        WriteSD8(streamIndex, SD_CTL, ctl);

        // Wait for stream to stop
        for (int i = 0; i < 1000; i++) {
            if ((ReadSD8(streamIndex, SD_CTL) & SD_CTL0_RUN) == 0)
                break;
            MicroDelay(10);
        }
    }

    // =========================================================================
    // MSI setup
    // =========================================================================

    static void HandleInterrupt(uint8_t irq);

    static bool SetupMsi(uint8_t bus, uint8_t dev, uint8_t func) {
        uint8_t cap = Pci::FindCapability(bus, dev, func, Pci::PCI_CAP_MSI);
        if (cap == 0) {
            KernelLogStream(INFO, "HDA") << "MSI capability not found";
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

        msgCtrl &= ~(0x70);
        msgCtrl |= (1 << 0);
        Pci::LegacyWrite16(bus, dev, func, cap + 2, msgCtrl);

        uint16_t pciCmd = Pci::LegacyRead16(bus, dev, func, (uint8_t)Pci::PCI_REG_COMMAND);
        pciCmd |= Pci::PCI_CMD_INTX_DISABLE;
        Pci::LegacyWrite16(bus, dev, func, (uint8_t)Pci::PCI_REG_COMMAND, pciCmd);

        Hal::RegisterIrqHandler(MSI_IRQ, HandleInterrupt);

        KernelLogStream(OK, "HDA") << "MSI enabled: vector " << base::dec << (uint64_t)MSI_VECTOR
            << " (IRQ slot " << (uint64_t)MSI_IRQ << ")" << (is64bit ? " [64-bit]" : " [32-bit]");

        return true;
    }

    // =========================================================================
    // Interrupt handler
    // =========================================================================

    static void HandleInterrupt(uint8_t /*irq*/) {
        uint32_t intsts = Read32(REG_INTSTS);

        // Handle stream interrupts (bits 0-29 correspond to stream descriptors)
        if (g_stream.Active) {
            uint8_t si = g_stream.StreamIndex;
            if (intsts & (1u << si)) {
                uint8_t sts = ReadSD8(si, SD_STS);
                WriteSD8(si, SD_STS, sts);
            }
        }

        // Handle RIRB interrupt (controller interrupt enable bit 30)
        // Do NOT advance g_rirbReadPtr here — ReadResponse() owns it.
        // Just clear status. Unsolicited responses are detected and flagged
        // in ReadResponse() by checking ResponseEx bit 4.
        if (intsts & (1u << 30)) {
            uint8_t rirbSts = Read8(REG_RIRBSTS);
            Write8(REG_RIRBSTS, rirbSts);
        }
    }

    // =========================================================================
    // DMA buffer allocation
    // =========================================================================

    static bool AllocateDmaBuffers() {
        // Allocate BDL (needs 128-byte alignment, page-aligned is fine)
        void* bdlVirt = Memory::g_pfa->AllocateZeroed();
        g_bdlPhys = Memory::SubHHDM(bdlVirt);
        g_bdl = (volatile BdlEntry*)bdlVirt;

        // Allocate DMA audio buffers (BUFFER_COUNT * BUFFER_SIZE = 32 KiB = 8 pages)
        int pages = TOTAL_BUFFER_SIZE / 0x1000;
        void* bufVirt = Memory::g_pfa->ReallocConsecutive(nullptr, pages);
        if (!bufVirt) {
            KernelLogStream(ERROR, "HDA") << "Failed to allocate DMA buffer (" << base::dec << pages << " pages)";
            return false;
        }
        memset(bufVirt, 0, TOTAL_BUFFER_SIZE);
        g_dmaBufferPhys = Memory::SubHHDM(bufVirt);
        g_dmaBuffer = (uint8_t*)bufVirt;

        // Allocate DMA position buffer (one page)
        void* posVirt = Memory::g_pfa->AllocateZeroed();
        g_dmaPosPhys = Memory::SubHHDM(posVirt);
        g_dmaPos = (volatile uint32_t*)posVirt;

        KernelLogStream(OK, "HDA") << "DMA buffers allocated: " << base::dec
            << BUFFER_COUNT << "x" << BUFFER_SIZE << " bytes";

        return true;
    }

    // =========================================================================
    // Probe (called by PCI driver matching)
    // =========================================================================

    bool Probe(const Pci::PciDevice& dev) {
        g_bus = dev.Bus;
        g_dev = dev.Device;
        g_func = dev.Function;

        KernelLogStream(INFO, "HDA") << "Probing Intel HDA at "
            << base::hex << (uint64_t)g_bus << ":" << (uint64_t)g_dev << "." << base::dec << (uint64_t)g_func
            << " (device=" << base::hex << (uint64_t)dev.DeviceId << ")";

        // Enable memory space and bus mastering
        Pci::EnableBusMaster(g_bus, g_dev, g_func);

        // Read BAR0
        uint64_t mmioPhys = Pci::ReadBar0(g_bus, g_dev, g_func);
        if (mmioPhys == 0) {
            KernelLogStream(ERROR, "HDA") << "BAR0 is zero";
            return false;
        }

        KernelLogStream(INFO, "HDA") << "MMIO base: " << base::hex << mmioPhys;

        // Map MMIO region (HDA MMIO is typically 16 KiB)
        for (uint64_t offset = 0; offset < 0x4000; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset,
                                           Memory::HHDM(mmioPhys + offset));
        }
        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        // Read capabilities
        uint16_t gcap = Read16(REG_GCAP);
        uint8_t vmin = Read8(REG_VMIN);
        uint8_t vmaj = Read8(REG_VMAJ);

        g_numOutputStreams = (gcap >> 12) & 0xF;
        g_numInputStreams = (gcap >> 8) & 0xF;
        uint8_t numBidiStreams = (gcap >> 3) & 0x1F;
        bool ok64bit = (gcap & 0x01) != 0;

        KernelLogStream(INFO, "HDA") << "HDA v" << base::dec << (uint64_t)vmaj << "." << (uint64_t)vmin
            << " | ISS=" << (uint64_t)g_numInputStreams
            << " OSS=" << (uint64_t)g_numOutputStreams
            << " BSS=" << (uint64_t)numBidiStreams
            << (ok64bit ? " 64-bit" : " 32-bit");

        if (g_numOutputStreams == 0) {
            KernelLogStream(ERROR, "HDA") << "No output streams available";
            return false;
        }

        // Reset the controller
        if (!ResetController()) return false;

        // Allocate DMA buffers
        if (!AllocateDmaBuffers()) return false;

        // Set up DMA position buffer
        Write32(REG_DPIBLBASE, (uint32_t)(g_dmaPosPhys & 0xFFFFFFFF) | 1);  // Bit 0 = enable
        Write32(REG_DPIBUBASE, (uint32_t)(g_dmaPosPhys >> 32));

        // Initialize CORB/RIRB
        if (!InitCorbRirb()) return false;

        // Set up MSI (with legacy fallback)
        bool msiOk = SetupMsi(g_bus, g_dev, g_func);
        if (!msiOk) {
            uint8_t irqLine = Pci::LegacyRead8(g_bus, g_dev, g_func, (uint8_t)Pci::PCI_REG_INTERRUPT);
            if (irqLine != 0xFF) {
                KernelLogStream(INFO, "HDA") << "Falling back to legacy IRQ " << base::dec << (uint64_t)irqLine;
                Hal::RegisterIrqHandler(irqLine, HandleInterrupt);
            } else {
                KernelLogStream(WARNING, "HDA") << "No interrupt available, polling only";
            }
        }

        // Enable interrupts
        uint32_t intctl = INTCTL_GIE | INTCTL_CIE;
        // Enable interrupt for all output streams
        for (uint8_t i = 0; i < g_numOutputStreams; i++) {
            intctl |= (1u << (g_numInputStreams + i));
        }
        Write32(REG_INTCTL, intctl);

        // Discover codecs
        if (!DiscoverCodecs()) return false;

        // Enumerate output path
        if (!EnumerateOutputPath()) return false;

        // Read initial jack state
        if (g_hpNid && g_hpPresenceDetect) {
            g_hpPlugged = IsHpPlugged();
            KernelLogStream(INFO, "HDA") << "Headphone jack: "
                << (g_hpPlugged ? "plugged" : "unplugged");
        }

        g_initialized = true;
        KernelLogStream(OK, "HDA") << "Intel HDA initialized successfully";

        return true;
    }

    // =========================================================================
    // Public API
    // =========================================================================

    bool IsInitialized() {
        return g_initialized;
    }

    uint32_t GetCodecVendorId() {
        return g_codecVendorId;
    }

    int Open(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
        if (!g_initialized) return -1;
        if (g_stream.Active) return -1;  // Only one stream at a time
        if (channels < 1 || channels > 8) return -1;
        if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 20
            && bitsPerSample != 24 && bitsPerSample != 32) return -1;

        // Output stream index = numInputStreams (first output stream)
        uint8_t streamIndex = g_numInputStreams;
        uint8_t streamTag = 1;

        // Encode the stream format
        uint16_t fmt = EncodeFormat(sampleRate, channels, bitsPerSample);

        // Configure the codec output path
        ConfigureOutputPath(fmt, streamTag);

        // Set up the output stream DMA
        if (!SetupOutputStream(streamIndex, fmt)) return -1;

        // Zero the DMA buffer
        memset(g_dmaBuffer, 0, TOTAL_BUFFER_SIZE);

        // Record stream state
        g_stream.Active = true;
        g_stream.SampleRate = sampleRate;
        g_stream.Channels = channels;
        g_stream.BitsPerSample = bitsPerSample;
        g_stream.StreamIndex = streamIndex;
        g_stream.StreamTag = streamTag;
        g_stream.WritePos = 0;

        // Start the stream
        StartStream(streamIndex);

        KernelLogStream(OK, "HDA") << "Stream opened: " << base::dec
            << (uint64_t)sampleRate << "Hz " << (uint64_t)bitsPerSample << "-bit "
            << (uint64_t)channels << "ch";

        return 0;  // Handle 0
    }

    void Close(int handle) {
        if (handle != 0 || !g_stream.Active) return;

        StopStream(g_stream.StreamIndex);

        // Mute output
        CodecCommand(g_codecAddr, g_dacNid,
            0x30000 | AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | AMP_MUTE);

        g_stream.Active = false;

        KernelLogStream(OK, "HDA") << "Stream closed";
    }

    int Write(int handle, const uint8_t* data, uint32_t size) {
        if (handle != 0 || !g_stream.Active || !data || size == 0)
            return -1;

        // Drain unsolicited responses from the RIRB — during playback no
        // CodecCommands are sent, so ReadResponse() never runs and jack
        // events would otherwise go unnoticed.
        if (g_hpPresenceDetect && g_speakerNid) {
            DrainUnsolicitedResponses();

            if (g_jackDebounce > 0) {
                g_jackDebounce--;
                g_jackEventPending = false;
            } else if (g_jackEventPending) {
                g_jackEventPending = false;
                PollJackState();
            }
        }

        // Get current hardware playback position from DMA position buffer
        // Each stream's position is at dmaPos[streamIndex * 2]
        uint32_t hwPos = g_dmaPos[g_stream.StreamIndex * 2];
        uint32_t writePos = g_stream.WritePos;

        // Calculate available space in the ring buffer
        uint32_t available;
        if (writePos >= hwPos) {
            available = TOTAL_BUFFER_SIZE - (writePos - hwPos);
        } else {
            available = hwPos - writePos;
        }

        // Leave a small gap to avoid write pointer catching up to read pointer
        if (available > 64) available -= 64;
        else available = 0;

        if (size > available) size = available;
        if (size == 0) return 0;

        // Write data to DMA buffer (handle wrap-around)
        uint32_t firstChunk = TOTAL_BUFFER_SIZE - writePos;
        if (firstChunk > size) firstChunk = size;

        memcpy(g_dmaBuffer + writePos, data, firstChunk);
        if (size > firstChunk) {
            memcpy(g_dmaBuffer, data + firstChunk, size - firstChunk);
        }

        g_stream.WritePos = (writePos + size) % TOTAL_BUFFER_SIZE;

        return (int)size;
    }

    int Control(int handle, int cmd, int value) {
        if (handle != 0) return -1;

        switch (cmd) {
            case AUDIO_CTL_SET_VOLUME:
                if (!g_initialized) { g_volume = value; return 0; }
                SetOutputVolume(value);
                return 0;

            case AUDIO_CTL_GET_VOLUME:
                return g_volume;

            case AUDIO_CTL_GET_POS:
                if (!g_stream.Active) return 0;
                return (int)g_dmaPos[g_stream.StreamIndex * 2];

            case AUDIO_CTL_PAUSE:
                if (!g_stream.Active) return -1;
                if (value)
                    StopStream(g_stream.StreamIndex);
                else
                    StartStream(g_stream.StreamIndex);
                return 0;

            default:
                return -1;
        }
    }

};
