/*
    * Mouse.hpp
    * PS/2 Mouse driver
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::PS2::Mouse {

    // Mouse button flags
    constexpr uint8_t ButtonLeft   = (1 << 0);
    constexpr uint8_t ButtonRight  = (1 << 1);
    constexpr uint8_t ButtonMiddle = (1 << 2);

    struct MouseState {
        int32_t  X;
        int32_t  Y;
        int32_t  ScrollDelta;
        uint8_t  Buttons;
    };

    void Initialize();

    // Interrupt handler -- called from IRQ dispatch (EOI is sent automatically)
    void HandleIRQ(uint8_t irq);

    // Public interface
    MouseState GetMouseState();
    int32_t GetX();
    int32_t GetY();
    uint8_t GetButtons();

    void SetBounds(int32_t maxX, int32_t maxY);

    // Inject a mouse report from an external source (e.g., USB HID mouse)
    void InjectMouseReport(uint8_t buttons, int8_t deltaX, int8_t deltaY, int8_t scroll);

};