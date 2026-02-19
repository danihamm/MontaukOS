/*
    * Mouse.cpp
    * PS/2 Mouse driver with optional scroll wheel support
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Mouse.hpp"
#include "PS2Controller.hpp"

#include <Io/IoPort.hpp>
#include <CppLib/Stream.hpp>
#include <CppLib/Spinlock.hpp>
#include <Terminal/Terminal.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Hal/Apic/IoApic.hpp>

namespace Drivers::PS2::Mouse {

    // Mouse protocol commands
    constexpr uint8_t CmdSetDefaults       = 0xF6;
    constexpr uint8_t CmdEnableReporting   = 0xF4;
    constexpr uint8_t CmdDisableReporting  = 0xF5;
    constexpr uint8_t CmdSetSampleRate     = 0xF3;
    constexpr uint8_t CmdGetDeviceId       = 0xF2;
    constexpr uint8_t CmdAck               = 0xFA;

    // Mouse packet byte 0 bit fields
    constexpr uint8_t PacketYOverflow = (1 << 7);
    constexpr uint8_t PacketXOverflow = (1 << 6);
    constexpr uint8_t PacketYSign     = (1 << 5);
    constexpr uint8_t PacketXSign     = (1 << 4);
    constexpr uint8_t PacketAlwaysOne = (1 << 3);

    // Mouse state
    static MouseState g_State = {};
    static kcp::Spinlock g_StateLock;

    // Screen bounds
    static int32_t g_MaxX = 1024;
    static int32_t g_MaxY = 768;

    // Packet assembly state
    static uint8_t g_PacketBuffer[4] = {};
    static uint8_t g_PacketIndex = 0;
    static bool    g_HasScrollWheel = false;
    static uint8_t g_PacketSize = 3;

    static uint8_t SendMouseCommand(uint8_t command) {
        Drivers::PS2::SendToPort2(command);
        return Drivers::PS2::ReadData();
    }

    static void SetSampleRate(uint8_t rate) {
        SendMouseCommand(CmdSetSampleRate);
        SendMouseCommand(rate);
    }

    static bool DetectScrollWheel() {
        // Magic sequence to enable scroll wheel: set sample rate 200, 100, 80
        // then query device ID. If it returns 3, scroll wheel is present.
        SetSampleRate(200);
        SetSampleRate(100);
        SetSampleRate(80);

        SendMouseCommand(CmdGetDeviceId);
        uint8_t deviceId = Drivers::PS2::ReadData();

        return (deviceId == 3);
    }

    void Initialize() {
        Kt::KernelLogStream(Kt::INFO, "PS2/Mouse") << "Initializing mouse driver";

        if (!Drivers::PS2::IsDualChannel()) {
            Kt::KernelLogStream(Kt::WARNING, "PS2/Mouse") << "PS/2 controller is not dual-channel, mouse unavailable";
            return;
        }

        // Set defaults
        uint8_t ack = SendMouseCommand(CmdSetDefaults);
        if (ack != CmdAck) {
            Kt::KernelLogStream(Kt::WARNING, "PS2/Mouse") << "Set defaults: unexpected response: " << base::hex << (uint64_t)ack;
        }

        // Try to enable scroll wheel
        g_HasScrollWheel = DetectScrollWheel();
        if (g_HasScrollWheel) {
            g_PacketSize = 4;
            Kt::KernelLogStream(Kt::OK, "PS2/Mouse") << "Scroll wheel detected";
        } else {
            g_PacketSize = 3;
            Kt::KernelLogStream(Kt::INFO, "PS2/Mouse") << "Standard 3-byte mouse protocol";
        }

        // Enable data reporting
        ack = SendMouseCommand(CmdEnableReporting);
        if (ack != CmdAck) {
            Kt::KernelLogStream(Kt::WARNING, "PS2/Mouse") << "Enable reporting: unexpected response: " << base::hex << (uint64_t)ack;
        }

        g_State = {};
        g_PacketIndex = 0;

        // Register IRQ handler and unmask IRQ12 on the IOAPIC
        Hal::RegisterIrqHandler(Hal::IRQ_MOUSE, HandleIRQ);
        Hal::IoApic::UnmaskIrq(Hal::IoApic::GetGsiForIrq(Hal::IRQ_MOUSE));

        Kt::KernelLogStream(Kt::OK, "PS2/Mouse") << "Mouse driver initialized";
    }

    void HandleIRQ(uint8_t irq) {
        (void)irq;
        uint8_t data = Io::In8(DataPort);

        // Synchronization: byte 0 must always have bit 3 set
        if (g_PacketIndex == 0 && !(data & PacketAlwaysOne)) {
            // Out of sync, discard and wait for a valid start byte
            return;
        }

        g_PacketBuffer[g_PacketIndex] = data;
        g_PacketIndex++;

        if (g_PacketIndex < g_PacketSize) {
            return;
        }

        // Full packet received, process it
        g_PacketIndex = 0;

        uint8_t flags = g_PacketBuffer[0];
        uint8_t buttons = flags & 0x07;

        // Check for overflow -- discard the packet if overflow is set
        if (flags & (PacketXOverflow | PacketYOverflow)) {
            return;
        }

        // Reconstruct signed X and Y deltas with sign extension
        int32_t deltaX = (int32_t)g_PacketBuffer[1];
        int32_t deltaY = (int32_t)g_PacketBuffer[2];

        if (flags & PacketXSign) {
            deltaX |= (int32_t)0xFFFFFF00;
        }
        if (flags & PacketYSign) {
            deltaY |= (int32_t)0xFFFFFF00;
        }

        // PS/2 mouse Y axis is inverted (positive = up)
        deltaY = -deltaY;

        // Scroll wheel delta (4th byte, signed)
        int32_t scrollDelta = 0;
        if (g_HasScrollWheel && g_PacketSize == 4) {
            scrollDelta = (int8_t)g_PacketBuffer[3];
        }

        g_StateLock.Acquire();

        g_State.X += deltaX;
        g_State.Y += deltaY;
        g_State.Buttons = buttons;
        g_State.ScrollDelta = scrollDelta;

        // Clamp to screen bounds
        if (g_State.X < 0) g_State.X = 0;
        if (g_State.Y < 0) g_State.Y = 0;
        if (g_State.X > g_MaxX) g_State.X = g_MaxX;
        if (g_State.Y > g_MaxY) g_State.Y = g_MaxY;

        g_StateLock.Release();
    }

    MouseState GetMouseState() {
        g_StateLock.Acquire();
        MouseState state = g_State;
        g_StateLock.Release();
        return state;
    }

    int32_t GetX() {
        return g_State.X;
    }

    int32_t GetY() {
        return g_State.Y;
    }

    uint8_t GetButtons() {
        return g_State.Buttons;
    }

    void SetBounds(int32_t maxX, int32_t maxY) {
        g_MaxX = maxX;
        g_MaxY = maxY;
    }

    void InjectMouseReport(uint8_t buttons, int8_t deltaX, int8_t deltaY, int8_t scroll) {
        g_StateLock.Acquire();

        g_State.X += (int32_t)deltaX;
        g_State.Y += (int32_t)deltaY;
        g_State.Buttons = buttons;
        g_State.ScrollDelta = (int32_t)scroll;

        // Clamp to screen bounds
        if (g_State.X < 0) g_State.X = 0;
        if (g_State.Y < 0) g_State.Y = 0;
        if (g_State.X > g_MaxX) g_State.X = g_MaxX;
        if (g_State.Y > g_MaxY) g_State.Y = g_MaxY;

        g_StateLock.Release();
    }

};