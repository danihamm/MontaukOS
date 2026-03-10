/*
    * Sbc.hpp
    * SBC (Sub-Band Codec) encoder for Bluetooth A2DP
    * Fixed-point implementation (no FPU/SSE required)
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::Bluetooth::Sbc {

    // =========================================================================
    // SBC configuration
    // =========================================================================

    // Standard SBC parameters for A2DP
    constexpr int SBC_SUBBANDS   = 8;
    constexpr int SBC_BLOCKS     = 16;
    constexpr int SBC_CHANNELS   = 2;    // Stereo
    constexpr int SBC_BITPOOL    = 53;   // Standard quality

    // Allocation method
    constexpr uint8_t ALLOC_SNR    = 0;
    constexpr uint8_t ALLOC_LOUDNESS = 1;

    // Channel mode
    constexpr uint8_t MODE_MONO         = 0;
    constexpr uint8_t MODE_DUAL_CHANNEL = 1;
    constexpr uint8_t MODE_STEREO       = 2;
    constexpr uint8_t MODE_JOINT_STEREO = 3;

    // Sampling frequency
    constexpr uint8_t FREQ_16000  = 0;
    constexpr uint8_t FREQ_32000  = 1;
    constexpr uint8_t FREQ_44100  = 2;
    constexpr uint8_t FREQ_48000  = 3;

    // SBC frame header
    struct SbcHeader {
        uint8_t SyncWord;       // 0x9C
        uint8_t Config;         // freq(2) | blocks(2) | mode(2) | alloc(1) | subbands(1)
        uint8_t Bitpool;
        uint8_t Crc;
    } __attribute__((packed));

    // =========================================================================
    // Encoder state
    // =========================================================================

    struct SbcEncoder {
        uint8_t  Frequency;
        uint8_t  Blocks;
        uint8_t  ChannelMode;
        uint8_t  AllocMethod;
        uint8_t  Subbands;
        uint8_t  Bitpool;
        uint8_t  Channels;

        // Analysis filter state (per-channel windowed buffer)
        int32_t  X[SBC_CHANNELS][SBC_SUBBANDS * 10];
        int      XPos[SBC_CHANNELS];

        // Computed frame size in bytes
        uint32_t FrameSize;

        // Samples per frame
        uint32_t SamplesPerFrame;  // blocks * subbands
    };

    // =========================================================================
    // Public API
    // =========================================================================

    // Initialize encoder with given parameters
    void Init(SbcEncoder* enc, uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);

    // Encode one SBC frame from PCM data
    // pcm: interleaved 16-bit signed PCM, length = blocks * subbands * channels
    // out: output buffer for SBC frame
    // Returns number of bytes written to out
    uint32_t Encode(SbcEncoder* enc, const int16_t* pcm, uint8_t* out);

    // Get the frame size in bytes for the current configuration
    uint32_t GetFrameSize(const SbcEncoder* enc);

    // Get the number of PCM samples consumed per frame (per channel)
    uint32_t GetSamplesPerFrame(const SbcEncoder* enc);

}
