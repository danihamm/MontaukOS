/*
    * HidMouse.cpp
    * USB HID Boot Protocol Mouse driver
    * Copyright (c) 2025 Daniel Hammer
*/

#include "HidMouse.hpp"
#include <Drivers/PS2/Mouse.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Drivers::USB::HidMouse {

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    static uint8_t g_SlotId = 0;

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void RegisterDevice(uint8_t slotId) {
        g_SlotId = slotId;

        KernelLogStream(OK, "USB/Mouse") << "Registered HID mouse on slot " << (uint64_t)slotId;
    }

    void ProcessReport(const uint8_t* data, uint16_t length) {
        if (length < 3) return;

        // Boot protocol mouse report:
        //   Byte 0: Buttons (bit 0 = left, bit 1 = right, bit 2 = middle)
        //   Byte 1: X displacement (signed int8_t)
        //   Byte 2: Y displacement (signed int8_t)
        //   Byte 3: Scroll wheel (signed int8_t, optional)

        uint8_t buttons = data[0] & 0x07;
        int8_t deltaX   = (int8_t)data[1];
        int8_t deltaY   = (int8_t)data[2];
        int8_t scroll   = (length >= 4) ? (int8_t)data[3] : 0;

        Drivers::PS2::Mouse::InjectMouseReport(buttons, deltaX, deltaY, scroll);
    }

}
