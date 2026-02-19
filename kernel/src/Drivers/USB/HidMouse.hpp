/*
    * HidMouse.hpp
    * USB HID Boot Protocol Mouse driver
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::HidMouse {

    // Register a mouse device by slot ID
    void RegisterDevice(uint8_t slotId);

    // Process a 3-4 byte boot protocol mouse report
    void ProcessReport(const uint8_t* data, uint16_t length);

};
