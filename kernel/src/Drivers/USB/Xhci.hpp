/*
    * Xhci.hpp
    * xHCI (USB 3.x) Host Controller driver
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>

namespace Drivers::USB::Xhci {

    // ---------------------------------------------------------------------------
    // Constants
    // ---------------------------------------------------------------------------

    constexpr uint32_t MAX_SLOTS       = 16;
    constexpr uint32_t MAX_PORTS       = 16;
    constexpr uint32_t CMD_RING_SIZE   = 64;
    constexpr uint32_t EVT_RING_SIZE   = 64;
    constexpr uint32_t XFER_RING_SIZE  = 32;

    // MSI configuration (E1000E uses IRQ 24/vector 56, we use 25/57)
    constexpr uint8_t  MSI_IRQ         = 25;
    constexpr uint32_t MSI_VECTOR      = 57;
    constexpr uint32_t MSI_ADDR_BASE   = 0xFEE00000;

    // PCI class/subclass/progif for xHCI
    constexpr uint8_t  PCI_CLASS_SERIAL = 0x0C;
    constexpr uint8_t  PCI_SUBCLASS_USB = 0x03;
    constexpr uint8_t  PCI_PROGIF_XHCI  = 0x30;

    // PCI config space offsets
    constexpr uint8_t  PCI_REG_BAR0     = 0x10;
    constexpr uint8_t  PCI_REG_BAR1     = 0x14;
    constexpr uint8_t  PCI_REG_COMMAND  = 0x04;

    // PCI command register bits
    constexpr uint16_t PCI_CMD_BUS_MASTER   = (1 << 2);
    constexpr uint16_t PCI_CMD_MEM_SPACE    = (1 << 1);
    constexpr uint16_t PCI_CMD_INTX_DISABLE = (1 << 10);

    // ---------------------------------------------------------------------------
    // xHCI Capability Register offsets (from BAR0)
    // ---------------------------------------------------------------------------

    constexpr uint32_t CAP_CAPLENGTH   = 0x00;  // 1 byte
    constexpr uint32_t CAP_HCIVERSION  = 0x02;  // 2 bytes
    constexpr uint32_t CAP_HCSPARAMS1  = 0x04;  // 4 bytes
    constexpr uint32_t CAP_HCSPARAMS2  = 0x08;  // 4 bytes
    constexpr uint32_t CAP_HCSPARAMS3  = 0x0C;  // 4 bytes
    constexpr uint32_t CAP_HCCPARAMS1  = 0x10;  // 4 bytes
    constexpr uint32_t CAP_DBOFF       = 0x14;  // 4 bytes
    constexpr uint32_t CAP_RTSOFF      = 0x18;  // 4 bytes

    // ---------------------------------------------------------------------------
    // xHCI Operational Register offsets (from BAR0 + CAPLENGTH)
    // ---------------------------------------------------------------------------

    constexpr uint32_t OP_USBCMD       = 0x00;
    constexpr uint32_t OP_USBSTS       = 0x04;
    constexpr uint32_t OP_PAGESIZE     = 0x08;
    constexpr uint32_t OP_DNCTRL       = 0x14;
    constexpr uint32_t OP_CRCR         = 0x18;  // 8 bytes
    constexpr uint32_t OP_DCBAAP       = 0x30;  // 8 bytes
    constexpr uint32_t OP_CONFIG       = 0x38;
    constexpr uint32_t OP_PORTSC_BASE  = 0x400;
    constexpr uint32_t OP_PORTSC_STRIDE = 0x10;

    // USBCMD bits
    constexpr uint32_t USBCMD_RS       = (1 << 0);   // Run/Stop
    constexpr uint32_t USBCMD_HCRST    = (1 << 1);   // Host Controller Reset
    constexpr uint32_t USBCMD_INTE     = (1 << 2);   // Interrupter Enable
    constexpr uint32_t USBCMD_HSEE     = (1 << 3);   // Host System Error Enable

    // USBSTS bits
    constexpr uint32_t USBSTS_HCH      = (1 << 0);   // HC Halted
    constexpr uint32_t USBSTS_HSE      = (1 << 2);   // Host System Error
    constexpr uint32_t USBSTS_EINT     = (1 << 3);   // Event Interrupt
    constexpr uint32_t USBSTS_PCD      = (1 << 4);   // Port Change Detect
    constexpr uint32_t USBSTS_CNR      = (1 << 11);  // Controller Not Ready

    // PORTSC bits
    constexpr uint32_t PORTSC_CCS      = (1 << 0);   // Current Connect Status
    constexpr uint32_t PORTSC_PED      = (1 << 1);   // Port Enabled/Disabled
    constexpr uint32_t PORTSC_PR       = (1 << 4);   // Port Reset
    constexpr uint32_t PORTSC_PLS_MASK = (0xF << 5);  // Port Link State
    constexpr uint32_t PORTSC_PP       = (1 << 9);   // Port Power
    constexpr uint32_t PORTSC_SPEED_MASK = (0xF << 10); // Port Speed
    constexpr uint32_t PORTSC_PRC      = (1 << 21);  // Port Reset Change
    constexpr uint32_t PORTSC_CSC      = (1 << 17);  // Connect Status Change
    constexpr uint32_t PORTSC_PEC      = (1 << 18);  // Port Enabled/Disabled Change
    constexpr uint32_t PORTSC_WRC      = (1 << 19);  // Warm Port Reset Change
    constexpr uint32_t PORTSC_OCC      = (1 << 20);  // Over-current Change
    constexpr uint32_t PORTSC_PLC      = (1 << 22);  // Port Link State Change
    constexpr uint32_t PORTSC_CEC      = (1 << 23);  // Port Config Error Change
    // Write-1-to-clear change bits
    constexpr uint32_t PORTSC_CHANGE_BITS = PORTSC_CSC | PORTSC_PEC | PORTSC_WRC
                                          | PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC;
    // Bits that must be preserved when writing PORTSC (RW1S/RW1CS excluded)
    constexpr uint32_t PORTSC_PRESERVE = PORTSC_PP;

    // Port speed values (from PORTSC bits 13:10)
    constexpr uint32_t SPEED_FULL      = 1;
    constexpr uint32_t SPEED_LOW       = 2;
    constexpr uint32_t SPEED_HIGH      = 3;
    constexpr uint32_t SPEED_SUPER     = 4;

    // ---------------------------------------------------------------------------
    // Runtime Register offsets (from BAR0 + RTSOFF)
    // ---------------------------------------------------------------------------

    // Interrupter 0 registers
    constexpr uint32_t IR0_IMAN        = 0x20;   // Interrupter Management
    constexpr uint32_t IR0_IMOD        = 0x24;   // Interrupter Moderation
    constexpr uint32_t IR0_ERSTSZ      = 0x28;   // Event Ring Segment Table Size
    constexpr uint32_t IR0_ERSTBA      = 0x30;   // Event Ring Segment Table Base Address (8 bytes)
    constexpr uint32_t IR0_ERDP        = 0x38;   // Event Ring Dequeue Pointer (8 bytes)

    // IMAN bits
    constexpr uint32_t IMAN_IP         = (1 << 0);   // Interrupt Pending
    constexpr uint32_t IMAN_IE         = (1 << 1);   // Interrupt Enable

    // ---------------------------------------------------------------------------
    // TRB (Transfer Request Block) - 16 bytes
    // ---------------------------------------------------------------------------

    struct TRB {
        uint32_t Parameter0;
        uint32_t Parameter1;
        uint32_t Status;
        uint32_t Control;
    } __attribute__((packed));

    // TRB type field (bits 15:10 of Control)
    constexpr uint32_t TRB_TYPE_SHIFT  = 10;
    constexpr uint32_t TRB_TYPE_MASK   = (0x3F << TRB_TYPE_SHIFT);

    // TRB types
    constexpr uint32_t TRB_NORMAL              = 1;
    constexpr uint32_t TRB_SETUP_STAGE         = 2;
    constexpr uint32_t TRB_DATA_STAGE          = 3;
    constexpr uint32_t TRB_STATUS_STAGE        = 4;
    constexpr uint32_t TRB_LINK                = 6;
    constexpr uint32_t TRB_ENABLE_SLOT         = 9;
    constexpr uint32_t TRB_DISABLE_SLOT        = 10;
    constexpr uint32_t TRB_ADDRESS_DEVICE      = 11;
    constexpr uint32_t TRB_CONFIGURE_ENDPOINT  = 12;
    constexpr uint32_t TRB_EVALUATE_CONTEXT    = 13;
    constexpr uint32_t TRB_RESET_ENDPOINT      = 14;
    constexpr uint32_t TRB_NOOP_CMD            = 23;
    constexpr uint32_t TRB_TRANSFER_EVENT      = 32;
    constexpr uint32_t TRB_COMMAND_COMPLETION  = 33;
    constexpr uint32_t TRB_PORT_STATUS_CHANGE  = 34;

    // TRB control field bits
    constexpr uint32_t TRB_CYCLE_BIT   = (1 << 0);
    constexpr uint32_t TRB_IOC         = (1 << 5);   // Interrupt On Completion
    constexpr uint32_t TRB_IDT         = (1 << 6);   // Immediate Data
    constexpr uint32_t TRB_BSR         = (1 << 9);   // Block Set Address Request
    constexpr uint32_t TRB_DIR_IN      = (1 << 16);  // Direction: 1=IN (device to host)
    constexpr uint32_t TRB_TRT_IN      = (3 << 16);  // Transfer Type: IN
    constexpr uint32_t TRB_TRT_OUT     = (2 << 16);  // Transfer Type: OUT
    constexpr uint32_t TRB_TRT_NODATA  = (0 << 16);  // Transfer Type: No Data Stage
    constexpr uint32_t TRB_ISP         = (1 << 2);   // Interrupt on Short Packet
    constexpr uint32_t TRB_CHAIN       = (1 << 4);   // Chain bit
    constexpr uint32_t TRB_ENT         = (1 << 1);   // Evaluate Next TRB

    // Completion codes (from Status field bits 31:24)
    constexpr uint32_t CC_SUCCESS             = 1;
    constexpr uint32_t CC_SHORT_PACKET        = 13;

    // ---------------------------------------------------------------------------
    // Event Ring Segment Table Entry
    // ---------------------------------------------------------------------------

    struct ERSTEntry {
        uint64_t RingSegmentBase;
        uint32_t RingSegmentSize;
        uint32_t Reserved;
    } __attribute__((packed));

    // ---------------------------------------------------------------------------
    // Device Context structures (xHCI spec section 6.2)
    // ---------------------------------------------------------------------------

    struct SlotContext {
        uint32_t Field0;    // Route String, Speed, MTT, Hub, Context Entries
        uint32_t Field1;    // Max Exit Latency, Root Hub Port Number, Num Ports
        uint32_t Field2;    // TT Hub Slot ID, TT Port Number, Interrupter Target
        uint32_t Field3;    // Device Address, Slot State
        uint32_t Reserved[4];
    } __attribute__((packed));

    struct EndpointContext {
        uint32_t Field0;    // EP State, Mult, MaxPStreams, Interval, LSA
        uint32_t Field1;    // CErr, EP Type, HID, Max Burst Size, Max Packet Size
        uint64_t TRDequeuePtr; // TR Dequeue Pointer (with DCS at bit 0)
        uint32_t Field2;    // Average TRB Length, Max ESIT Payload Lo
        uint32_t Reserved[3];
    } __attribute__((packed));

    // Endpoint types (bits 5:3 of EP Field1)
    constexpr uint32_t EP_TYPE_ISOCH_OUT   = 1;
    constexpr uint32_t EP_TYPE_BULK_OUT    = 2;
    constexpr uint32_t EP_TYPE_INTERRUPT_OUT = 3;
    constexpr uint32_t EP_TYPE_CONTROL     = 4;
    constexpr uint32_t EP_TYPE_ISOCH_IN    = 5;
    constexpr uint32_t EP_TYPE_BULK_IN     = 6;
    constexpr uint32_t EP_TYPE_INTERRUPT_IN = 7;

    struct InputControlContext {
        uint32_t DropFlags;
        uint32_t AddFlags;
        uint32_t Reserved[5];
        uint8_t  ConfigValue;
        uint8_t  InterfaceNumber;
        uint8_t  AlternateSetting;
        uint8_t  Reserved2;
    } __attribute__((packed));

    // Full InputContext: InputControlContext + SlotContext + 31 EndpointContexts
    // (but we only use EP0 + a few endpoints)
    struct InputContext {
        InputControlContext ICC;
        SlotContext         Slot;
        EndpointContext     EP[31];
    } __attribute__((packed));

    struct DeviceContext {
        SlotContext     Slot;
        EndpointContext EP[31];
    } __attribute__((packed));

    // ---------------------------------------------------------------------------
    // Per-device tracking
    // ---------------------------------------------------------------------------

    struct UsbDeviceInfo {
        bool     Active;
        uint8_t  PortId;
        uint32_t Speed;
        uint16_t VendorId;
        uint16_t ProductId;
        uint8_t  InterfaceClass;
        uint8_t  InterfaceSubClass;
        uint8_t  InterfaceProtocol;

        // Interrupt IN endpoint
        uint8_t  InterruptEpNum;      // Endpoint number (1-15)
        uint16_t InterruptMaxPacket;
        uint8_t  InterruptInterval;

        // Transfer ring for Interrupt IN endpoint
        TRB*     InterruptRing;
        uint64_t InterruptRingPhys;
        uint32_t InterruptRingEnqueue;
        bool     InterruptRingCCS;    // Current Cycle State

        // EP0 transfer ring
        TRB*     EP0Ring;
        uint64_t EP0RingPhys;
        uint32_t EP0RingEnqueue;
        bool     EP0RingCCS;

        // Device context (output)
        DeviceContext* OutputContext;
        uint64_t       OutputContextPhys;
    };

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------

    void Initialize();
    bool IsInitialized();

    // Deferred hot-plug processing (call from timer tick, not interrupt context)
    void ProcessDeferredWork();

    // Send a command on the command ring, wait for completion.
    // Returns completion code.
    uint32_t SendCommand(const TRB& trb);

    // Perform a control transfer on slot's EP0.
    // setup: 8 bytes of USB setup packet (packed into TRB params)
    // data: optional data buffer (virtual address), dataLen: length
    // dirIn: true = device-to-host
    // Returns completion code.
    uint32_t ControlTransfer(uint8_t slotId, uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                             void* data, bool dirIn);

    // Queue an interrupt IN transfer on a device's interrupt endpoint
    void QueueInterruptTransfer(uint8_t slotId);

    // Ring a doorbell
    void RingDoorbell(uint8_t slotId, uint8_t target);

    // Access device info
    UsbDeviceInfo* GetDevice(uint8_t slotId);

    // Poll event ring (called from interrupt handler or during init)
    void PollEvents();

};
