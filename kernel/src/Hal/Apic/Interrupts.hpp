/*
    * Interrupts.hpp
    * Hardware interrupt registration and dispatch
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Hal {
    // IRQ handler function type. The parameter is the IRQ number (0-47).
    using IrqHandler = void(*)(uint8_t irq);

    // Number of IRQ slots supported (0-23: legacy ISA via IOAPIC, 24-47: MSI)
    constexpr int IRQ_COUNT = 48;

    // IRQ vector base: hardware IRQs start at IDT vector 32
    constexpr uint8_t IRQ_VECTOR_BASE = 32;

    // Well-known ISA IRQ assignments
    constexpr uint8_t IRQ_TIMER    = 0;
    constexpr uint8_t IRQ_KEYBOARD = 1;
    constexpr uint8_t IRQ_CASCADE  = 2;
    constexpr uint8_t IRQ_COM2     = 3;
    constexpr uint8_t IRQ_COM1     = 4;
    constexpr uint8_t IRQ_FLOPPY   = 6;
    constexpr uint8_t IRQ_RTC      = 8;
    constexpr uint8_t IRQ_MOUSE    = 12;
    constexpr uint8_t IRQ_ATA1     = 14;
    constexpr uint8_t IRQ_ATA2     = 15;

    // Register a handler for the given IRQ number (0-47)
    void RegisterIrqHandler(uint8_t irq, IrqHandler handler);

    // Install IRQ stubs into the IDT and set up the dispatch table
    void InitializeIrqHandlers();
};
