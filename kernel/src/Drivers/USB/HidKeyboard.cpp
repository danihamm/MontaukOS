/*
    * HidKeyboard.cpp
    * USB HID Boot Protocol Keyboard driver
    * Copyright (c) 2025 Daniel Hammer
*/

#include "HidKeyboard.hpp"
#include <Drivers/PS2/Keyboard.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Drivers::USB::HidKeyboard {

    // -------------------------------------------------------------------------
    // USB HID Usage ID -> PS/2 Scancode Set 1 translation table
    // Index = USB HID Usage ID, Value = PS/2 Scancode Set 1 make code
    // -------------------------------------------------------------------------

    static const uint8_t g_HidToScancode[256] = {
        // 0x00 - 0x03: No Event, Error Roll Over, POST Fail, Error Undefined
        0x00, 0x00, 0x00, 0x00,
        // 0x04 - 0x1D: Letters a-z
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23,  // a b c d e f g h
        0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,  // i j k l m n o p
        0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,  // q r s t u v w x
        0x15, 0x2C,                                         // y z
        // 0x1E - 0x27: Digits 1-9, 0
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,  // 1 2 3 4 5 6 7 8
        0x0A, 0x0B,                                         // 9 0
        // 0x28 - 0x38: Special keys
        0x1C,  // 0x28 Enter
        0x01,  // 0x29 Escape
        0x0E,  // 0x2A Backspace
        0x0F,  // 0x2B Tab
        0x39,  // 0x2C Space
        0x0C,  // 0x2D Minus
        0x0D,  // 0x2E Equal
        0x1A,  // 0x2F Left Bracket
        0x1B,  // 0x30 Right Bracket
        0x2B,  // 0x31 Backslash
        0x2B,  // 0x32 Non-US # (same as backslash)
        0x27,  // 0x33 Semicolon
        0x28,  // 0x34 Apostrophe
        0x29,  // 0x35 Grave Accent / Tilde
        0x33,  // 0x36 Comma
        0x34,  // 0x37 Period
        0x35,  // 0x38 Slash
        // 0x39: Caps Lock
        0x3A,
        // 0x3A - 0x45: F1 - F12
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,  // F1 - F6
        0x41, 0x42, 0x43, 0x44, 0x57, 0x58,  // F7 - F12
        // 0x46: Print Screen, 0x47: Scroll Lock, 0x48: Pause
        0x00, 0x46, 0x00,
        // 0x49 - 0x4E: Insert, Home, Page Up, Delete, End, Page Down
        0x52, 0x47, 0x49, 0x53, 0x4F, 0x51,
        // 0x4F - 0x52: Arrow keys (Right, Left, Down, Up)
        0x4D, 0x4B, 0x50, 0x48,
        // 0x53: Num Lock
        0x45,
        // 0x54 - 0x63: Keypad
        0x35,  // 0x54 KP /
        0x37,  // 0x55 KP *
        0x4A,  // 0x56 KP -
        0x4E,  // 0x57 KP +
        0x1C,  // 0x58 KP Enter
        0x4F,  // 0x59 KP 1
        0x50,  // 0x5A KP 2
        0x51,  // 0x5B KP 3
        0x4B,  // 0x5C KP 4
        0x4C,  // 0x5D KP 5
        0x4D,  // 0x5E KP 6
        0x47,  // 0x5F KP 7
        0x48,  // 0x60 KP 8
        0x49,  // 0x61 KP 9
        0x52,  // 0x62 KP 0
        0x53,  // 0x63 KP .
        // 0x64 - 0xFF: remaining entries are zero (non-US backslash, application, etc.)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x64 - 0x6B
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x6C - 0x73
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x74 - 0x7B
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x7C - 0x83
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x84 - 0x8B
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x8C - 0x93
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x94 - 0x9B
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0x9C - 0xA3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xA4 - 0xAB
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xAC - 0xB3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xB4 - 0xBB
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xBC - 0xC3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xC4 - 0xCB
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xCC - 0xD3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xD4 - 0xDB
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xDC - 0xE3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xE4 - 0xEB
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xEC - 0xF3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 0xF4 - 0xFB
        0x00, 0x00, 0x00, 0x00                             // 0xFC - 0xFF
    };

    // -------------------------------------------------------------------------
    // Scancode Set 1 -> ASCII lookup tables (from PS/2 keyboard driver)
    // -------------------------------------------------------------------------

    static const char g_ScancodeToAscii[128] = {
        0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   // 0x00 - 0x07
        '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',  // 0x08 - 0x0F
        'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',   // 0x10 - 0x17
        'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',   // 0x18 - 0x1F
        'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',   // 0x20 - 0x27
        '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',   // 0x28 - 0x2F
        'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',   // 0x30 - 0x37
        0,    ' ',  0,    0,    0,    0,    0,    0,     // 0x38 - 0x3F
        0,    0,    0,    0,    0,    0,    0,    '7',   // 0x40 - 0x47
        '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',   // 0x48 - 0x4F
        '2',  '3',  '0',  '.',  0,    0,    0,    0,     // 0x50 - 0x57
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x58 - 0x5F
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x60 - 0x67
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x68 - 0x6F
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x70 - 0x77
        0,    0,    0,    0,    0,    0,    0,    0      // 0x78 - 0x7F
    };

    static const char g_ScancodeToAsciiShifted[128] = {
        0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',   // 0x00 - 0x07
        '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',  // 0x08 - 0x0F
        'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',   // 0x10 - 0x17
        'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',   // 0x18 - 0x1F
        'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',   // 0x20 - 0x27
        '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',   // 0x28 - 0x2F
        'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',   // 0x30 - 0x37
        0,    ' ',  0,    0,    0,    0,    0,    0,     // 0x38 - 0x3F
        0,    0,    0,    0,    0,    0,    0,    '7',   // 0x40 - 0x47
        '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',   // 0x48 - 0x4F
        '2',  '3',  '0',  '.',  0,    0,    0,    0,     // 0x50 - 0x57
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x58 - 0x5F
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x60 - 0x67
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x68 - 0x6F
        0,    0,    0,    0,    0,    0,    0,    0,     // 0x70 - 0x77
        0,    0,    0,    0,    0,    0,    0,    0      // 0x78 - 0x7F
    };

    // -------------------------------------------------------------------------
    // USB HID modifier byte bit definitions
    // -------------------------------------------------------------------------

    constexpr uint8_t MOD_LEFT_CTRL   = (1 << 0);
    constexpr uint8_t MOD_LEFT_SHIFT  = (1 << 1);
    constexpr uint8_t MOD_LEFT_ALT    = (1 << 2);
    constexpr uint8_t MOD_LEFT_GUI    = (1 << 3);
    constexpr uint8_t MOD_RIGHT_CTRL  = (1 << 4);
    constexpr uint8_t MOD_RIGHT_SHIFT = (1 << 5);
    constexpr uint8_t MOD_RIGHT_ALT   = (1 << 6);
    constexpr uint8_t MOD_RIGHT_GUI   = (1 << 7);

    // PS/2 scancodes for modifier keys (for synthetic events)
    constexpr uint8_t SC_LEFT_CTRL    = 0x1D;
    constexpr uint8_t SC_LEFT_SHIFT   = 0x2A;
    constexpr uint8_t SC_LEFT_ALT     = 0x38;
    constexpr uint8_t SC_RIGHT_CTRL   = 0x1D;  // Extended in PS/2, but we use same base
    constexpr uint8_t SC_RIGHT_SHIFT  = 0x36;
    constexpr uint8_t SC_RIGHT_ALT    = 0x38;  // Extended in PS/2

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    static uint8_t g_SlotId = 0;
    static uint8_t g_PrevKeys[6] = {};
    static uint8_t g_PrevModifiers = 0;

    // Typematic repeat state
    static uint8_t  g_RepeatKey = 0;     // HID usage ID currently repeating
    static uint16_t g_HoldCount = 0;     // Reports since key was first held

    // Tuned for ~16ms report interval (SET_IDLE(4))
    constexpr uint16_t TYPEMATIC_DELAY  = 31;  // ~500ms before repeat starts
    constexpr uint16_t TYPEMATIC_PERIOD = 2;   // ~32ms between repeats (~31 chars/sec)

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    // Check if a key usage ID is present in a 6-key array
    static bool KeyInArray(uint8_t key, const uint8_t* arr) {
        for (int i = 0; i < 6; i++) {
            if (arr[i] == key) return true;
        }
        return false;
    }

    // Check if a HID usage ID is a non-character key (arrows, nav, F-keys, etc.)
    // These share scancodes with keypad numbers but must not produce ASCII.
    static bool IsNonCharKey(uint8_t hidUsage) {
        return (hidUsage >= 0x39 && hidUsage <= 0x53);
    }

    // Build and inject a KeyEvent into the PS/2 keyboard buffer
    static void InjectKey(uint8_t scancode, bool pressed, uint8_t modifiers,
                          bool nonChar = false) {
        bool shift = (modifiers & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0;
        bool ctrl  = (modifiers & (MOD_LEFT_CTRL | MOD_RIGHT_CTRL)) != 0;
        bool alt   = (modifiers & (MOD_LEFT_ALT | MOD_RIGHT_ALT)) != 0;

        char ascii = 0;
        if (!nonChar && scancode < 128 && pressed) {
            if (shift) {
                ascii = g_ScancodeToAsciiShifted[scancode];
            } else {
                ascii = g_ScancodeToAscii[scancode];
            }
        }

        Drivers::PS2::Keyboard::KeyEvent event = {
            .Scancode = scancode,
            .Ascii    = ascii,
            .Pressed  = pressed,
            .Shift    = shift,
            .Ctrl     = ctrl,
            .Alt      = alt,
            .CapsLock = false
        };

        Drivers::PS2::Keyboard::InjectKeyEvent(event);
    }

    // Inject a modifier key event
    static void InjectModifierKey(uint8_t scancode, bool pressed, uint8_t modifiers) {
        Drivers::PS2::Keyboard::KeyEvent event = {
            .Scancode = scancode,
            .Ascii    = 0,
            .Pressed  = pressed,
            .Shift    = (modifiers & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0,
            .Ctrl     = (modifiers & (MOD_LEFT_CTRL | MOD_RIGHT_CTRL)) != 0,
            .Alt      = (modifiers & (MOD_LEFT_ALT | MOD_RIGHT_ALT)) != 0,
            .CapsLock = false
        };

        Drivers::PS2::Keyboard::InjectKeyEvent(event);
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void RegisterDevice(uint8_t slotId) {
        g_SlotId = slotId;

        // Reset state
        for (int i = 0; i < 6; i++) g_PrevKeys[i] = 0;
        g_PrevModifiers = 0;
        g_RepeatKey = 0;
        g_HoldCount = 0;

        KernelLogStream(OK, "USB/KB") << "Registered HID keyboard on slot " << (uint64_t)slotId;
    }

    void ProcessReport(const uint8_t* data, uint16_t length) {
        if (length < 8) return;

        uint8_t modifiers   = data[0];
        // data[1] is reserved
        const uint8_t* keys = &data[2];

        // -----------------------------------------------------------------
        // Handle modifier changes
        // -----------------------------------------------------------------
        uint8_t modChanged = modifiers ^ g_PrevModifiers;

        if (modChanged & MOD_LEFT_CTRL) {
            InjectModifierKey(SC_LEFT_CTRL, (modifiers & MOD_LEFT_CTRL) != 0, modifiers);
        }
        if (modChanged & MOD_LEFT_SHIFT) {
            InjectModifierKey(SC_LEFT_SHIFT, (modifiers & MOD_LEFT_SHIFT) != 0, modifiers);
        }
        if (modChanged & MOD_LEFT_ALT) {
            InjectModifierKey(SC_LEFT_ALT, (modifiers & MOD_LEFT_ALT) != 0, modifiers);
        }
        if (modChanged & MOD_RIGHT_CTRL) {
            InjectModifierKey(SC_RIGHT_CTRL, (modifiers & MOD_RIGHT_CTRL) != 0, modifiers);
        }
        if (modChanged & MOD_RIGHT_SHIFT) {
            InjectModifierKey(SC_RIGHT_SHIFT, (modifiers & MOD_RIGHT_SHIFT) != 0, modifiers);
        }
        if (modChanged & MOD_RIGHT_ALT) {
            InjectModifierKey(SC_RIGHT_ALT, (modifiers & MOD_RIGHT_ALT) != 0, modifiers);
        }

        // -----------------------------------------------------------------
        // Detect newly pressed keys (in current but not in previous)
        // -----------------------------------------------------------------
        bool newKeyPressed = false;
        for (int i = 0; i < 6; i++) {
            uint8_t key = keys[i];
            if (key == 0) continue;

            if (!KeyInArray(key, g_PrevKeys)) {
                uint8_t scancode = g_HidToScancode[key];
                if (scancode != 0) {
                    InjectKey(scancode, true, modifiers, IsNonCharKey(key));
                    g_RepeatKey = key;
                    g_HoldCount = 0;
                    newKeyPressed = true;
                }
            }
        }

        // -----------------------------------------------------------------
        // Detect released keys (in previous but not in current)
        // -----------------------------------------------------------------
        for (int i = 0; i < 6; i++) {
            uint8_t key = g_PrevKeys[i];
            if (key == 0) continue;

            if (!KeyInArray(key, keys)) {
                uint8_t scancode = g_HidToScancode[key];
                if (scancode != 0) {
                    InjectKey(scancode, false, modifiers, IsNonCharKey(key));
                }
                if (key == g_RepeatKey) {
                    g_RepeatKey = 0;
                    g_HoldCount = 0;
                }
            }
        }

        // -----------------------------------------------------------------
        // Typematic repeat for held keys
        // -----------------------------------------------------------------
        if (!newKeyPressed && g_RepeatKey != 0 && KeyInArray(g_RepeatKey, keys)) {
            g_HoldCount++;
            if (g_HoldCount >= TYPEMATIC_DELAY &&
                (g_HoldCount - TYPEMATIC_DELAY) % TYPEMATIC_PERIOD == 0) {
                uint8_t scancode = g_HidToScancode[g_RepeatKey];
                if (scancode != 0) {
                    InjectKey(scancode, true, modifiers, IsNonCharKey(g_RepeatKey));
                }
            }
        }

        // -----------------------------------------------------------------
        // Save current state for next report
        // -----------------------------------------------------------------
        for (int i = 0; i < 6; i++) g_PrevKeys[i] = keys[i];
        g_PrevModifiers = modifiers;
    }

}
