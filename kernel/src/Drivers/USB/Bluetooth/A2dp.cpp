/*
    * A2dp.cpp
    * Bluetooth A2DP / AVDTP implementation
    * Copyright (c) 2026 Daniel Hammer
*/

#include "A2dp.hpp"
#include "Sbc.hpp"
#include "L2cap.hpp"
#include "Hci.hpp"
#include <Drivers/USB/Xhci.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Libraries/Memory.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

namespace Drivers::USB::Bluetooth::A2dp {

    // =========================================================================
    // AVDTP constants
    // =========================================================================

    // AVDTP signal IDs
    constexpr uint8_t AVDTP_DISCOVER         = 0x01;
    constexpr uint8_t AVDTP_GET_CAPABILITIES = 0x02;
    constexpr uint8_t AVDTP_SET_CONFIGURATION = 0x03;
    constexpr uint8_t AVDTP_GET_CONFIGURATION = 0x04;
    constexpr uint8_t AVDTP_RECONFIGURE      = 0x05;
    constexpr uint8_t AVDTP_OPEN             = 0x06;
    constexpr uint8_t AVDTP_START            = 0x07;
    constexpr uint8_t AVDTP_CLOSE            = 0x08;
    constexpr uint8_t AVDTP_SUSPEND          = 0x09;
    constexpr uint8_t AVDTP_ABORT            = 0x0A;

    // AVDTP message types
    constexpr uint8_t MSG_COMMAND            = 0x00;
    constexpr uint8_t MSG_GENERAL_REJECT     = 0x01;
    constexpr uint8_t MSG_RESPONSE_ACCEPT    = 0x02;
    constexpr uint8_t MSG_RESPONSE_REJECT    = 0x03;

    // AVDTP packet types
    constexpr uint8_t PKT_SINGLE             = 0x00;

    // Service category IDs
    constexpr uint8_t CAT_MEDIA_TRANSPORT    = 0x01;
    constexpr uint8_t CAT_MEDIA_CODEC        = 0x07;

    // Media type
    constexpr uint8_t MEDIA_AUDIO            = 0x00;

    // Codec type
    constexpr uint8_t CODEC_SBC              = 0x00;

    // SBC capability octets
    // Octet 0: Sampling Frequency (bits 7-4) | Channel Mode (bits 3-0)
    // Octet 1: Block Length (bits 7-4) | Subbands (bits 3-2) | Alloc Method (bits 1-0)
    // Octet 2: Min Bitpool
    // Octet 3: Max Bitpool

    // =========================================================================
    // State
    // =========================================================================

    static State g_state = State::Idle;
    static uint16_t g_sigCid = 0;      // L2CAP CID for AVDTP signaling
    static uint16_t g_mediaCid = 0;    // L2CAP CID for AVDTP media transport
    static uint8_t g_txLabel = 1;
    static uint8_t g_remoteSeid = 0;   // Remote stream endpoint ID
    static uint8_t g_localSeid = 1;    // Our local SEID

    // SBC encoder
    static Sbc::SbcEncoder g_sbcEncoder = {};
    static bool g_sbcInitialized = false;

    // Media packet state
    static uint16_t g_seqNum = 0;
    static uint32_t g_timestamp = 0;

    // Volume
    static int g_volume = 80;

    // AVDTP response tracking
    static volatile bool g_avdtpResponseReady = false;
    static uint8_t g_avdtpResponseBuf[128] = {};
    static uint32_t g_avdtpResponseLen = 0;

    // =========================================================================
    // AVDTP signaling helpers
    // =========================================================================

    static void SendAvdtpCommand(uint8_t signalId, const uint8_t* payload, uint16_t len) {
        uint8_t buf[128] = {};

        // AVDTP single packet header
        buf[0] = (g_txLabel << 4) | (PKT_SINGLE << 2) | MSG_COMMAND;
        buf[1] = signalId;
        g_txLabel = (g_txLabel + 1) & 0x0F;

        if (payload && len > 0) {
            memcpy(&buf[2], payload, len);
        }

        L2cap::SendData(g_sigCid, buf, 2 + len);
    }

    static void SendAvdtpResponse(uint8_t txLabel, uint8_t signalId,
                                   const uint8_t* payload, uint16_t len) {
        uint8_t buf[128] = {};

        buf[0] = (txLabel << 4) | (PKT_SINGLE << 2) | MSG_RESPONSE_ACCEPT;
        buf[1] = signalId;

        if (payload && len > 0) {
            memcpy(&buf[2], payload, len);
        }

        L2cap::SendData(g_sigCid, buf, 2 + len);
    }

    // =========================================================================
    // WaitAvdtpResponse
    // =========================================================================

