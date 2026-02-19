/*
    * ApicTimer.cpp
    * Local APIC timer: PIT-calibrated periodic tick for timekeeping
    * Copyright (c) 2025 Daniel Hammer
*/

#include "ApicTimer.hpp"
#include <Hal/Apic/Apic.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Io/IoPort.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Sched/Scheduler.hpp>
#include <Drivers/Net/E1000E.hpp>

using namespace Kt;

namespace Timekeeping {
    // PIT constants
    static constexpr uint32_t PIT_FREQUENCY = 1193182;     // PIT oscillator frequency in Hz
    static constexpr uint16_t PIT_CHANNEL2_DATA = 0x42;
    static constexpr uint16_t PIT_COMMAND = 0x43;
    static constexpr uint16_t PIT_GATE_PORT = 0x61;

    // APIC timer LVT mode bits
    static constexpr uint32_t LVT_MASKED   = (1 << 16);
    static constexpr uint32_t LVT_PERIODIC = (1 << 17);

    // APIC timer divide configuration values
    static constexpr uint32_t DIVIDE_BY_16 = 0x03;

    // Timer tick rate: 1000 Hz (1 ms per tick)
    static constexpr uint32_t TIMER_HZ = 1000;

    // Global state
    static volatile uint64_t g_tickCount = 0;
    static uint32_t g_ticksPerMs = 0;

    static bool g_schedEnabled = false;

    // Timer IRQ handler: increment tick count, poll NIC, and drive scheduler
    static void TimerHandler(uint8_t) {
        g_tickCount = g_tickCount + 1;

        // In polling mode, drain the NIC's RX ring from timer context
        // (equivalent to a real NIC IRQ handler, runs with interrupts disabled)
        Drivers::Net::E1000E::Poll();

        if (g_schedEnabled) {
            Sched::Tick();
        }
    }

    // Use PIT channel 2 to create a precise delay for calibration.
    // Returns the number of APIC timer ticks that elapsed during ~10ms.
    static uint32_t CalibratePit() {
        // We want to measure ~10ms worth of APIC timer ticks.
        // PIT count for 10ms: PIT_FREQUENCY / 100 = 11931
        uint16_t pitCount = PIT_FREQUENCY / 100;

        // Set up PIT channel 2 in one-shot mode (mode 0), lobyte/hibyte
        // Command: channel 2 (bits 7-6 = 10), lobyte/hibyte (bits 5-4 = 11),
        //          mode 0 (bits 3-1 = 000), binary (bit 0 = 0) => 0xB0
        Io::Out8(0xB0, PIT_COMMAND);

        // Disable PIT channel 2 gate to prepare
        uint8_t gate = Io::In8(PIT_GATE_PORT);
        gate &= ~0x01;   // Disable gate (bit 0 = 0)
        gate &= ~0x02;   // Disable speaker (bit 1 = 0)
        Io::Out8(gate, PIT_GATE_PORT);

        // Load the count value (low byte first, then high byte)
        Io::Out8((uint8_t)(pitCount & 0xFF), PIT_CHANNEL2_DATA);
        Io::IoPortWait();
        Io::Out8((uint8_t)(pitCount >> 8), PIT_CHANNEL2_DATA);

        // Set APIC timer to max count with divide-by-16, one-shot (masked so no IRQ)
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_DIVIDE, DIVIDE_BY_16);
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_LVT, LVT_MASKED);
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_INITIAL, 0xFFFFFFFF);

        // Enable PIT channel 2 gate to start counting
        gate = Io::In8(PIT_GATE_PORT);
        gate |= 0x01;    // Enable gate (bit 0 = 1)
        Io::Out8(gate, PIT_GATE_PORT);

        // Wait for PIT channel 2 output to go high (bit 5 of port 0x61)
        while (!(Io::In8(PIT_GATE_PORT) & 0x20)) {
            asm volatile("pause");
        }

        // Read how many APIC timer ticks elapsed
        uint32_t currentCount = Hal::LocalApic::ReadRegister(Hal::LocalApic::REG_TIMER_CURRENT);
        uint32_t elapsed = 0xFFFFFFFF - currentCount;

        // Stop the APIC timer
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_INITIAL, 0);

        return elapsed;
    }

    void ApicTimerInitialize() {
        KernelLogStream(INFO, "Timer") << "Calibrating APIC timer using PIT channel 2";

        // Calibrate: measure APIC ticks over ~10ms
        uint32_t ticksIn10ms = CalibratePit();
        g_ticksPerMs = ticksIn10ms / 10;

        if (g_ticksPerMs == 0) {
            KernelLogStream(ERROR, "Timer") << "APIC timer calibration failed (0 ticks/ms)";
            return;
        }

        uint64_t timerFreqHz = (uint64_t)g_ticksPerMs * 1000;

        KernelLogStream(OK, "Timer") << "APIC timer: " << base::dec << (uint64_t)g_ticksPerMs
            << " ticks/ms (" << timerFreqHz << " Hz, divide-by-16)";

        // Register IRQ handler for timer (IRQ 0 = vector 32)
        Hal::RegisterIrqHandler(Hal::IRQ_TIMER, TimerHandler);

        // Configure APIC timer: periodic mode, vector 32
        uint32_t lvt = (Hal::IRQ_VECTOR_BASE + Hal::IRQ_TIMER) | LVT_PERIODIC;
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_DIVIDE, DIVIDE_BY_16);
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_LVT, lvt);

        // Set initial count for 1ms intervals (1000 Hz tick rate)
        uint32_t initialCount = g_ticksPerMs;
        Hal::LocalApic::WriteRegister(Hal::LocalApic::REG_TIMER_INITIAL, initialCount);

        KernelLogStream(OK, "Timer") << "APIC timer started: " << base::dec << (uint64_t)TIMER_HZ
            << " Hz periodic, initial count=" << (uint64_t)initialCount;
    }

    uint64_t GetTicks() {
        return g_tickCount;
    }

    uint64_t GetMilliseconds() {
        return g_tickCount;  // 1 tick = 1 ms at 1000 Hz
    }

    void EnableSchedulerTick() {
        g_schedEnabled = true;
    }

    void Sleep(uint64_t ms) {
        uint64_t target = g_tickCount + ms;
        while (g_tickCount < target) {
            asm volatile("hlt");
        }
    }
};
