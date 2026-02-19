/*
    * UsbDevice.hpp
    * USB device enumeration and standard descriptors
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::UsbDevice {

    // ---------------------------------------------------------------------------
    // USB Standard Descriptors
    // ---------------------------------------------------------------------------

    struct DeviceDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint16_t bcdUSB;
        uint8_t  bDeviceClass;
        uint8_t  bDeviceSubClass;
        uint8_t  bDeviceProtocol;
        uint8_t  bMaxPacketSize0;
        uint16_t idVendor;
        uint16_t idProduct;
        uint16_t bcdDevice;
        uint8_t  iManufacturer;
        uint8_t  iProduct;
        uint8_t  iSerialNumber;
        uint8_t  bNumConfigurations;
    } __attribute__((packed));

    struct ConfigDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint16_t wTotalLength;
        uint8_t  bNumInterfaces;
        uint8_t  bConfigurationValue;
        uint8_t  iConfiguration;
        uint8_t  bmAttributes;
        uint8_t  bMaxPower;
    } __attribute__((packed));

    struct InterfaceDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bInterfaceNumber;
        uint8_t bAlternateSetting;
        uint8_t bNumEndpoints;
        uint8_t bInterfaceClass;
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t iInterface;
    } __attribute__((packed));

    struct EndpointDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bEndpointAddress;
        uint8_t  bmAttributes;
        uint16_t wMaxPacketSize;
        uint8_t  bInterval;
    } __attribute__((packed));

    // ---------------------------------------------------------------------------
    // USB Constants
    // ---------------------------------------------------------------------------

    // Descriptor types
    constexpr uint8_t DESC_DEVICE        = 1;
    constexpr uint8_t DESC_CONFIGURATION = 2;
    constexpr uint8_t DESC_INTERFACE     = 4;
    constexpr uint8_t DESC_ENDPOINT      = 5;

    // USB class codes
    constexpr uint8_t CLASS_HID          = 0x03;
    constexpr uint8_t SUBCLASS_BOOT      = 0x01;
    constexpr uint8_t PROTOCOL_KEYBOARD  = 0x01;
    constexpr uint8_t PROTOCOL_MOUSE     = 0x02;

    // USB standard requests (bRequest)
    constexpr uint8_t REQ_GET_DESCRIPTOR    = 0x06;
    constexpr uint8_t REQ_SET_CONFIGURATION = 0x09;
    constexpr uint8_t REQ_SET_INTERFACE     = 0x0B;

    // HID class requests
    constexpr uint8_t REQ_SET_PROTOCOL   = 0x0B;
    constexpr uint8_t REQ_SET_IDLE       = 0x0A;

    // Request type (bmRequestType)
    constexpr uint8_t REQTYPE_DEV_TO_HOST    = 0x80;
    constexpr uint8_t REQTYPE_HOST_TO_DEV    = 0x00;
    constexpr uint8_t REQTYPE_CLASS_IFACE    = 0x21;  // Host-to-device, class, interface

    // Endpoint direction mask
    constexpr uint8_t EP_DIR_IN = 0x80;

    // Endpoint transfer type mask
    constexpr uint8_t EP_XFER_TYPE_MASK  = 0x03;
    constexpr uint8_t EP_XFER_INTERRUPT  = 0x03;

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------

    // Enumerate a newly connected device on the given port with the given speed.
    // Returns the assigned slot ID, or 0 on failure.
    uint8_t EnumerateDevice(uint8_t portId, uint32_t speed);

};