    static bool WaitAvdtpResponse(uint32_t timeoutMs = 3000) {
        g_avdtpResponseReady = false;
        uint64_t start = Timekeeping::GetMilliseconds();

        while (Timekeeping::GetMilliseconds() - start < timeoutMs) {
            Xhci::PollEvents();
            if (g_avdtpResponseReady) return true;
            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }
        return false;
    }

    // =========================================================================
    // AVDTP signaling procedures
    // =========================================================================

    static bool AvdtpDiscover() {
        SendAvdtpCommand(AVDTP_DISCOVER, nullptr, 0);

        if (!WaitAvdtpResponse()) {
            KernelLogStream(WARNING, "BT-A2DP") << "AVDTP Discover timeout";
            return false;
        }

        // Parse discover response to find audio sink SEID
        // Response format: each SEP is 2 bytes:
        //   Byte 0: SEID(6) | InUse(1) | Rsvd(1)
        //   Byte 1: MediaType(4) | SEPType(4)  (SEPType: 0=Source, 1=Sink)
        if (g_avdtpResponseLen >= 4) {
            for (uint32_t i = 2; i + 1 < g_avdtpResponseLen; i += 2) {
                uint8_t seid = (g_avdtpResponseBuf[i] >> 2) & 0x3F;
                bool inUse = (g_avdtpResponseBuf[i] >> 1) & 1;
                uint8_t mediaType = (g_avdtpResponseBuf[i + 1] >> 4) & 0x0F;
                uint8_t sepType = g_avdtpResponseBuf[i + 1] & 0x0F;

                if (mediaType == MEDIA_AUDIO && sepType == 0x01 && !inUse) {
                    g_remoteSeid = seid;
                    KernelLogStream(INFO, "BT-A2DP") << "Found audio sink SEID="
                        << (uint64_t)seid;
                    return true;
                }
            }
        }

        KernelLogStream(WARNING, "BT-A2DP") << "No audio sink SEP found";
        return false;
    }

    static bool AvdtpGetCapabilities() {
        uint8_t payload[1] = {(uint8_t)(g_remoteSeid << 2)};
        SendAvdtpCommand(AVDTP_GET_CAPABILITIES, payload, 1);
        return WaitAvdtpResponse();
    }

    static bool AvdtpSetConfiguration() {
        // Set Configuration payload:
        //   ACP SEID (1 byte) | INT SEID (1 byte) | Service Capabilities...
        uint8_t payload[12] = {};
        payload[0] = (g_remoteSeid << 2);           // ACP SEID
        payload[1] = (g_localSeid << 2);            // INT SEID

        // Media Transport capability (no data)
        payload[2] = CAT_MEDIA_TRANSPORT;           // Category
        payload[3] = 0;                             // Length

        // Media Codec capability (SBC)
        payload[4] = CAT_MEDIA_CODEC;               // Category
        payload[5] = 6;                             // Length
        payload[6] = (MEDIA_AUDIO << 4);            // Media Type
        payload[7] = CODEC_SBC;                     // Codec Type
        // SBC codec info (4 bytes)
        // Sampling: 44.1kHz (bit 5), Channel mode: Joint Stereo (bit 0)
        payload[8] = 0x21;   // 44.1kHz | Joint Stereo
        // Block length: 16 (bit 7), Subbands: 8 (bit 1), Alloc: Loudness (bit 0)
        payload[9] = 0x83;   // 16 blocks | 8 subbands | Loudness
        payload[10] = 2;     // Min bitpool
        payload[11] = 53;    // Max bitpool

        SendAvdtpCommand(AVDTP_SET_CONFIGURATION, payload, 12);

        if (!WaitAvdtpResponse()) {
            KernelLogStream(WARNING, "BT-A2DP") << "AVDTP SetConfiguration timeout";
            return false;
        }

        g_state = State::Configured;
        KernelLogStream(OK, "BT-A2DP") << "Stream configured";
        return true;
    }

    static bool AvdtpOpen() {
        uint8_t payload[1] = {(uint8_t)(g_remoteSeid << 2)};
        SendAvdtpCommand(AVDTP_OPEN, payload, 1);

        if (!WaitAvdtpResponse()) {
            KernelLogStream(WARNING, "BT-A2DP") << "AVDTP Open timeout";
            return false;
        }

        g_state = State::Open;
        KernelLogStream(OK, "BT-A2DP") << "Stream opened";
        return true;
    }

    static bool AvdtpStart() {
        uint8_t payload[1] = {(uint8_t)(g_remoteSeid << 2)};
        SendAvdtpCommand(AVDTP_START, payload, 1);

        if (!WaitAvdtpResponse()) {
            KernelLogStream(WARNING, "BT-A2DP") << "AVDTP Start timeout";
            return false;
        }

        g_state = State::Streaming;
        KernelLogStream(OK, "BT-A2DP") << "Streaming started";
        return true;
    }

