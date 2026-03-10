/*
    * BluetoothSyscall.hpp
    * SYS_BTSCAN, SYS_BTCONNECT, SYS_BTDISCONNECT, SYS_BTLIST, SYS_BTINFO
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Drivers/USB/Bluetooth/Bluetooth.hpp>
#include <Drivers/USB/Bluetooth/Hci.hpp>
#include <Libraries/Memory.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static int64_t Sys_BtScan(BtScanResult* buf, int maxCount, uint32_t timeoutMs) {
        if (!buf || maxCount <= 0) return -1;
        if (!Drivers::USB::Bluetooth::IsInitialized()) return -1;

        // Use a stack buffer for kernel-side results (max 16)
        Drivers::USB::Bluetooth::Hci::InquiryDevice tmpBuf[16];
        int kMax = maxCount;
        if (kMax > 16) kMax = 16;

        int found = Drivers::USB::Bluetooth::Scan(tmpBuf, kMax, timeoutMs);
        if (found < 0) return found;

        // Convert kernel InquiryDevice to userspace BtScanResult
        for (int i = 0; i < found; i++) {
            memcpy(buf[i].bdAddr, tmpBuf[i].BdAddr, 6);
            buf[i]._pad[0] = 0;
            buf[i]._pad[1] = 0;
            buf[i].classOfDevice = tmpBuf[i].ClassOfDevice;
            buf[i].rssi = tmpBuf[i].Rssi;
            buf[i]._pad2[0] = 0;
            buf[i]._pad2[1] = 0;
            buf[i]._pad2[2] = 0;
            memcpy(buf[i].name, tmpBuf[i].Name, 64);
        }

        return (int64_t)found;
    }

    static int64_t Sys_BtConnect(const uint8_t* bdAddr) {
        if (!bdAddr) return -1;
        if (!Drivers::USB::Bluetooth::IsInitialized()) return -1;

        return (int64_t)Drivers::USB::Bluetooth::Connect(bdAddr);
    }

    static int64_t Sys_BtDisconnect(const uint8_t* bdAddr) {
        if (!bdAddr) return -1;
        if (!Drivers::USB::Bluetooth::IsInitialized()) return -1;

        return (int64_t)Drivers::USB::Bluetooth::Disconnect(bdAddr);
    }

    static int64_t Sys_BtList(BtDevInfo* buf, int maxCount) {
        if (!buf || maxCount <= 0) return 0;
        if (!Drivers::USB::Bluetooth::IsInitialized()) return 0;

        Drivers::USB::Bluetooth::Hci::ConnectionInfo tmpBuf[4];
        int kMax = maxCount;
        if (kMax > 4) kMax = 4;

        int count = Drivers::USB::Bluetooth::ListConnected(tmpBuf, kMax);

        for (int i = 0; i < count; i++) {
            memcpy(buf[i].bdAddr, tmpBuf[i].BdAddr, 6);
            buf[i].connected = tmpBuf[i].Active ? 1 : 0;
            buf[i].encrypted = tmpBuf[i].Encrypted ? 1 : 0;
            buf[i].handle = tmpBuf[i].Handle;
            buf[i].linkType = tmpBuf[i].LinkType;
            buf[i]._pad = 0;
        }

        return (int64_t)count;
    }

    static int64_t Sys_BtInfo(BtAdapterInfo* buf) {
        if (!buf) return -1;

        memset(buf, 0, sizeof(BtAdapterInfo));

        if (!Drivers::USB::Bluetooth::IsInitialized()) {
            buf->initialized = 0;
            return -1;
        }

        buf->initialized = 1;
        memcpy(buf->bdAddr, Drivers::USB::Bluetooth::GetBdAddr(), 6);
        buf->scanning = Drivers::USB::Bluetooth::Hci::IsInquiryActive() ? 1 : 0;

        // Copy adapter name
        const char* name = "MontaukOS";
        int i = 0;
        for (; i < 63 && name[i]; i++) buf->name[i] = name[i];
        buf->name[i] = '\0';

        return 0;
    }

}
