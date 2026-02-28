/*
    * Device.hpp
    * SYS_DEVLIST syscall
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Hal/Apic/ApicInit.hpp>
#include <Drivers/PS2/PS2Controller.hpp>
#include <Drivers/USB/Xhci.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Drivers/Graphics/IntelGPU.hpp>
#include <Pci/Pci.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static void dl_strcpy(char* dst, const char* src, int max) {
        int i = 0;
        for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
        dst[i] = '\0';
    }

    static int dl_append(char* dst, int pos, const char* src, int max) {
        for (int i = 0; src[i] && pos < max - 1; i++) dst[pos++] = src[i];
        dst[pos] = '\0';
        return pos;
    }

    static int dl_append_hex(char* dst, int pos, unsigned val, int digits, int max) {
        const char* hex = "0123456789abcdef";
        char tmp[8];
        for (int i = digits - 1; i >= 0; i--) { tmp[i] = hex[val & 0xF]; val >>= 4; }
        for (int i = 0; i < digits && pos < max - 1; i++) dst[pos++] = tmp[i];
        dst[pos] = '\0';
        return pos;
    }

    static int dl_append_dec(char* dst, int pos, int val, int max) {
        if (val == 0) { if (pos < max - 1) dst[pos++] = '0'; dst[pos] = '\0'; return pos; }
        char tmp[12]; int i = 0;
        while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
        while (i > 0 && pos < max - 1) dst[pos++] = tmp[--i];
        dst[pos] = '\0';
        return pos;
    }

    static int Sys_DevList(DevInfo* buf, int maxCount) {
        if (buf == nullptr || maxCount <= 0) return 0;
        int count = 0;

        auto add = [&](uint8_t cat, const char* name, const char* detail) {
            if (count >= maxCount) return;
            buf[count].category = cat;
            buf[count]._pad[0] = 0; buf[count]._pad[1] = 0; buf[count]._pad[2] = 0;
            dl_strcpy(buf[count].name, name, 48);
            dl_strcpy(buf[count].detail, detail, 48);
            count++;
        };

        // CPU cores (category 0)
        int cpuCount = Hal::GetDetectedCpuCount();
        if (cpuCount > 0) {
            char detail[48];
            int p = 0;
            p = dl_append(detail, p, "x86_64, ", 48);
            p = dl_append_dec(detail, p, cpuCount, 48);
            p = dl_append(detail, p, " core(s)", 48);
            add(0, "Processor", detail);
        }

        // Interrupt controllers (category 1)
        add(1, "Local APIC", "");
        add(1, "I/O APIC", "");

        // Timer (category 2)
        add(2, "LAPIC Timer", "Local APIC periodic timer");

        // PS/2 Input (category 3)
        add(3, "PS/2 Keyboard", "IRQ 1");
        if (Drivers::PS2::IsDualChannel()) {
            add(3, "PS/2 Mouse", "IRQ 12");
        }

        // USB devices (category 4)
        if (Drivers::USB::Xhci::IsInitialized()) {
            for (uint8_t slot = 1; slot <= Drivers::USB::Xhci::MAX_SLOTS && count < maxCount; slot++) {
                auto* dev = Drivers::USB::Xhci::GetDevice(slot);
                if (!dev || !dev->Active) continue;
                const char* devName = "USB Device";
                if (dev->InterfaceClass == 3) {
                    if (dev->InterfaceProtocol == 1) devName = "USB HID Keyboard";
                    else if (dev->InterfaceProtocol == 2) devName = "USB HID Mouse";
                    else devName = "USB HID Device";
                } else if (dev->InterfaceClass == 8) {
                    devName = "USB Mass Storage";
                } else if (dev->InterfaceClass == 9) {
                    devName = "USB Hub";
                }
                char detail[48];
                int p = 0;
                p = dl_append(detail, p, "Port ", 48);
                p = dl_append_dec(detail, p, dev->PortId, 48);
                p = dl_append(detail, p, ", VID:", 48);
                p = dl_append_hex(detail, p, dev->VendorId, 4, 48);
                p = dl_append(detail, p, " PID:", 48);
                p = dl_append_hex(detail, p, dev->ProductId, 4, 48);
                add(4, devName, detail);
            }
        }

        // Network (category 5)
        if (Drivers::Net::E1000::IsInitialized()) {
            add(5, "Intel E1000", "Gigabit Ethernet (82540EM)");
        }
        if (Drivers::Net::E1000E::IsInitialized()) {
            add(5, "Intel E1000E", "Gigabit Ethernet (82574L)");
        }

        // Display (category 6)
        if (Drivers::Graphics::IntelGPU::IsInitialized()) {
            auto* gpu = Drivers::Graphics::IntelGPU::GetGpuInfo();
            if (gpu) {
                add(6, gpu->name, "Intel Integrated Graphics");
            }
        }

        // PCI devices (category 7)
        auto& pciDevs = Pci::GetDevices();
        for (int i = 0; i < (int)pciDevs.size() && count < maxCount; i++) {
            auto& d = pciDevs[i];
            const char* className = Pci::GetClassName(d.ClassCode, d.SubClass);
            char detail[48];
            int p = 0;
            p = dl_append_hex(detail, p, d.Bus, 2, 48);
            p = dl_append(detail, p, ":", 48);
            p = dl_append_hex(detail, p, d.Device, 2, 48);
            p = dl_append(detail, p, ".", 48);
            p = dl_append_dec(detail, p, d.Function, 48);
            p = dl_append(detail, p, " ", 48);
            p = dl_append_hex(detail, p, d.VendorId, 4, 48);
            p = dl_append(detail, p, ":", 48);
            p = dl_append_hex(detail, p, d.DeviceId, 4, 48);
            add(7, className, detail);
        }

        return count;
    }
};