    // =========================================================================
    // OnChannelReady — called by L2CAP when an AVDTP channel is configured
    // =========================================================================

    void OnChannelReady(uint16_t l2capCid) {
        if (g_sigCid == 0) {
            // First AVDTP channel is signaling
            g_sigCid = l2capCid;
            KernelLogStream(OK, "BT-A2DP") << "AVDTP signaling channel ready: CID="
                << (uint64_t)l2capCid;

            // Auto-discover remote SEPs
            g_state = State::Discovering;
            if (AvdtpDiscover()) {
                AvdtpGetCapabilities();
                AvdtpSetConfiguration();
            }
        } else if (g_mediaCid == 0) {
            // Second AVDTP channel is media transport
            g_mediaCid = l2capCid;
            KernelLogStream(OK, "BT-A2DP") << "AVDTP media channel ready: CID="
                << (uint64_t)l2capCid;
        }
    }

    // =========================================================================
    // ProcessAvdtp — handle AVDTP signaling packets
    // =========================================================================

    void ProcessAvdtp(const uint8_t* data, uint16_t len) {
        if (len < 2) return;

        uint8_t txLabel = (data[0] >> 4) & 0x0F;
        uint8_t pktType = (data[0] >> 2) & 0x03;
        uint8_t msgType = data[0] & 0x03;
        uint8_t signalId = data[1] & 0x3F;

        if (msgType == MSG_RESPONSE_ACCEPT || msgType == MSG_RESPONSE_REJECT) {
            // This is a response to our command
            memcpy(g_avdtpResponseBuf, data, len > sizeof(g_avdtpResponseBuf) ? sizeof(g_avdtpResponseBuf) : len);
            g_avdtpResponseLen = len;
            g_avdtpResponseReady = true;
            return;
        }

        // Handle incoming commands
        if (msgType == MSG_COMMAND) {
            switch (signalId) {
                case AVDTP_DISCOVER: {
                    // Respond with our local SEP (audio source)
                    uint8_t rsp[2] = {};
                    rsp[0] = (g_localSeid << 2);  // SEID, not in use
                    rsp[1] = (MEDIA_AUDIO << 4) | 0x00;  // Audio, Source
                    SendAvdtpResponse(txLabel, AVDTP_DISCOVER, rsp, 2);
                    break;
                }

                case AVDTP_GET_CAPABILITIES: {
                    // Respond with our SBC capabilities
                    uint8_t rsp[10] = {};
                    rsp[0] = CAT_MEDIA_TRANSPORT;
                    rsp[1] = 0;
                    rsp[2] = CAT_MEDIA_CODEC;
                    rsp[3] = 6;
                    rsp[4] = (MEDIA_AUDIO << 4);
                    rsp[5] = CODEC_SBC;
                    rsp[6] = 0x21;  // 44.1kHz | Joint Stereo
                    rsp[7] = 0x83;  // 16 blocks | 8 subbands | Loudness
                    rsp[8] = 2;     // Min bitpool
                    rsp[9] = 53;    // Max bitpool
                    SendAvdtpResponse(txLabel, AVDTP_GET_CAPABILITIES, rsp, 10);
                    break;
                }

                case AVDTP_SET_CONFIGURATION: {
                    // Accept configuration from remote
                    if (len >= 4) {
                        g_remoteSeid = (data[2] >> 2) & 0x3F;
                        g_state = State::Configured;
                        SendAvdtpResponse(txLabel, AVDTP_SET_CONFIGURATION, nullptr, 0);
                        KernelLogStream(OK, "BT-A2DP") << "Remote configured stream, SEID="
                            << (uint64_t)g_remoteSeid;
                    }
                    break;
                }

                case AVDTP_OPEN: {
                    g_state = State::Open;
                    SendAvdtpResponse(txLabel, AVDTP_OPEN, nullptr, 0);
                    KernelLogStream(OK, "BT-A2DP") << "Remote opened stream";

                    // The media transport channel will be set up via L2CAP after this
                    break;
                }

                case AVDTP_START: {
                    g_state = State::Streaming;
                    SendAvdtpResponse(txLabel, AVDTP_START, nullptr, 0);
                    KernelLogStream(OK, "BT-A2DP") << "Remote started streaming";
                    break;
                }

                case AVDTP_CLOSE: {
                    g_state = State::Idle;
                    SendAvdtpResponse(txLabel, AVDTP_CLOSE, nullptr, 0);
                    break;
                }

                case AVDTP_SUSPEND: {
                    g_state = State::Open;
                    SendAvdtpResponse(txLabel, AVDTP_SUSPEND, nullptr, 0);
                    break;
                }

                case AVDTP_ABORT: {
                    g_state = State::Idle;
                    SendAvdtpResponse(txLabel, AVDTP_ABORT, nullptr, 0);
                    break;
                }

                default:
                    break;
            }
        }
    }

