/*
    * UsbDevice.cpp
    * USB device enumeration and configuration
    * Copyright (c) 2025 Daniel Hammer
*/

#include "UsbDevice.hpp"
#include "Xhci.hpp"
#include "HidKeyboard.hpp"
#include "HidMouse.hpp"
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Libraries/Memory.hpp>

using namespace Kt;

// Access xHCI internal state needed during enumeration
namespace Drivers::USB::Xhci {
    extern volatile uint32_t g_cmdCompletionSlotId;
    extern uint64_t* g_dcbaa;
}

namespace Drivers::USB::UsbDevice {

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    static uint16_t MaxPacketSizeForSpeed(uint32_t speed) {
        switch (speed) {
            case Xhci::SPEED_LOW:   return 8;
            case Xhci::SPEED_FULL:  return 8;
            case Xhci::SPEED_HIGH:  return 64;
            case Xhci::SPEED_SUPER: return 512;
            default:                return 64;
        }
    }

    // Map xHCI port speed to the slot context speed field value.
    // Per the xHCI spec the slot context speed field uses the same encoding
    // as PORTSC (1=Full, 2=Low, 3=High, 4=Super).
    static uint32_t SpeedToSlotContextValue(uint32_t speed) {
        return speed;  // Same encoding
    }

    // Convert USB endpoint bInterval to xHCI Endpoint Context Interval value.
    // HS/SS: bInterval is already in 2^(n-1) * 125µs encoding — use directly.
    // FS/LS: bInterval is in milliseconds (frames) — convert via fls(bInterval * 8).
    static uint32_t ConvertInterval(uint32_t speed, uint8_t bInterval) {
        if (bInterval == 0) return 0;

        if (speed == Xhci::SPEED_HIGH || speed == Xhci::SPEED_SUPER) {
            return bInterval;
        }

        // FS/LS: bInterval ms → microframes, then find highest set bit position
        uint32_t microframes = (uint32_t)bInterval * 8;
        uint32_t interval = 0;
        while (microframes > 0) {
            interval++;
            microframes >>= 1;
        }
        if (interval > 15) interval = 15;
        return interval;
    }

    // Convert xHCI port speed to a human-readable string
    static const char* SpeedToString(uint32_t speed) {
        switch (speed) {
            case Xhci::SPEED_LOW:   return "Low";
            case Xhci::SPEED_FULL:  return "Full";
            case Xhci::SPEED_HIGH:  return "High";
            case Xhci::SPEED_SUPER: return "Super";
            default:                return "Unknown";
        }
    }

    // ---------------------------------------------------------------------------
    // EnumerateDevice
    // ---------------------------------------------------------------------------

