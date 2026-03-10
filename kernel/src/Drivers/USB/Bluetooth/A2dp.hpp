/*
    * A2dp.hpp
    * Bluetooth A2DP (Advanced Audio Distribution Profile)
    * AVDTP signaling and SBC audio streaming
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::Bluetooth::A2dp {

    // =========================================================================
    // A2DP stream states
    // =========================================================================

    enum class State {
        Idle,           // No A2DP connection
        Discovering,    // AVDTP discover in progress
        Configured,     // Stream endpoint configured
        Open,           // Stream open, ready for audio
        Streaming       // Actively streaming audio
    };

    // =========================================================================
    // Public API
    // =========================================================================

    // Called by L2CAP when an AVDTP channel becomes ready
    void OnChannelReady(uint16_t l2capCid);

    // Process an AVDTP signaling packet
    void ProcessAvdtp(const uint8_t* data, uint16_t len);

    // Configure a stream for the given PCM parameters
    bool ConfigureStream(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);

    // Start streaming
    bool StartStream();

    // Stop streaming
    bool StopStream();

    // Write PCM audio data to the Bluetooth audio stream
    // Returns number of bytes consumed
    int WriteAudio(const uint8_t* pcmData, uint32_t pcmLen);

    // Get current state
    State GetState();

    // Check if currently streaming
    bool IsStreaming();

    // Get volume (0-100)
    int GetVolume();

    // Set volume (0-100)
    void SetVolume(int percent);

}