    // =========================================================================
    // ConfigureStream
    // =========================================================================

    bool ConfigureStream(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
        Sbc::Init(&g_sbcEncoder, sampleRate, channels, bitsPerSample);
        g_sbcInitialized = true;
        g_seqNum = 0;
        g_timestamp = 0;

        KernelLogStream(OK, "BT-A2DP") << "SBC encoder initialized: "
            << (uint64_t)sampleRate << "Hz " << (uint64_t)bitsPerSample << "-bit "
            << (uint64_t)channels << "ch";

        return true;
    }

    // =========================================================================
    // StartStream / StopStream
    // =========================================================================

    bool StartStream() {
        if (g_state == State::Open || g_state == State::Configured) {
            if (g_state == State::Configured) {
                if (!AvdtpOpen()) return false;
            }
            return AvdtpStart();
        }
        return (g_state == State::Streaming);
    }

    bool StopStream() {
        if (g_state == State::Streaming) {
            uint8_t payload[1] = {(uint8_t)(g_remoteSeid << 2)};
            SendAvdtpCommand(AVDTP_SUSPEND, payload, 1);
            WaitAvdtpResponse(1000);
            g_state = State::Open;
        }
        return true;
    }

    // =========================================================================
    // WriteAudio — encode PCM to SBC and stream over Bluetooth
    // =========================================================================

    int WriteAudio(const uint8_t* pcmData, uint32_t pcmLen) {
        if (!g_sbcInitialized || g_state != State::Streaming || g_mediaCid == 0) {
            return -1;
        }

        uint32_t samplesPerFrame = Sbc::GetSamplesPerFrame(&g_sbcEncoder);
        uint32_t bytesPerFrame = samplesPerFrame * g_sbcEncoder.Channels * 2; // 16-bit samples
        uint32_t sbcFrameSize = Sbc::GetFrameSize(&g_sbcEncoder);

        // Apply volume scaling to PCM data
        // We work on a local copy for volume adjustment
        int16_t scaledPcm[512];  // Max ~128 samples * 2 channels = 256 samples
        if (bytesPerFrame > sizeof(scaledPcm)) return -1;

        uint32_t consumed = 0;

        while (consumed + bytesPerFrame <= pcmLen) {
            // Copy and scale by volume
            const int16_t* src = (const int16_t*)(pcmData + consumed);
            uint32_t numSamples = samplesPerFrame * g_sbcEncoder.Channels;
            for (uint32_t i = 0; i < numSamples; i++) {
                scaledPcm[i] = (int16_t)(((int32_t)src[i] * g_volume) / 100);
            }

            // Build media packet: RTP-like header (12 bytes) + SBC payload header (1 byte) + SBC frames
            uint8_t mediaPkt[256] = {};

            // Simplified media packet header (AVDTP media packet)
            // Byte 0: V=2, P=0, X=0, CC=0 -> 0x80
            // Byte 1: M=0, PT=96 -> 0x60
            // Bytes 2-3: Sequence number
            // Bytes 4-7: Timestamp
            // Bytes 8-11: SSRC
            // Byte 12: SBC payload header (number of SBC frames)
            mediaPkt[0] = 0x80;
            mediaPkt[1] = 0x60;
            mediaPkt[2] = (uint8_t)(g_seqNum >> 8);
            mediaPkt[3] = (uint8_t)(g_seqNum & 0xFF);
            mediaPkt[4] = (uint8_t)(g_timestamp >> 24);
            mediaPkt[5] = (uint8_t)(g_timestamp >> 16);
            mediaPkt[6] = (uint8_t)(g_timestamp >> 8);
            mediaPkt[7] = (uint8_t)(g_timestamp & 0xFF);
            mediaPkt[8] = 0; mediaPkt[9] = 0; mediaPkt[10] = 0; mediaPkt[11] = 0x01;  // SSRC
            mediaPkt[12] = 1;  // Number of SBC frames in this packet

            // Encode SBC frame
            uint32_t encodedSize = Sbc::Encode(&g_sbcEncoder, scaledPcm, &mediaPkt[13]);

            uint32_t totalLen = 13 + encodedSize;

            // Send via L2CAP on media channel
            L2cap::SendData(g_mediaCid, mediaPkt, (uint16_t)totalLen);

            g_seqNum++;
            g_timestamp += samplesPerFrame;
            consumed += bytesPerFrame;
        }

        return (int)consumed;
    }

    // =========================================================================
    // State queries
    // =========================================================================

    State GetState() {
        return g_state;
    }

    bool IsStreaming() {
        return (g_state == State::Streaming);
    }

    int GetVolume() {
        return g_volume;
    }

    void SetVolume(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        g_volume = percent;
    }

}
