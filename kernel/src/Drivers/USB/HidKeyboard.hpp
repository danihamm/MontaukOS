/*
    * HidKeyboard.hpp
    * USB HID Boot Protocol Keyboard driver
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::HidKeyboard {

    // Register a keyboard device by slot ID
    void RegisterDevice(uint8_t slotId);

    // Process an 8-byte boot protocol keyboard report
    void ProcessReport(const uint8_t* data, uint16_t length);

};
