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
#include <Drivers/USB/UsbDevice.hpp>
#include <Drivers/USB/Bluetooth/Bluetooth.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Drivers/Graphics/IntelGPU.hpp>
#include <Drivers/Storage/Ahci.hpp>
#include <Drivers/Audio/IntelHda.hpp>
#include <Pci/Pci.hpp>

#include "Syscall.hpp"
#include <Drivers/Storage/BlockDevice.hpp>
#include <Drivers/Storage/Nvme.hpp>

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
                if (dev->InterfaceClass == Drivers::USB::UsbDevice::CLASS_HID) {
                    if (dev->InterfaceProtocol == 1) devName = "USB HID Keyboard";
                    else if (dev->InterfaceProtocol == 2) devName = "USB HID Mouse";
                    else devName = "USB HID Device";
                } else if (dev->InterfaceClass == Drivers::USB::UsbDevice::CLASS_WIRELESS) {
                    devName = "Bluetooth Adapter";
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

        // Audio (category 9)
        if (Drivers::Audio::IntelHda::IsInitialized()) {
            uint32_t codecId = Drivers::Audio::IntelHda::GetCodecVendorId();
            char detail[48];
            int p = 0;
            p = dl_append(detail, p, "HDA Codec ", 48);
            p = dl_append_hex(detail, p, codecId >> 16, 4, 48);
            p = dl_append(detail, p, ":", 48);
            p = dl_append_hex(detail, p, codecId & 0xFFFF, 4, 48);
            add(9, "Intel HDA", detail);
        }

        if (Drivers::USB::Bluetooth::IsInitialized()) {
            add(9, "Bluetooth Audio", "A2DP (SBC)");
        }

        // Storage (category 7)
        if (Drivers::Storage::Ahci::IsInitialized()) {
            for (int port = 0; port < 32 && count < maxCount; port++) {
                auto* info = Drivers::Storage::Ahci::GetPortInfo(port);
                if (!info) continue;
                uint64_t sectors = info->SectorCount;
                uint64_t sizeMB = (sectors * 512) / (1024 * 1024);
                uint64_t sizeGB = sizeMB / 1024;
                char detail[48];
                int p = 0;
                if (sizeGB > 0) {
                    p = dl_append_dec(detail, p, (int)sizeGB, 48);
                    p = dl_append(detail, p, " GiB, SATA port ", 48);
                } else {
                    p = dl_append_dec(detail, p, (int)sizeMB, 48);
                    p = dl_append(detail, p, " MiB, SATA port ", 48);
                }
                p = dl_append_dec(detail, p, port, 48);
                add(7, info->Model, detail);
                buf[count - 1]._pad[0] = (uint8_t)port; // stash port index
            }
        }

        if (Drivers::Storage::Nvme::IsInitialized()) {
            for (int ns = 0; ns < Drivers::Storage::Nvme::GetNamespaceCount() && count < maxCount; ns++) {
                auto* nsInfo = Drivers::Storage::Nvme::GetNamespaceInfo(ns);
                if (!nsInfo || !nsInfo->Active) continue;
                uint64_t sectors = nsInfo->SectorCount;
                uint64_t sizeMB = (sectors * nsInfo->SectorSize) / (1024 * 1024);
                uint64_t sizeGB = sizeMB / 1024;
                char detail[48];
                int p = 0;
                if (sizeGB > 0) {
                    p = dl_append_dec(detail, p, (int)sizeGB, 48);
                    p = dl_append(detail, p, " GiB, NVMe ns ", 48);
                } else {
                    p = dl_append_dec(detail, p, (int)sizeMB, 48);
                    p = dl_append(detail, p, " MiB, NVMe ns ", 48);
                }
                p = dl_append_dec(detail, p, ns, 48);
                add(7, nsInfo->Model, detail);
            }
        }

        // PCI devices (category 8)
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
            add(8, className, detail);
        }

        return count;
    }

    static int Sys_DiskInfo(DiskInfo* buf, int blockDevIndex) {
        if (buf == nullptr) return -1;

        auto* bdev = Drivers::Storage::GetBlockDevice(blockDevIndex);
        if (!bdev) return -1;

        // Zero the struct first so SATA-specific fields default to 0
        for (unsigned i = 0; i < sizeof(DiskInfo); i++)
            ((uint8_t*)buf)[i] = 0;

        buf->port = (uint8_t)blockDevIndex;
        buf->sectorCount = bdev->SectorCount;
        buf->sectorSizeLog = bdev->SectorSize;
        buf->sectorSizePhys = bdev->SectorSize;
        dl_strcpy(buf->model, bdev->Model, 41);

        // Try to fill AHCI-specific fields if this is an AHCI device
        if (Drivers::Storage::Ahci::IsInitialized()) {
            for (int p = 0; p < 32; p++) {
                auto* info = Drivers::Storage::Ahci::GetPortInfo(p);
                if (!info) continue;
                // Match by sector count and model name
                if (info->SectorCount == bdev->SectorCount &&
                    info->Model[0] == bdev->Model[0]) {
                    buf->type = (uint8_t)info->Type;
                    buf->sataGen = (uint8_t)info->SataGen;
                    buf->sectorSizeLog = info->SectorSizeLog;
                    buf->sectorSizePhys = info->SectorSizePhys;
                    buf->rpm = info->Rpm;
                    buf->ncqDepth = info->NcqDepth;
                    buf->supportsLba48 = info->SupportsLba48 ? 1 : 0;
                    buf->supportsNcq = info->SupportsNcq ? 1 : 0;
                    buf->supportsTrim = info->SupportsTrim ? 1 : 0;
                    buf->supportsSmart = info->SupportsSmart ? 1 : 0;
                    buf->supportsWriteCache = info->SupportsWriteCache ? 1 : 0;
                    buf->supportsReadAhead = info->SupportsReadAhead ? 1 : 0;
                    dl_strcpy(buf->serial, info->Serial, 21);
                    dl_strcpy(buf->firmware, info->Firmware, 9);
                    break;
                }
            }
        }

        // For NVMe devices: set type=3 (NVMe), rpm=1 (SSD)
        if (buf->type == 0 && Drivers::Storage::Nvme::IsInitialized()) {
            for (int ns = 0; ns < Drivers::Storage::Nvme::GetNamespaceCount(); ns++) {
                auto* nsInfo = Drivers::Storage::Nvme::GetNamespaceInfo(ns);
                if (!nsInfo) continue;
                if (nsInfo->SectorCount == bdev->SectorCount) {
                    buf->type = 3;   // NVMe
                    buf->rpm = 1;    // SSD / non-rotating
                    break;
                }
            }
        }

        // If type is still 0, this block device is not recognized
        if (buf->type == 0) return -1;

        return 0;
    }
};
