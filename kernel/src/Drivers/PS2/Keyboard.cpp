/*
    * Keyboard.cpp
    * PS/2 Keyboard driver -- Scancode Set 1
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Keyboard.hpp"
#include "PS2Controller.hpp"

#include <Io/IoPort.hpp>
#include <CppLib/Stream.hpp>
#include <CppLib/Spinlock.hpp>
#include <Terminal/Terminal.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Hal/Apic/IoApic.hpp>

namespace Drivers::PS2::Keyboard {

    // Scancode Set 1 to ASCII lookup table (unshifted)
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

    // Scancode Set 1 to ASCII lookup table (shifted)
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

    // Scancode constants for modifier keys (Set 1)
    constexpr uint8_t ScLeftShift    = 0x2A;
    constexpr uint8_t ScRightShift   = 0x36;
    constexpr uint8_t ScLeftCtrl     = 0x1D;
    constexpr uint8_t ScLeftAlt      = 0x38;
    constexpr uint8_t ScCapsLock     = 0x3A;
    constexpr uint8_t ScNumLock      = 0x45;
    constexpr uint8_t ScScrollLock   = 0x46;

    // Break code flag (bit 7 set means key release)
    constexpr uint8_t BreakCodeBit = 0x80;

    // Ring buffer for key events
    static KeyEvent g_KeyBuffer[KeyBufferSize];
    static volatile uint32_t g_BufferHead = 0;
    static volatile uint32_t g_BufferTail = 0;
    static kcp::Spinlock g_BufferLock;

    // Current modifier state
    static ModifierState g_Modifiers = {};

    // Track extended scancode prefix (0xE0)
    static bool g_ExtendedScancode = false;

    static bool IsShiftActive() {
        bool shift = g_Modifiers.LeftShift || g_Modifiers.RightShift;
        bool caps = g_Modifiers.CapsLock;
        return shift ^ caps;
    }

    static void BufferPush(const KeyEvent& event) {
        uint32_t nextHead = (g_BufferHead + 1) & (KeyBufferSize - 1);
        if (nextHead == g_BufferTail) {
            // Buffer full, drop the event
            return;
        }
        g_KeyBuffer[g_BufferHead] = event;
        g_BufferHead = nextHead;
    }

    static bool BufferPop(KeyEvent& event) {
        if (g_BufferTail == g_BufferHead) {
            return false;
        }
        event = g_KeyBuffer[g_BufferTail];
        g_BufferTail = (g_BufferTail + 1) & (KeyBufferSize - 1);
        return true;
    }

    void Initialize() {
        Kt::KernelLogStream(Kt::INFO, "PS2/KB") << "Initializing keyboard driver";

        // Reset keyboard to default state
        // Send 0xFF (Reset) to the keyboard
        Drivers::PS2::SendData(0xFF);
        // Expect 0xFA (ACK) then 0xAA (self-test passed)
        uint8_t ack = Drivers::PS2::ReadData();
        if (ack != 0xFA) {
            Kt::KernelLogStream(Kt::WARNING, "PS2/KB") << "Keyboard reset: unexpected ACK: " << base::hex << (uint64_t)ack;
        }

        uint8_t selfTest = Drivers::PS2::ReadData();
        if (selfTest != 0xAA) {
            Kt::KernelLogStream(Kt::WARNING, "PS2/KB") << "Keyboard self-test: unexpected result: " << base::hex << (uint64_t)selfTest;
        }

        // Enable scanning (in case it was disabled)
        Drivers::PS2::SendData(0xF4);
        ack = Drivers::PS2::ReadData();

        g_Modifiers = {};
        g_BufferHead = 0;
        g_BufferTail = 0;
        g_ExtendedScancode = false;

        // Register IRQ handler and unmask IRQ1 on the IOAPIC
        Hal::RegisterIrqHandler(Hal::IRQ_KEYBOARD, HandleIRQ);
        Hal::IoApic::UnmaskIrq(Hal::IoApic::GetGsiForIrq(Hal::IRQ_KEYBOARD));

        Kt::KernelLogStream(Kt::OK, "PS2/KB") << "Keyboard driver initialized";
    }

    void HandleIRQ(uint8_t irq) {
        (void)irq;
        uint8_t scancode = Io::In8(DataPort);

        // Handle extended scancode prefix
        if (scancode == 0xE0) {
            g_ExtendedScancode = true;
            return;
        }

        bool released = (scancode & BreakCodeBit) != 0;
        uint8_t keycode = scancode & ~BreakCodeBit;

        if (g_ExtendedScancode) {
            g_ExtendedScancode = false;
            // Extended scancodes: Right Ctrl (0x1D), Right Alt (0x38), etc.
            if (keycode == ScLeftCtrl) {
                g_Modifiers.RightCtrl = !released;
            } else if (keycode == ScLeftAlt) {
                g_Modifiers.RightAlt = !released;
            }
            // Extended keys don't produce ASCII characters for now
            KeyEvent event = {
                .Scancode = scancode,
                .Ascii    = 0,
                .Pressed  = !released,
                .Shift    = g_Modifiers.LeftShift || g_Modifiers.RightShift,
                .Ctrl     = g_Modifiers.LeftCtrl || g_Modifiers.RightCtrl,
                .Alt      = g_Modifiers.LeftAlt || g_Modifiers.RightAlt,
                .CapsLock = g_Modifiers.CapsLock
            };
            g_BufferLock.Acquire();
            BufferPush(event);
            g_BufferLock.Release();

            return;
        }

        // Handle modifier keys (update state, but still push event to buffer)
        bool isModifier = false;
        switch (keycode) {
            case ScLeftShift:
                g_Modifiers.LeftShift = !released;
                isModifier = true;
                break;
            case ScRightShift:
                g_Modifiers.RightShift = !released;
                isModifier = true;
                break;
            case ScLeftCtrl:
                g_Modifiers.LeftCtrl = !released;
                isModifier = true;
                break;
            case ScLeftAlt:
                g_Modifiers.LeftAlt = !released;
                isModifier = true;
                break;
            case ScCapsLock:
                if (!released) {
                    g_Modifiers.CapsLock = !g_Modifiers.CapsLock;
                }
                isModifier = true;
                break;
            case ScNumLock:
                if (!released) {
                    g_Modifiers.NumLock = !g_Modifiers.NumLock;
                }
                isModifier = true;
                break;
            case ScScrollLock:
                if (!released) {
                    g_Modifiers.ScrollLock = !g_Modifiers.ScrollLock;
                }
                isModifier = true;
                break;
            default:
                break;
        }

        // Modifiers still need events in the buffer (for apps like doom)
        // but lock keys (caps/num/scroll) don't need buffer events
        if (isModifier && keycode != ScCapsLock && keycode != ScNumLock && keycode != ScScrollLock) {
            KeyEvent event = {
                .Scancode = scancode,
                .Ascii    = 0,
                .Pressed  = !released,
                .Shift    = g_Modifiers.LeftShift || g_Modifiers.RightShift,
                .Ctrl     = g_Modifiers.LeftCtrl || g_Modifiers.RightCtrl,
                .Alt      = g_Modifiers.LeftAlt || g_Modifiers.RightAlt,
                .CapsLock = g_Modifiers.CapsLock
            };
            g_BufferLock.Acquire();
            BufferPush(event);
            g_BufferLock.Release();
            return;
        }
        if (isModifier) return;

        // Translate scancode to ASCII
        char ascii = 0;
        if (keycode < 128) {
            if (IsShiftActive()) {
                ascii = g_ScancodeToAsciiShifted[keycode];
            } else {
                ascii = g_ScancodeToAscii[keycode];
            }
        }

        KeyEvent event = {
            .Scancode = scancode,
            .Ascii    = ascii,
            .Pressed  = !released,
            .Shift    = g_Modifiers.LeftShift || g_Modifiers.RightShift,
            .Ctrl     = g_Modifiers.LeftCtrl || g_Modifiers.RightCtrl,
            .Alt      = g_Modifiers.LeftAlt || g_Modifiers.RightAlt,
            .CapsLock = g_Modifiers.CapsLock
        };

        g_BufferLock.Acquire();
        BufferPush(event);
        g_BufferLock.Release();
    }

    bool IsKeyAvailable() {
        return g_BufferHead != g_BufferTail;
    }

    KeyEvent GetKey() {
        KeyEvent event = {};
        g_BufferLock.Acquire();
        BufferPop(event);
        g_BufferLock.Release();
        return event;
    }

    char GetChar() {
        // Blocks until a printable key press is available
        while (true) {
            if (IsKeyAvailable()) {
                KeyEvent event = GetKey();
                if (event.Pressed && event.Ascii != 0) {
                    return event.Ascii;
                }
            }
            // Yield the CPU while waiting
            asm volatile("hlt");
        }
    }

    const ModifierState& GetModifiers() {
        return g_Modifiers;
    }

    void InjectKeyEvent(const KeyEvent& event) {
        g_BufferLock.Acquire();
        BufferPush(event);
        g_BufferLock.Release();
    }

};