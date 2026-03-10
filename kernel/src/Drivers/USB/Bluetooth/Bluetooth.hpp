/*
    * Bluetooth.hpp
    * Top-level Bluetooth subsystem header
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include "Hci.hpp"

namespace Drivers::USB::Bluetooth {

    // Called by USB enumeration when a Bluetooth adapter is detected
    void RegisterAdapter(uint8_t slotId);

    // Query adapter state
    bool IsInitialized();
    uint8_t GetSlotId();

    // Get Bluetooth device address (6 bytes)
    const uint8_t* GetBdAddr();

    // Scan for nearby devices (blocking, up to timeoutMs)
    // Returns number of devices found; results written to buf
    int Scan(Hci::InquiryDevice* buf, int maxCount, uint32_t timeoutMs);

    // Initiate connection to a remote device by BD_ADDR
    // Returns 0 on success (connection established), -1 on failure
    int Connect(const uint8_t* bdAddr, uint32_t timeoutMs = 10000);

    // Disconnect a device by BD_ADDR
    // Returns 0 on success, -1 if not connected
    int Disconnect(const uint8_t* bdAddr);

    // List connected devices
    // Returns number of connected devices; info written to buf
    int ListConnected(Hci::ConnectionInfo* buf, int maxCount);

}
