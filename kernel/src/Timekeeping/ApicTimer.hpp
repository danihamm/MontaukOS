/*
    * ApicTimer.hpp
    * Local APIC timer for periodic tick interrupts and timekeeping
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Timekeeping {
    // Initialize the APIC timer: calibrate against PIT, start periodic interrupts
    void ApicTimerInitialize();

    // Get the monotonic tick count (increments on each timer interrupt)
    uint64_t GetTicks();

    // Get elapsed milliseconds since timer initialization
    uint64_t GetMilliseconds();

    // Busy-wait sleep for the given number of milliseconds
    void Sleep(uint64_t ms);
};