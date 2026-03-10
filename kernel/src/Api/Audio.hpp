/*
    * Audio.hpp
    * Audio syscall implementations
    * Routes audio to Intel HDA or Bluetooth A2DP based on handle
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Drivers/Audio/IntelHda.hpp>
#include <Drivers/USB/Bluetooth/Bluetooth.hpp>
#include <Drivers/USB/Bluetooth/A2dp.hpp>

#include "Syscall.hpp"

namespace Montauk {

    // Audio handle convention:
    //   0x00 - 0x0F : Intel HDA handles
    //   0x100       : Bluetooth A2DP audio output

    static constexpr int AUDIO_HANDLE_BT = 0x100;

    static int64_t Sys_AudioOpen(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample) {
        // If HDA is available, use it (primary output)
        if (Drivers::Audio::IntelHda::IsInitialized()) {
            return (int64_t)Drivers::Audio::IntelHda::Open(sampleRate, channels, bitsPerSample);
        }

        // Fallback: try Bluetooth audio if available
        if (Drivers::USB::Bluetooth::IsInitialized()) {
            auto state = Drivers::USB::Bluetooth::A2dp::GetState();
            if (state == Drivers::USB::Bluetooth::A2dp::State::Open ||
                state == Drivers::USB::Bluetooth::A2dp::State::Streaming ||
                state == Drivers::USB::Bluetooth::A2dp::State::Configured) {
                Drivers::USB::Bluetooth::A2dp::ConfigureStream(sampleRate, channels, bitsPerSample);
                Drivers::USB::Bluetooth::A2dp::StartStream();
                return AUDIO_HANDLE_BT;
            }
        }

        return -1;
    }

    static int64_t Sys_AudioClose(int handle) {
        if (handle == AUDIO_HANDLE_BT) {
            Drivers::USB::Bluetooth::A2dp::StopStream();
            return 0;
        }
        Drivers::Audio::IntelHda::Close(handle);
        return 0;
    }

    static int64_t Sys_AudioWrite(int handle, const uint8_t* data, uint32_t size) {
        if (handle == AUDIO_HANDLE_BT) {
            return (int64_t)Drivers::USB::Bluetooth::A2dp::WriteAudio(data, size);
        }
        return (int64_t)Drivers::Audio::IntelHda::Write(handle, data, size);
    }

    static int64_t Sys_AudioCtl(int handle, int cmd, int value) {
        if (handle == AUDIO_HANDLE_BT) {
            switch (cmd) {
                case AUDIO_CTL_SET_VOLUME:
                    Drivers::USB::Bluetooth::A2dp::SetVolume(value);
                    return 0;
                case AUDIO_CTL_GET_VOLUME:
                    return Drivers::USB::Bluetooth::A2dp::GetVolume();
                case AUDIO_CTL_PAUSE:
                    if (value) Drivers::USB::Bluetooth::A2dp::StopStream();
                    else Drivers::USB::Bluetooth::A2dp::StartStream();
                    return 0;
                case AUDIO_CTL_GET_OUTPUT:
                    return 1;  // Bluetooth
                default:
                    return -1;
            }
        }

        // Additional control commands for audio routing
        if (cmd == AUDIO_CTL_GET_OUTPUT) return 0;  // HDA
        if (cmd == AUDIO_CTL_BT_STATUS) {
            if (!Drivers::USB::Bluetooth::IsInitialized()) return 0;
            return (int64_t)Drivers::USB::Bluetooth::A2dp::GetState();
        }

        return (int64_t)Drivers::Audio::IntelHda::Control(handle, cmd, value);
    }

};
