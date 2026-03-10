/*
    * Init.hpp
    * Driver initialization orchestration
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once

namespace Drivers {

    // Probe PCI devices for Early-phase drivers (GPU).
    void ProbeEarly();

    // Post-probe: wire up GPU framebuffer to cursor subsystem.
    void InitializeGraphics();

    // Probe PCI devices for Normal-phase drivers (xHCI, E1000, E1000E, AHCI, NVMe, IntelHDA).
    // USB class drivers (Bluetooth, etc.) initialize automatically during xHCI enumeration.
    void ProbeNormal();

    // Post-probe: initialize network stack.
    void InitializeNetwork();

    // Post-probe: register SATA drives with VFS.
    void InitializeStorage();

    // Post-probe: initialize audio subsystem.
    void InitializeAudio();

}