    uint8_t EnumerateDevice(uint8_t portId, uint32_t speed) {
        KernelLogStream(INFO, "USB") << "Enumerating device on port "
            << (uint64_t)portId << " speed=" << SpeedToString(speed);

        // -----------------------------------------------------------------
        // Step 1: Enable Slot
        // -----------------------------------------------------------------
        Xhci::TRB enableSlotTrb = {};
        enableSlotTrb.Control = (Xhci::TRB_ENABLE_SLOT << Xhci::TRB_TYPE_SHIFT);

        uint32_t cc = Xhci::SendCommand(enableSlotTrb);
        if (cc != Xhci::CC_SUCCESS) {
            KernelLogStream(ERROR, "USB") << "Enable Slot failed, cc=" << (uint64_t)cc;
            return 0;
        }

        uint8_t slotId = (uint8_t)Xhci::g_cmdCompletionSlotId;
        if (slotId == 0 || slotId > Xhci::MAX_SLOTS) {
            KernelLogStream(ERROR, "USB") << "Invalid slot ID: " << (uint64_t)slotId;
            return 0;
        }

        KernelLogStream(INFO, "USB") << "Slot " << (uint64_t)slotId << " enabled";

        // -----------------------------------------------------------------
        // Step 2: Allocate device output context and set DCBAA entry
        // -----------------------------------------------------------------
        auto* dev = Xhci::GetDevice(slotId);
        dev->Active = true;
        dev->PortId = portId;
        dev->Speed  = speed;

        // Allocate a zeroed page for the output DeviceContext
        auto* outputCtx = (Xhci::DeviceContext*)Memory::g_pfa->AllocateZeroed();
        dev->OutputContext     = outputCtx;
        dev->OutputContextPhys = Memory::SubHHDM(outputCtx);

        // Point DCBAA[slotId] to the output context physical address
        Xhci::g_dcbaa[slotId] = dev->OutputContextPhys;

        // -----------------------------------------------------------------
        // Step 3: Build Input Context for Address Device command
        // -----------------------------------------------------------------
        auto* inputCtx = (Xhci::InputContext*)Memory::g_pfa->AllocateZeroed();

        // Input Control Context: add Slot Context (bit 0) and EP0 (bit 1)
        inputCtx->ICC.AddFlags = 0x3;

        // Slot Context
        uint32_t speedVal    = SpeedToSlotContextValue(speed);
        uint32_t ctxEntries  = 1;  // Context Entries = 1 (Slot + EP0 only)
        inputCtx->Slot.Field0 = (ctxEntries << 27) | (speedVal << 20);
        inputCtx->Slot.Field1 = ((uint32_t)portId << 16);  // Root Hub Port Number

        // EP0 Context
        // Allocate EP0 transfer ring
        auto* ep0Ring = (Xhci::TRB*)Memory::g_pfa->AllocateZeroed();
        dev->EP0Ring        = ep0Ring;
        dev->EP0RingPhys    = Memory::SubHHDM(ep0Ring);
        dev->EP0RingEnqueue = 0;
        dev->EP0RingCCS     = true;

        // Set up Link TRB at last position to wrap back to start
        // (bit 1 = Toggle Cycle on Link TRBs)
        Xhci::TRB& ep0Link = ep0Ring[Xhci::XFER_RING_SIZE - 1];
        ep0Link.Parameter0 = (uint32_t)(dev->EP0RingPhys & 0xFFFFFFFF);
        ep0Link.Parameter1 = (uint32_t)(dev->EP0RingPhys >> 32);
        ep0Link.Status = 0;
        ep0Link.Control = (Xhci::TRB_LINK << Xhci::TRB_TYPE_SHIFT) | Xhci::TRB_ENT;

        uint16_t maxPacket = MaxPacketSizeForSpeed(speed);

        // Field1: CErr=3 (bits 2:1), EP Type=Control=4 (bits 5:3), Max Packet Size (bits 31:16)
        inputCtx->EP[0].Field1 = (3 << 1)
                                | (Xhci::EP_TYPE_CONTROL << 3)
                                | ((uint32_t)maxPacket << 16);
        // TR Dequeue Pointer with DCS=1
        inputCtx->EP[0].TRDequeuePtr = dev->EP0RingPhys | 1;
        // Average TRB Length = 8
        inputCtx->EP[0].Field2 = 8;

        // -----------------------------------------------------------------
        // Step 4: Address Device command
        // -----------------------------------------------------------------
        Xhci::TRB addrTrb = {};
        uint64_t inputCtxPhys = Memory::SubHHDM(inputCtx);
        addrTrb.Parameter0 = (uint32_t)(inputCtxPhys & 0xFFFFFFFF);
        addrTrb.Parameter1 = (uint32_t)(inputCtxPhys >> 32);
        addrTrb.Control    = (Xhci::TRB_ADDRESS_DEVICE << Xhci::TRB_TYPE_SHIFT)
                           | ((uint32_t)slotId << 24);

        cc = Xhci::SendCommand(addrTrb);
        if (cc != Xhci::CC_SUCCESS) {
            KernelLogStream(ERROR, "USB") << "Address Device failed, slot="
                << (uint64_t)slotId << " cc=" << (uint64_t)cc;
            dev->Active = false;
            return 0;
        }

        KernelLogStream(INFO, "USB") << "Slot " << (uint64_t)slotId << " addressed";

        // -----------------------------------------------------------------
        // Step 5: GET_DESCRIPTOR (Device)
        // -----------------------------------------------------------------
        DeviceDescriptor devDesc = {};
        cc = Xhci::ControlTransfer(slotId, REQTYPE_DEV_TO_HOST, REQ_GET_DESCRIPTOR,
                                   (DESC_DEVICE << 8), 0, sizeof(DeviceDescriptor),
                                   &devDesc, true);
        if (cc != Xhci::CC_SUCCESS && cc != Xhci::CC_SHORT_PACKET) {
            KernelLogStream(ERROR, "USB") << "GET_DESCRIPTOR(Device) failed, cc=" << (uint64_t)cc;
            dev->Active = false;
            return 0;
        }

        dev->VendorId  = devDesc.idVendor;
        dev->ProductId = devDesc.idProduct;

        KernelLogStream(INFO, "USB") << "Slot " << (uint64_t)slotId
            << ": VID:PID = " << base::hex << (uint64_t)devDesc.idVendor
            << ":" << (uint64_t)devDesc.idProduct << base::dec;

        // -----------------------------------------------------------------
        // Step 6: GET_DESCRIPTOR (Configuration) -- header first, then full
        // -----------------------------------------------------------------
        ConfigDescriptor cfgHdr = {};
        cc = Xhci::ControlTransfer(slotId, REQTYPE_DEV_TO_HOST, REQ_GET_DESCRIPTOR,
                                   (DESC_CONFIGURATION << 8), 0, sizeof(ConfigDescriptor),
                                   &cfgHdr, true);
        if (cc != Xhci::CC_SUCCESS && cc != Xhci::CC_SHORT_PACKET) {
            KernelLogStream(ERROR, "USB") << "GET_DESCRIPTOR(Config header) failed, cc=" << (uint64_t)cc;
            dev->Active = false;
            return 0;
        }

        uint8_t cfgBuf[256] = {};
        uint16_t totalLen = cfgHdr.wTotalLength;
        if (totalLen > 256) totalLen = 256;

        cc = Xhci::ControlTransfer(slotId, REQTYPE_DEV_TO_HOST, REQ_GET_DESCRIPTOR,
                                   (DESC_CONFIGURATION << 8), 0, totalLen,
                                   cfgBuf, true);
        if (cc != Xhci::CC_SUCCESS && cc != Xhci::CC_SHORT_PACKET) {
            KernelLogStream(ERROR, "USB") << "GET_DESCRIPTOR(Config full) failed, cc=" << (uint64_t)cc;
            dev->Active = false;
            return 0;
        }

        // -----------------------------------------------------------------
        // Step 7: Parse configuration descriptor blob
        // -----------------------------------------------------------------
        uint16_t offset = 0;
        bool foundHid = false;
        bool foundEp = false;

        while (offset + 2 <= totalLen) {
            uint8_t len  = cfgBuf[offset];
            uint8_t type = cfgBuf[offset + 1];
            if (len == 0) break;

            if (type == DESC_INTERFACE && offset + sizeof(InterfaceDescriptor) <= totalLen) {
                auto* iface = (InterfaceDescriptor*)&cfgBuf[offset];
                // Reset foundHid at each new interface boundary so we don't
                // accidentally pick up endpoints from a different interface.
                foundHid = false;
                if (!foundEp &&
                    iface->bInterfaceClass == CLASS_HID &&
                    iface->bInterfaceSubClass == SUBCLASS_BOOT) {
                    dev->InterfaceClass    = iface->bInterfaceClass;
                    dev->InterfaceSubClass = iface->bInterfaceSubClass;
                    dev->InterfaceProtocol = iface->bInterfaceProtocol;
                    foundHid = true;
                }
            }

            if (type == DESC_ENDPOINT && foundHid && !foundEp &&
                offset + sizeof(EndpointDescriptor) <= totalLen) {
                auto* ep = (EndpointDescriptor*)&cfgBuf[offset];
                if ((ep->bEndpointAddress & EP_DIR_IN) &&
                    (ep->bmAttributes & EP_XFER_TYPE_MASK) == EP_XFER_INTERRUPT) {
                    dev->InterruptEpNum     = ep->bEndpointAddress & 0x0F;
                    dev->InterruptMaxPacket = ep->wMaxPacketSize & 0x7FF;
                    dev->InterruptInterval  = ep->bInterval;
                    foundEp = true;
                }
            }

            offset += len;
        }

        // -----------------------------------------------------------------
        // Step 8: SET_CONFIGURATION
        // -----------------------------------------------------------------
        cc = Xhci::ControlTransfer(slotId, REQTYPE_HOST_TO_DEV, REQ_SET_CONFIGURATION,
                                   cfgHdr.bConfigurationValue, 0, 0, nullptr, false);
        if (cc != Xhci::CC_SUCCESS) {
            KernelLogStream(ERROR, "USB") << "SET_CONFIGURATION failed, cc=" << (uint64_t)cc;
            dev->Active = false;
            return 0;
        }

        // -----------------------------------------------------------------
        // Step 9: Configure Endpoint (if HID interrupt endpoint was found)
        // -----------------------------------------------------------------
        if (foundEp) {
            // Device Context Index for an IN endpoint:  DCI = EpNum * 2 + 1
            uint8_t dci = dev->InterruptEpNum * 2 + 1;

            auto* inputCtx2 = (Xhci::InputContext*)Memory::g_pfa->AllocateZeroed();

            // ICC: Add slot context (bit 0) and the interrupt endpoint (bit dci)
            inputCtx2->ICC.AddFlags = (1 << 0) | (1 << dci);

            // Copy the current slot context from the output context
            inputCtx2->Slot = dev->OutputContext->Slot;

            // Update Context Entries in slot context to at least cover this DCI
            uint32_t newCtxEntries = dci;
            inputCtx2->Slot.Field0 = (inputCtx2->Slot.Field0 & ~(0x1Fu << 27))
                                   | (newCtxEntries << 27);

            // Allocate interrupt transfer ring
            auto* intRing = (Xhci::TRB*)Memory::g_pfa->AllocateZeroed();
            dev->InterruptRing        = intRing;
            dev->InterruptRingPhys    = Memory::SubHHDM(intRing);
            dev->InterruptRingEnqueue = 0;
            dev->InterruptRingCCS     = true;

            // Set up Link TRB at last position
            Xhci::TRB& intLink = intRing[Xhci::XFER_RING_SIZE - 1];
            intLink.Parameter0 = (uint32_t)(dev->InterruptRingPhys & 0xFFFFFFFF);
            intLink.Parameter1 = (uint32_t)(dev->InterruptRingPhys >> 32);
            intLink.Status = 0;
            intLink.Control = (Xhci::TRB_LINK << Xhci::TRB_TYPE_SHIFT) | Xhci::TRB_ENT;

            // Endpoint Context for the interrupt IN endpoint
            auto& epCtx = inputCtx2->EP[dci - 1];  // EP array is 0-indexed, DCI 1 = EP[0]

            // Field0: Interval (bits 23:16) — convert bInterval to xHCI encoding
            uint32_t xhciInterval = ConvertInterval(speed, dev->InterruptInterval);
            epCtx.Field0 = (xhciInterval << 16);

            // Field1: CErr=3 (bits 2:1), EP Type=Interrupt IN=7 (bits 5:3),
            //         Max Packet Size (bits 31:16)
            epCtx.Field1 = (3 << 1)
                         | (Xhci::EP_TYPE_INTERRUPT_IN << 3)
                         | ((uint32_t)dev->InterruptMaxPacket << 16);

            // TR Dequeue Pointer with DCS=1
            epCtx.TRDequeuePtr = dev->InterruptRingPhys | 1;

            // Average TRB Length
            epCtx.Field2 = dev->InterruptMaxPacket;

            // Send Configure Endpoint command
            Xhci::TRB cfgTrb = {};
            uint64_t inputCtx2Phys = Memory::SubHHDM(inputCtx2);
            cfgTrb.Parameter0 = (uint32_t)(inputCtx2Phys & 0xFFFFFFFF);
            cfgTrb.Parameter1 = (uint32_t)(inputCtx2Phys >> 32);
            cfgTrb.Control    = (Xhci::TRB_CONFIGURE_ENDPOINT << Xhci::TRB_TYPE_SHIFT)
                              | ((uint32_t)slotId << 24);

            cc = Xhci::SendCommand(cfgTrb);
            if (cc != Xhci::CC_SUCCESS) {
                KernelLogStream(ERROR, "USB") << "Configure Endpoint failed, slot="
                    << (uint64_t)slotId << " cc=" << (uint64_t)cc;
                dev->Active = false;
                return 0;
            }

            KernelLogStream(INFO, "USB") << "Slot " << (uint64_t)slotId
                << ": Interrupt EP " << (uint64_t)dev->InterruptEpNum
                << " configured (DCI " << (uint64_t)dci << ")";
        }

        // -----------------------------------------------------------------
        // Step 10: SET_PROTOCOL(0) -- request Boot Protocol
        // -----------------------------------------------------------------
        if (foundEp) {
            cc = Xhci::ControlTransfer(slotId, REQTYPE_CLASS_IFACE, REQ_SET_PROTOCOL,
                                       0, 0, 0, nullptr, false);
            if (cc != Xhci::CC_SUCCESS) {
                KernelLogStream(WARNING, "USB") << "SET_PROTOCOL(Boot) failed, cc=" << (uint64_t)cc;
                // Non-fatal: some devices only support boot protocol anyway
            }
        }

        // -----------------------------------------------------------------
        // Step 11: SET_IDLE(4) -- 16ms idle rate for software typematic
        // -----------------------------------------------------------------
        if (foundEp) {
            // wValue upper byte = duration in 4ms units, lower byte = report ID
            cc = Xhci::ControlTransfer(slotId, REQTYPE_CLASS_IFACE, REQ_SET_IDLE,
                                       (4 << 8), 0, 0, nullptr, false);
            if (cc != Xhci::CC_SUCCESS) {
                KernelLogStream(WARNING, "USB") << "SET_IDLE(4) failed, cc=" << (uint64_t)cc;
                // Non-fatal: not all devices support SET_IDLE
            }
        }

        // -----------------------------------------------------------------
        // Step 12: Queue first interrupt transfer
        // -----------------------------------------------------------------
        if (foundEp) {
            Xhci::QueueInterruptTransfer(slotId);
        }

        // -----------------------------------------------------------------
        // Step 13: Register with the appropriate HID driver
        // -----------------------------------------------------------------
        if (dev->InterfaceProtocol == PROTOCOL_KEYBOARD) {
            HidKeyboard::RegisterDevice(slotId);
            KernelLogStream(OK, "USB") << "Slot " << (uint64_t)slotId << ": HID Boot Keyboard";
        } else if (dev->InterfaceProtocol == PROTOCOL_MOUSE) {
            HidMouse::RegisterDevice(slotId);
            KernelLogStream(OK, "USB") << "Slot " << (uint64_t)slotId << ": HID Boot Mouse";
        } else if (foundEp) {
            KernelLogStream(INFO, "USB") << "Slot " << (uint64_t)slotId
                << ": HID device, protocol=" << (uint64_t)dev->InterfaceProtocol;
        } else {
            KernelLogStream(INFO, "USB") << "Slot " << (uint64_t)slotId
                << ": Non-HID device, class=" << (uint64_t)devDesc.bDeviceClass;
        }

        return slotId;
    }

}
