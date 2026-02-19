/*
    * Keyboard.hpp
    * PS/2 Keyboard driver
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::PS2::Keyboard {

    // Key event structure
    struct KeyEvent {
        uint8_t Scancode;
        char    Ascii;
        bool    Pressed;
        bool    Shift;
        bool    Ctrl;
        bool    Alt;
        bool    CapsLock;
    };

    // Modifier key state
    struct ModifierState {
        bool LeftShift;
        bool RightShift;
        bool LeftCtrl;
        bool RightCtrl;
        bool LeftAlt;
        bool RightAlt;
        bool CapsLock;
        bool NumLock;
        bool ScrollLock;
    };

    // Ring buffer size (must be power of 2)
    constexpr uint32_t KeyBufferSize = 256;

    void Initialize();

    // Interrupt handler -- called from IRQ dispatch (EOI is sent automatically)
    void HandleIRQ(uint8_t irq);

    // Public interface for consuming key events
    bool IsKeyAvailable();
    KeyEvent GetKey();
    char GetChar();

    // Modifier state query
    const ModifierState& GetModifiers();

    // Inject a key event from an external source (e.g., USB HID keyboard)
    void InjectKeyEvent(const KeyEvent& event);

};