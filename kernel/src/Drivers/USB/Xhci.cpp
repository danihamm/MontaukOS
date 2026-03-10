/*
    * Xhci.cpp
    * xHCI (USB 3.x) Host Controller driver
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Xhci.hpp"
#include "UsbDevice.hpp"
#include "HidKeyboard.hpp"
#include "HidMouse.hpp"
#include <Pci/Pci.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Libraries/Memory.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

static void BusyWaitMs(uint64_t ms) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    if (flags & (1 << 9)) {
        // Interrupts enabled — use timer-based delay
        uint64_t start = Timekeeping::GetMilliseconds();
        while (Timekeeping::GetMilliseconds() - start < ms) {
            asm volatile("pause" ::: "memory");
        }
    } else {
        // Interrupts disabled (e.g. timer tick context) — use I/O port delay
        // Each outb to port 0x80 takes ~1µs on x86
        for (uint64_t i = 0; i < ms * 1000; i++) {
            asm volatile("outb %%al, $0x80" ::: "memory");
        }
    }
}

namespace Drivers::USB::Xhci {

    // -------------------------------------------------------------------------
    // Static state
    // -------------------------------------------------------------------------

    static bool g_initialized = false;
    static bool g_bootScanComplete = false;  // true after initial port scan finishes

    // Hot-plug deferred work
    static volatile bool g_hotplugPending[MAX_PORTS] = {};
    static bool g_hotplugProcessing = false;

    // MMIO region pointers
    static volatile uint8_t* g_mmioBase = nullptr;
    static uint8_t  g_capLength = 0;
    static volatile uint8_t* g_opBase = nullptr;
    static volatile uint8_t* g_rtBase = nullptr;
    static volatile uint8_t* g_dbBase = nullptr;

    // Controller parameters
    static uint32_t g_maxSlots = 0;
    static uint32_t g_maxPorts = 0;

    // DCBAA (Device Context Base Address Array) -- not static: accessed by UsbDevice.cpp
    uint64_t* g_dcbaa = nullptr;
    static uint64_t  g_dcbaaPhys = 0;

    // Command ring
    static TRB*     g_cmdRing = nullptr;
    static uint64_t g_cmdRingPhys = 0;
    static uint32_t g_cmdRingEnqueue = 0;
    static bool     g_cmdRingCCS = true;

    // Event ring
    static TRB*     g_evtRing = nullptr;
    static uint64_t g_evtRingPhys = 0;
    static uint32_t g_evtRingDequeue = 0;
    static bool     g_evtRingCCS = true;

    // Event Ring Segment Table
    static ERSTEntry* g_erst = nullptr;
    static uint64_t   g_erstPhys = 0;

    // Command completion tracking
    static volatile bool     g_cmdCompleted = false;
    static volatile uint32_t g_cmdCompletionCode = 0;
    volatile uint32_t g_cmdCompletionSlotId = 0;  // not static: accessed by UsbDevice.cpp

    // Transfer completion tracking (for EP0 control transfers during init)
    static volatile bool     g_xferCompleted = false;
    static volatile uint32_t g_xferCompletionCode = 0;

    // Per-device info
    static UsbDeviceInfo g_devices[MAX_SLOTS + 1] = {};

    // Interrupt transfer data buffers (per slot)
    static uint8_t* g_interruptDataBuf[MAX_SLOTS + 1] = {};
    static uint64_t g_interruptDataBufPhys[MAX_SLOTS + 1] = {};

    // Bulk IN transfer data buffers (per slot)
    static uint8_t* g_bulkInDataBuf[MAX_SLOTS + 1] = {};
    static uint64_t g_bulkInDataBufPhys[MAX_SLOTS + 1] = {};

    // Transfer callbacks for non-HID class drivers (per slot)
    static TransferCallback g_transferCallbacks[MAX_SLOTS + 1] = {};

    // Scratchpad buffer array
    static uint64_t* g_scratchpadBufs = nullptr;

    // -------------------------------------------------------------------------
    // Register access helpers
    // -------------------------------------------------------------------------

    static void WriteOp(uint32_t reg, uint32_t value) {
        *(volatile uint32_t*)(g_opBase + reg) = value;
    }

    static uint32_t ReadOp(uint32_t reg) {
        return *(volatile uint32_t*)(g_opBase + reg);
    }

    static void WriteRt(uint32_t reg, uint32_t value) {
        *(volatile uint32_t*)(g_rtBase + reg) = value;
    }

    static uint32_t ReadRt(uint32_t reg) {
        return *(volatile uint32_t*)(g_rtBase + reg);
    }

    static uint32_t ReadCap(uint32_t reg) {
        return *(volatile uint32_t*)(g_mmioBase + reg);
    }

    static void WriteDoorbell(uint32_t index, uint32_t value) {
        *(volatile uint32_t*)(g_dbBase + index * 4) = value;
    }

    // -------------------------------------------------------------------------
    // DMA buffer allocation
    // -------------------------------------------------------------------------

    static uint8_t* AllocateDmaBuffer(uint64_t& outPhysAddr) {
        void* virt = Memory::g_pfa->AllocateZeroed();
        outPhysAddr = Memory::SubHHDM(virt);
        return (uint8_t*)virt;
    }

    // -------------------------------------------------------------------------
    // Transfer ring advance helpers (handle Link TRB wrap)
    // -------------------------------------------------------------------------

    // Advance EP0 ring enqueue pointer, activating Link TRB when reached
    static void AdvanceEP0Ring(UsbDeviceInfo& dev) {
        dev.EP0RingEnqueue++;
        if (dev.EP0RingEnqueue >= XFER_RING_SIZE - 1) {
            // Reached Link TRB — set its cycle bit and wrap
            TRB& link = dev.EP0Ring[XFER_RING_SIZE - 1];
            if (dev.EP0RingCCS) {
                link.Control |= TRB_CYCLE_BIT;
            } else {
                link.Control &= ~TRB_CYCLE_BIT;
            }
            dev.EP0RingCCS = !dev.EP0RingCCS;
            dev.EP0RingEnqueue = 0;
        }
    }

    // Advance interrupt ring enqueue pointer, activating Link TRB when reached
    static void AdvanceInterruptRing(UsbDeviceInfo& dev) {
        dev.InterruptRingEnqueue++;
        if (dev.InterruptRingEnqueue >= XFER_RING_SIZE - 1) {
            TRB& link = dev.InterruptRing[XFER_RING_SIZE - 1];
            if (dev.InterruptRingCCS) {
                link.Control |= TRB_CYCLE_BIT;
            } else {
                link.Control &= ~TRB_CYCLE_BIT;
            }
            dev.InterruptRingCCS = !dev.InterruptRingCCS;
            dev.InterruptRingEnqueue = 0;
        }
    }

    // Advance bulk IN ring enqueue pointer, activating Link TRB when reached
    static void AdvanceBulkInRing(UsbDeviceInfo& dev) {
        dev.BulkInRingEnqueue++;
        if (dev.BulkInRingEnqueue >= XFER_RING_SIZE - 1) {
            TRB& link = dev.BulkInRing[XFER_RING_SIZE - 1];
            if (dev.BulkInRingCCS) {
                link.Control |= TRB_CYCLE_BIT;
            } else {
                link.Control &= ~TRB_CYCLE_BIT;
            }
            dev.BulkInRingCCS = !dev.BulkInRingCCS;
            dev.BulkInRingEnqueue = 0;
        }
    }

    // Advance bulk OUT ring enqueue pointer, activating Link TRB when reached
    static void AdvanceBulkOutRing(UsbDeviceInfo& dev) {
        dev.BulkOutRingEnqueue++;
        if (dev.BulkOutRingEnqueue >= XFER_RING_SIZE - 1) {
            TRB& link = dev.BulkOutRing[XFER_RING_SIZE - 1];
            if (dev.BulkOutRingCCS) {
                link.Control |= TRB_CYCLE_BIT;
            } else {
                link.Control &= ~TRB_CYCLE_BIT;
            }
            dev.BulkOutRingCCS = !dev.BulkOutRingCCS;
            dev.BulkOutRingEnqueue = 0;
        }
    }

    // -------------------------------------------------------------------------
    // Forward declarations
    // -------------------------------------------------------------------------

    static void HandleInterrupt(uint8_t irq);

    // -------------------------------------------------------------------------
    // MSI setup (same pattern as E1000E)
    // -------------------------------------------------------------------------

    static bool SetupMsi(uint8_t bus, uint8_t dev, uint8_t func) {
        uint8_t cap = Pci::FindCapability(bus, dev, func, Pci::PCI_CAP_MSI);
        if (cap == 0) {
            KernelLogStream(INFO, "xHCI") << "MSI capability not found";
            return false;
        }

        KernelLogStream(INFO, "xHCI") << "MSI capability at offset " << base::hex << (uint64_t)cap;

        // Read Message Control (cap+2)
        uint16_t msgCtrl = Pci::LegacyRead16(bus, dev, func, cap + 2);
        bool is64bit = (msgCtrl & (1 << 7)) != 0;

        // Write Message Address (cap+4): BSP APIC ID 0, physical destination, fixed delivery
        Pci::LegacyWrite32(bus, dev, func, cap + 4, MSI_ADDR_BASE);

        // Write Message Data (vector number, edge-triggered, fixed delivery)
        if (is64bit) {
            // 64-bit: Upper Address at cap+8, Data at cap+12
            Pci::LegacyWrite32(bus, dev, func, cap + 8, 0);
            Pci::LegacyWrite16(bus, dev, func, cap + 12, MSI_VECTOR);
        } else {
            // 32-bit: Data at cap+8
            Pci::LegacyWrite16(bus, dev, func, cap + 8, MSI_VECTOR);
        }

        // Enable MSI: set bit 0 (MSI Enable), clear bits 6:4 (single message)
        msgCtrl &= ~(0x70);  // Clear Multiple Message Enable (bits 6:4)
        msgCtrl |= (1 << 0); // MSI Enable
        Pci::LegacyWrite16(bus, dev, func, cap + 2, msgCtrl);

        // Disable legacy INTx in PCI command register
        uint16_t pciCmd = Pci::LegacyRead16(bus, dev, func, (uint8_t)Pci::PCI_REG_COMMAND);
        pciCmd |= Pci::PCI_CMD_INTX_DISABLE;
        Pci::LegacyWrite16(bus, dev, func, (uint8_t)Pci::PCI_REG_COMMAND, pciCmd);

        // Register the interrupt handler for MSI vector
        Hal::RegisterIrqHandler(MSI_IRQ, HandleInterrupt);

        KernelLogStream(OK, "xHCI") << "MSI enabled: vector " << base::dec << (uint64_t)MSI_VECTOR
            << " (IRQ slot " << (uint64_t)MSI_IRQ << ")" << (is64bit ? " [64-bit]" : " [32-bit]");

        return true;
    }

    // -------------------------------------------------------------------------
    // PollEvents - process event ring
    // -------------------------------------------------------------------------

    void PollEvents() {
        while (true) {
            TRB& evt = g_evtRing[g_evtRingDequeue];

            // Check if the cycle bit matches our expected cycle state
            bool evtCycle = (evt.Control & TRB_CYCLE_BIT) != 0;
            if (evtCycle != g_evtRingCCS) {
                break; // No more events
            }

            uint32_t trbType = (evt.Control & TRB_TYPE_MASK) >> TRB_TYPE_SHIFT;

            switch (trbType) {
                case TRB_COMMAND_COMPLETION: {
                    uint32_t completionCode = (evt.Status >> 24) & 0xFF;
                    uint32_t slotId = (evt.Control >> 24) & 0xFF;
                    g_cmdCompletionCode = completionCode;
                    g_cmdCompletionSlotId = slotId;
                    g_cmdCompleted = true;
                    break;
                }

                case TRB_PORT_STATUS_CHANGE: {
                    uint32_t portId = (evt.Parameter0 >> 24) & 0xFF;
                    uint32_t portsc = ReadOp(OP_PORTSC_BASE + (portId - 1) * OP_PORTSC_STRIDE);
                    // Clear change bits (write-1-to-clear)
                    WriteOp(OP_PORTSC_BASE + (portId - 1) * OP_PORTSC_STRIDE,
                            (portsc & PORTSC_PRESERVE) | PORTSC_CHANGE_BITS);

                    // Defer enumeration to ProcessDeferredWork (called from timer tick)
                    if (g_bootScanComplete && portId >= 1 && portId <= g_maxPorts) {
                        g_hotplugPending[portId - 1] = true;
                    }
                    break;
                }

                case TRB_TRANSFER_EVENT: {
                    uint32_t completionCode = (evt.Status >> 24) & 0xFF;
                    uint32_t slotId = (evt.Control >> 24) & 0xFF;
                    uint32_t epDci = (evt.Control >> 16) & 0x1F;

                    if (epDci == 1) {
                        // EP0 (DCI 1) - control transfer completion
                        g_xferCompletionCode = completionCode;
                        g_xferCompleted = true;
                    } else if (slotId > 0 && slotId <= MAX_SLOTS && g_devices[slotId].Active) {
                        UsbDeviceInfo& dev = g_devices[slotId];

                        if (completionCode == CC_SUCCESS || completionCode == CC_SHORT_PACKET) {
                            // Compute actual transfer length from residual
                            uint32_t residual = evt.Status & 0x00FFFFFF;

                            // Check if this is a bulk IN endpoint completion
                            uint8_t bulkInDci = dev.BulkInEpNum ? (dev.BulkInEpNum * 2 + 1) : 0;
                            uint8_t bulkOutDci = dev.BulkOutEpNum ? (dev.BulkOutEpNum * 2) : 0;
                            uint8_t intDci = dev.InterruptEpNum ? (dev.InterruptEpNum * 2 + 1) : 0;

                            if (epDci == bulkInDci && g_transferCallbacks[slotId]) {
                                // Bulk IN — dispatch via registered callback
                                uint16_t len = dev.BulkInMaxPacket;
                                if (residual < len) len = dev.BulkInMaxPacket - (uint16_t)residual;
                                g_transferCallbacks[slotId](slotId, epDci,
                                    g_bulkInDataBuf[slotId], len, completionCode);
                            } else if (epDci == bulkOutDci && g_transferCallbacks[slotId]) {
                                // Bulk OUT completion — notify callback
                                g_transferCallbacks[slotId](slotId, epDci,
                                    nullptr, 0, completionCode);
                            } else if (epDci == intDci) {
                                // Interrupt IN — HID or callback dispatch
                                uint16_t len = dev.InterruptMaxPacket;
                                if (residual < len) len = dev.InterruptMaxPacket - (uint16_t)residual;

                                if (dev.InterfaceClass == UsbDevice::CLASS_HID) {
                                    if (dev.InterfaceProtocol == UsbDevice::PROTOCOL_KEYBOARD) {
                                        HidKeyboard::ProcessReport(g_interruptDataBuf[slotId], len);
                                    } else if (dev.InterfaceProtocol == UsbDevice::PROTOCOL_MOUSE) {
                                        HidMouse::ProcessReport(g_interruptDataBuf[slotId], len);
                                    }
                                    // Re-queue for next HID report
                                    QueueInterruptTransfer(slotId);
                                } else if (g_transferCallbacks[slotId]) {
                                    // Callback is responsible for re-queuing
                                    g_transferCallbacks[slotId](slotId, epDci,
                                        g_interruptDataBuf[slotId], len, completionCode);
                                }
                            } else if (g_transferCallbacks[slotId]) {
                                // Unknown endpoint — try callback
                                g_transferCallbacks[slotId](slotId, epDci,
                                    nullptr, 0, completionCode);
                            }
                        } else {
                            KernelLogStream(WARNING, "xHCI") << "Transfer error on slot "
                                << base::dec << (uint64_t)slotId << " ep " << (uint64_t)epDci
                                << " cc=" << (uint64_t)completionCode;

                            // Notify callback of errors too
                            if (g_transferCallbacks[slotId]) {
                                g_transferCallbacks[slotId](slotId, epDci,
                                    nullptr, 0, completionCode);
                            }
                        }

                    }
                    break;
                }

                default:
                    break;
            }

            // Advance dequeue pointer
            g_evtRingDequeue++;
            if (g_evtRingDequeue >= EVT_RING_SIZE) {
                g_evtRingDequeue = 0;
                g_evtRingCCS = !g_evtRingCCS;
            }
        }

        // Update ERDP to tell the controller we have processed events
        // Bit 3 (EHB - Event Handler Busy) must be set to clear it
        uint64_t erdp = g_evtRingPhys + (uint64_t)g_evtRingDequeue * sizeof(TRB);
        erdp |= (1 << 3); // Set EHB to clear it
        WriteRt(IR0_ERDP, (uint32_t)(erdp & 0xFFFFFFFF));
        WriteRt(IR0_ERDP + 4, (uint32_t)(erdp >> 32));

    }

    // -------------------------------------------------------------------------
    // HandleInterrupt
    // -------------------------------------------------------------------------

    static void HandleInterrupt(uint8_t irq) {
        (void)irq;

        // Clear USBSTS.EINT only (don't accidentally clear other W1C bits)
        WriteOp(OP_USBSTS, USBSTS_EINT);

        // Clear IMAN.IP and ensure IE stays enabled
        WriteRt(IR0_IMAN, IMAN_IP | IMAN_IE);

        PollEvents();
    }

    // -------------------------------------------------------------------------
    // SendCommand - send a command TRB on the command ring
    // -------------------------------------------------------------------------

    uint32_t SendCommand(const TRB& trb) {
        // Place TRB at current enqueue position
        TRB& slot = g_cmdRing[g_cmdRingEnqueue];
        slot.Parameter0 = trb.Parameter0;
        slot.Parameter1 = trb.Parameter1;
        slot.Status = trb.Status;

        // Set the type and cycle bit in control
        uint32_t control = trb.Control & ~TRB_CYCLE_BIT;
        if (g_cmdRingCCS) {
            control |= TRB_CYCLE_BIT;
        }
        slot.Control = control;

        // Advance enqueue pointer
        g_cmdRingEnqueue++;
        if (g_cmdRingEnqueue >= CMD_RING_SIZE - 1) {
            // We've reached the Link TRB - toggle its cycle bit and wrap
            TRB& link = g_cmdRing[CMD_RING_SIZE - 1];
            // Update the link TRB cycle bit to match current CCS
            if (g_cmdRingCCS) {
                link.Control |= TRB_CYCLE_BIT;
            } else {
                link.Control &= ~TRB_CYCLE_BIT;
            }
            g_cmdRingCCS = !g_cmdRingCCS;
            g_cmdRingEnqueue = 0;
        }

        // Clear completion flag and ring the host controller doorbell
        g_cmdCompleted = false;
        WriteDoorbell(0, 0);

        // Poll until command completes (with timeout)
        for (uint32_t i = 0; i < 100000; i++) {
            PollEvents();
            if (g_cmdCompleted) {
                return g_cmdCompletionCode;
            }
            // Small delay
            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }

        KernelLogStream(WARNING, "xHCI") << "Command timeout";
        return 0xFF;
    }

    // -------------------------------------------------------------------------
    // ControlTransfer - perform a control transfer on EP0
    // -------------------------------------------------------------------------

    uint32_t ControlTransfer(uint8_t slotId, uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                             void* data, bool dirIn) {

        if (slotId == 0 || slotId > MAX_SLOTS || !g_devices[slotId].Active) {
            return 0xFF;
        }

        UsbDeviceInfo& dev = g_devices[slotId];

        // --- Setup Stage TRB ---
        TRB& setup = dev.EP0Ring[dev.EP0RingEnqueue];
        setup.Parameter0 = (uint32_t)bmRequestType | ((uint32_t)bRequest << 8) | ((uint32_t)wValue << 16);
        setup.Parameter1 = (uint32_t)wIndex | ((uint32_t)wLength << 16);
        setup.Status = 8; // Setup packet is always 8 bytes

        uint32_t setupControl = (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT;
        if (wLength > 0) {
            setupControl |= dirIn ? TRB_TRT_IN : TRB_TRT_OUT;
        } else {
            setupControl |= TRB_TRT_NODATA;
        }
        if (dev.EP0RingCCS) {
            setupControl |= TRB_CYCLE_BIT;
        }
        setup.Control = setupControl;

        // Advance EP0 enqueue (handles Link TRB wrap)
        AdvanceEP0Ring(dev);

        // --- Data Stage TRB (if wLength > 0) ---
        if (wLength > 0 && data != nullptr) {
            uint64_t dataPhys = Memory::SubHHDM(data);

            TRB& dataTrb = dev.EP0Ring[dev.EP0RingEnqueue];
            dataTrb.Parameter0 = (uint32_t)(dataPhys & 0xFFFFFFFF);
            dataTrb.Parameter1 = (uint32_t)(dataPhys >> 32);
            dataTrb.Status = wLength;

            uint32_t dataControl = (TRB_DATA_STAGE << TRB_TYPE_SHIFT);
            if (dirIn) {
                dataControl |= TRB_DIR_IN;
            }
            if (dev.EP0RingCCS) {
                dataControl |= TRB_CYCLE_BIT;
            }
            dataTrb.Control = dataControl;

            AdvanceEP0Ring(dev);
        }

        // --- Status Stage TRB ---
        TRB& status = dev.EP0Ring[dev.EP0RingEnqueue];
        status.Parameter0 = 0;
        status.Parameter1 = 0;
        status.Status = 0;

        // Status stage direction is opposite of data stage
        uint32_t statusControl = (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC;
        if (wLength > 0 && !dirIn) {
            statusControl |= TRB_DIR_IN;
        } else if (wLength == 0) {
            statusControl |= TRB_DIR_IN;
        }
        if (dev.EP0RingCCS) {
            statusControl |= TRB_CYCLE_BIT;
        }
        status.Control = statusControl;

        AdvanceEP0Ring(dev);

        // Ring doorbell for this slot, target EP0 (DCI 1)
        g_xferCompleted = false;
        WriteDoorbell(slotId, 1);

        // Poll until transfer completes
        for (uint32_t i = 0; i < 100000; i++) {
            PollEvents();
            if (g_xferCompleted) {
                return g_xferCompletionCode;
            }
            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }

        KernelLogStream(WARNING, "xHCI") << "Control transfer timeout on slot " << base::dec << (uint64_t)slotId;

        // Recover EP0 ring: Stop Endpoint, then Set TR Dequeue Pointer
        // so that subsequent control transfers on this slot still work.
        TRB stopTrb = {};
        stopTrb.Control = (TRB_STOP_ENDPOINT << TRB_TYPE_SHIFT)
                        | ((uint32_t)slotId << 24)
                        | (1 << 16);  // DCI=1 (EP0) in bits 20:16
        SendCommand(stopTrb);

        // Reset the enqueue pointer to the start and set the dequeue pointer
        // to match so the ring is back in sync.
        uint64_t newDeq = dev.EP0RingPhys
                        + (uint64_t)dev.EP0RingEnqueue * sizeof(TRB);
        if (dev.EP0RingCCS) {
            newDeq |= 1; // DCS bit
        }

        TRB deqTrb = {};
        deqTrb.Parameter0 = (uint32_t)(newDeq & 0xFFFFFFFF);
        deqTrb.Parameter1 = (uint32_t)(newDeq >> 32);
        deqTrb.Control    = (TRB_SET_TR_DEQUEUE << TRB_TYPE_SHIFT)
                          | ((uint32_t)slotId << 24)
                          | (1 << 16);  // DCI=1 (EP0)
        SendCommand(deqTrb);

        return 0xFF;
    }

    // -------------------------------------------------------------------------
    // QueueInterruptTransfer
    // -------------------------------------------------------------------------

    void QueueInterruptTransfer(uint8_t slotId) {
        if (slotId == 0 || slotId > MAX_SLOTS || !g_devices[slotId].Active) {
            return;
        }

        UsbDeviceInfo& dev = g_devices[slotId];

        // Allocate interrupt data buffer if not yet allocated
        if (g_interruptDataBuf[slotId] == nullptr) {
            g_interruptDataBuf[slotId] = AllocateDmaBuffer(g_interruptDataBufPhys[slotId]);
        }

        // Build a Normal TRB on the interrupt ring
        TRB& trb = dev.InterruptRing[dev.InterruptRingEnqueue];
        trb.Parameter0 = (uint32_t)(g_interruptDataBufPhys[slotId] & 0xFFFFFFFF);
        trb.Parameter1 = (uint32_t)(g_interruptDataBufPhys[slotId] >> 32);
        trb.Status = dev.InterruptMaxPacket;

        uint32_t control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | TRB_ISP;
        if (dev.InterruptRingCCS) {
            control |= TRB_CYCLE_BIT;
        }
        trb.Control = control;

        // Advance enqueue (handles Link TRB wrap)
        AdvanceInterruptRing(dev);

        // Ring doorbell: target = (InterruptEpNum * 2 + 1) for IN endpoint DCI
        uint8_t target = dev.InterruptEpNum * 2 + 1;
        WriteDoorbell(slotId, target);
    }

    // -------------------------------------------------------------------------
    // QueueBulkInTransfer
    // -------------------------------------------------------------------------

    void QueueBulkInTransfer(uint8_t slotId, uint8_t* data, uint64_t dataPhys, uint32_t length) {
        if (slotId == 0 || slotId > MAX_SLOTS || !g_devices[slotId].Active) return;

        UsbDeviceInfo& dev = g_devices[slotId];
        if (!dev.BulkInRing || dev.BulkInEpNum == 0) return;

        // If caller provides nullptr, use the per-slot bulk IN DMA buffer
        if (data == nullptr) {
            if (g_bulkInDataBuf[slotId] == nullptr) {
                g_bulkInDataBuf[slotId] = AllocateDmaBuffer(g_bulkInDataBufPhys[slotId]);
            }
            data = g_bulkInDataBuf[slotId];
            dataPhys = g_bulkInDataBufPhys[slotId];
        }

        TRB& trb = dev.BulkInRing[dev.BulkInRingEnqueue];
        trb.Parameter0 = (uint32_t)(dataPhys & 0xFFFFFFFF);
        trb.Parameter1 = (uint32_t)(dataPhys >> 32);
        trb.Status = length;

        uint32_t control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | TRB_ISP;
        if (dev.BulkInRingCCS) {
            control |= TRB_CYCLE_BIT;
        }
        trb.Control = control;

        AdvanceBulkInRing(dev);

        // Ring doorbell: DCI for bulk IN = EpNum * 2 + 1
        uint8_t target = dev.BulkInEpNum * 2 + 1;
        WriteDoorbell(slotId, target);
    }

    // -------------------------------------------------------------------------
    // QueueBulkOutTransfer
    // -------------------------------------------------------------------------

    void QueueBulkOutTransfer(uint8_t slotId, uint8_t* data, uint64_t dataPhys, uint32_t length) {
        if (slotId == 0 || slotId > MAX_SLOTS || !g_devices[slotId].Active) return;

        UsbDeviceInfo& dev = g_devices[slotId];
        if (!dev.BulkOutRing || dev.BulkOutEpNum == 0) return;

        TRB& trb = dev.BulkOutRing[dev.BulkOutRingEnqueue];
        trb.Parameter0 = (uint32_t)(dataPhys & 0xFFFFFFFF);
        trb.Parameter1 = (uint32_t)(dataPhys >> 32);
        trb.Status = length;

        uint32_t control = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC;
        if (dev.BulkOutRingCCS) {
            control |= TRB_CYCLE_BIT;
        }
        trb.Control = control;

        AdvanceBulkOutRing(dev);

        // Ring doorbell: DCI for bulk OUT = EpNum * 2
        uint8_t target = dev.BulkOutEpNum * 2;
        WriteDoorbell(slotId, target);
    }

    // -------------------------------------------------------------------------
    // RegisterTransferCallback
    // -------------------------------------------------------------------------

    void RegisterTransferCallback(uint8_t slotId, TransferCallback cb) {
        if (slotId > 0 && slotId <= MAX_SLOTS) {
            g_transferCallbacks[slotId] = cb;
        }
    }

    // -------------------------------------------------------------------------
    // RingDoorbell
    // -------------------------------------------------------------------------

    void RingDoorbell(uint8_t slotId, uint8_t target) {
        WriteDoorbell(slotId, target);
    }

    // -------------------------------------------------------------------------
    // GetDevice
    // -------------------------------------------------------------------------

    UsbDeviceInfo* GetDevice(uint8_t slotId) {
        if (slotId == 0 || slotId > MAX_SLOTS) {
            return nullptr;
        }
        return &g_devices[slotId];
    }

    // -------------------------------------------------------------------------
    // IsInitialized
    // -------------------------------------------------------------------------

    bool IsInitialized() {
        return g_initialized;
    }

    // -------------------------------------------------------------------------
    // ProcessDeferredWork - handle hot-plug outside interrupt context
    // Called from timer tick (same pattern as E1000E::Poll)
    // -------------------------------------------------------------------------

    void ProcessDeferredWork() {
        if (!g_initialized || !g_bootScanComplete) return;
        if (g_hotplugProcessing) return;
        g_hotplugProcessing = true;

        for (uint32_t port = 0; port < g_maxPorts; port++) {
            if (!g_hotplugPending[port]) continue;
            g_hotplugPending[port] = false;

            uint32_t portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);

            if (portsc & PORTSC_CCS) {
                // Device connected — check if already assigned to a slot
                bool alreadyActive = false;
                for (uint8_t s = 1; s <= MAX_SLOTS; s++) {
                    if (g_devices[s].Active && g_devices[s].PortId == port + 1) {
                        alreadyActive = true;
                        break;
                    }
                }
                if (alreadyActive) continue;

                if (portsc & PORTSC_PED) {
                    // Already enabled — enumerate after recovery delay
                    uint32_t speed = (portsc >> 10) & 0xF;
                    BusyWaitMs(10);
                    UsbDevice::EnumerateDevice(port + 1, speed);
                } else {
                    // Need port reset first
                    WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE,
                            (portsc & PORTSC_PRESERVE) | PORTSC_PR | PORTSC_CHANGE_BITS);

                    bool resetDone = false;
                    for (uint32_t i = 0; i < 100000; i++) {
                        PollEvents();
                        uint32_t ps = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
                        if (ps & PORTSC_PRC) {
                            resetDone = true;
                            break;
                        }
                        for (int j = 0; j < 100; j++) {
                            asm volatile("" ::: "memory");
                        }
                    }

                    if (!resetDone) {
                        KernelLogStream(WARNING, "xHCI") << "Hot-plug: port "
                            << base::dec << (uint64_t)(port + 1) << " reset timeout";
                        continue;
                    }

                    portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
                    uint32_t speed = (portsc >> 10) & 0xF;

                    // Clear change bits
                    WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE,
                            (portsc & PORTSC_PRESERVE) | PORTSC_CHANGE_BITS);

                    // Post-reset recovery delay (USB spec requires >= 10ms)
                    BusyWaitMs(10);

                    UsbDevice::EnumerateDevice(port + 1, speed);
                }
            } else {
                // Device disconnected — deactivate its slot
                for (uint8_t s = 1; s <= MAX_SLOTS; s++) {
                    if (g_devices[s].Active && g_devices[s].PortId == port + 1) {
                        g_devices[s].Active = false;
                        KernelLogStream(INFO, "xHCI") << "Hot-unplug: slot "
                            << base::dec << (uint64_t)s << " (port "
                            << (uint64_t)(port + 1) << ") deactivated";
                        break;
                    }
                }
            }
        }

        g_hotplugProcessing = false;
    }

    // -------------------------------------------------------------------------
    // Probe (called by PCI driver matching framework)
    // -------------------------------------------------------------------------

    bool Probe(const Pci::PciDevice& dev) {
        if (g_initialized) return false;

        KernelLogStream(OK, "xHCI") << "Found controller at PCI "
            << base::hex << (uint64_t)dev.Bus << ":"
            << (uint64_t)dev.Device << "." << (uint64_t)dev.Function;

        // Read BAR0 and map MMIO region
        uint64_t mmioPhys = Pci::ReadBar0(dev.Bus, dev.Device, dev.Function);

        KernelLogStream(INFO, "xHCI") << "BAR0 physical: " << base::hex << mmioPhys;

        constexpr uint64_t MmioSize = 0x10000;
        for (uint64_t offset = 0; offset < MmioSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }

        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        Pci::EnableBusMaster(dev.Bus, dev.Device, dev.Function);
        KernelLogStream(OK, "xHCI") << "Bus mastering enabled";

        // Parse capability registers
        g_capLength = *(volatile uint8_t*)(g_mmioBase + CAP_CAPLENGTH);

        uint32_t hciVersion = *(volatile uint16_t*)(g_mmioBase + CAP_HCIVERSION);
        KernelLogStream(INFO, "xHCI") << "Version: " << base::hex << (uint64_t)hciVersion
            << ", CapLength: " << (uint64_t)g_capLength;

        uint32_t hcsParams1 = ReadCap(CAP_HCSPARAMS1);
        g_maxSlots = hcsParams1 & 0xFF;
        g_maxPorts = (hcsParams1 >> 24) & 0xFF;

        uint32_t hcsParams2 = ReadCap(CAP_HCSPARAMS2);
        uint32_t scratchpadBufsHi = (hcsParams2 >> 21) & 0x1F;
        uint32_t scratchpadBufsLo = (hcsParams2 >> 27) & 0x1F;
        uint32_t maxScratchpadBufs = (scratchpadBufsHi << 5) | scratchpadBufsLo;

        uint32_t dbOff = ReadCap(CAP_DBOFF) & ~0x3u;
        uint32_t rtsOff = ReadCap(CAP_RTSOFF) & ~0x1Fu;

        g_opBase = g_mmioBase + g_capLength;
        g_rtBase = g_mmioBase + rtsOff;
        g_dbBase = g_mmioBase + dbOff;

        KernelLogStream(INFO, "xHCI") << "MaxSlots: " << base::dec << (uint64_t)g_maxSlots
            << ", MaxPorts: " << (uint64_t)g_maxPorts
            << ", ScratchpadBufs: " << (uint64_t)maxScratchpadBufs;

        if (g_maxSlots > MAX_SLOTS) g_maxSlots = MAX_SLOTS;
        if (g_maxPorts > MAX_PORTS) g_maxPorts = MAX_PORTS;

        // Halt controller
        uint32_t usbcmd = ReadOp(OP_USBCMD);
        usbcmd &= ~USBCMD_RS;
        WriteOp(OP_USBCMD, usbcmd);

        for (uint32_t i = 0; i < 100000; i++) {
            if (ReadOp(OP_USBSTS) & USBSTS_HCH) break;
            for (int j = 0; j < 10; j++) asm volatile("" ::: "memory");
        }

        if (!(ReadOp(OP_USBSTS) & USBSTS_HCH)) {
            KernelLogStream(WARNING, "xHCI") << "Controller failed to halt";
        }
        KernelLogStream(OK, "xHCI") << "Controller halted";

        // Reset controller
        WriteOp(OP_USBCMD, USBCMD_HCRST);
        for (uint32_t i = 0; i < 100000; i++) {
            if (!(ReadOp(OP_USBCMD) & USBCMD_HCRST)) break;
            for (int j = 0; j < 10; j++) asm volatile("" ::: "memory");
        }
        for (uint32_t i = 0; i < 100000; i++) {
            if (!(ReadOp(OP_USBSTS) & USBSTS_CNR)) break;
            for (int j = 0; j < 10; j++) asm volatile("" ::: "memory");
        }
        if (ReadOp(OP_USBSTS) & USBSTS_CNR) {
            KernelLogStream(WARNING, "xHCI") << "Controller not ready after reset";
        }
        KernelLogStream(OK, "xHCI") << "Controller reset complete";

        // Program CONFIG
        WriteOp(OP_CONFIG, g_maxSlots);

        // Allocate DCBAA
        g_dcbaa = (uint64_t*)AllocateDmaBuffer(g_dcbaaPhys);
        WriteOp(OP_DCBAAP, (uint32_t)(g_dcbaaPhys & 0xFFFFFFFF));
        WriteOp(OP_DCBAAP + 4, (uint32_t)(g_dcbaaPhys >> 32));
        KernelLogStream(OK, "xHCI") << "DCBAA at phys " << base::hex << g_dcbaaPhys;

        // Scratchpad buffers
        if (maxScratchpadBufs > 0) {
            uint64_t spArrayPhys;
            g_scratchpadBufs = (uint64_t*)AllocateDmaBuffer(spArrayPhys);
            for (uint32_t i = 0; i < maxScratchpadBufs; i++) {
                uint64_t bufPhys;
                AllocateDmaBuffer(bufPhys);
                g_scratchpadBufs[i] = bufPhys;
            }
            g_dcbaa[0] = spArrayPhys;
            KernelLogStream(OK, "xHCI") << "Allocated " << base::dec << (uint64_t)maxScratchpadBufs << " scratchpad buffers";
        }

        // Command ring
        g_cmdRing = (TRB*)AllocateDmaBuffer(g_cmdRingPhys);
        TRB& linkTrb = g_cmdRing[CMD_RING_SIZE - 1];
        linkTrb.Parameter0 = (uint32_t)(g_cmdRingPhys & 0xFFFFFFFF);
        linkTrb.Parameter1 = (uint32_t)(g_cmdRingPhys >> 32);
        linkTrb.Status = 0;
        linkTrb.Control = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_ENT;
        uint64_t crcr = g_cmdRingPhys | TRB_CYCLE_BIT;
        WriteOp(OP_CRCR, (uint32_t)(crcr & 0xFFFFFFFF));
        WriteOp(OP_CRCR + 4, (uint32_t)(crcr >> 32));
        g_cmdRingCCS = true;
        g_cmdRingEnqueue = 0;
        KernelLogStream(OK, "xHCI") << "Command ring at phys " << base::hex << g_cmdRingPhys;

        // Event ring + ERST
        g_evtRing = (TRB*)AllocateDmaBuffer(g_evtRingPhys);
        g_erst = (ERSTEntry*)AllocateDmaBuffer(g_erstPhys);
        g_erst[0].RingSegmentBase = g_evtRingPhys;
        g_erst[0].RingSegmentSize = EVT_RING_SIZE;
        g_erst[0].Reserved = 0;
        WriteRt(IR0_ERSTSZ, 1);
        WriteRt(IR0_ERDP, (uint32_t)(g_evtRingPhys & 0xFFFFFFFF));
        WriteRt(IR0_ERDP + 4, (uint32_t)(g_evtRingPhys >> 32));
        WriteRt(IR0_ERSTBA, (uint32_t)(g_erstPhys & 0xFFFFFFFF));
        WriteRt(IR0_ERSTBA + 4, (uint32_t)(g_erstPhys >> 32));
        g_evtRingCCS = true;
        g_evtRingDequeue = 0;
        KernelLogStream(OK, "xHCI") << "Event ring at phys " << base::hex << g_evtRingPhys;

        // MSI setup
        if (!SetupMsi(dev.Bus, dev.Device, dev.Function)) {
            KernelLogStream(WARNING, "xHCI") << "MSI not available, using poll mode";
        }

        // Enable interrupter 0
        WriteRt(IR0_IMAN, IMAN_IE);
        WriteRt(IR0_IMOD, 0);

        // Start controller
        WriteOp(OP_USBCMD, USBCMD_RS | USBCMD_INTE | USBCMD_HSEE);
        for (uint32_t i = 0; i < 100000; i++) {
            if (!(ReadOp(OP_USBSTS) & USBSTS_HCH)) break;
            for (int j = 0; j < 10; j++) asm volatile("" ::: "memory");
        }
        KernelLogStream(OK, "xHCI") << "Controller started";
        g_initialized = true;

        // Power on all ports
        for (uint32_t port = 0; port < g_maxPorts; port++) {
            uint32_t portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
            if (!(portsc & PORTSC_PP)) {
                WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE, PORTSC_PP);
            }
        }
        BusyWaitMs(20);
        KernelLogStream(OK, "xHCI") << "All ports powered";

        // Port scanning
        for (uint32_t port = 0; port < g_maxPorts; port++) {
            uint32_t portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
            if (!(portsc & PORTSC_CCS)) continue;

            KernelLogStream(INFO, "xHCI") << "Port " << base::dec << (uint64_t)(port + 1)
                << ": device connected, PORTSC=" << base::hex << (uint64_t)portsc;

            WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE,
                    (portsc & PORTSC_PRESERVE) | PORTSC_PR | PORTSC_CHANGE_BITS);

            bool resetDone = false;
            for (uint32_t i = 0; i < 100000; i++) {
                PollEvents();
                uint32_t ps = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
                if (ps & PORTSC_PRC) { resetDone = true; break; }
                for (int j = 0; j < 100; j++) asm volatile("" ::: "memory");
            }

            if (!resetDone) {
                KernelLogStream(WARNING, "xHCI") << "Port " << base::dec << (uint64_t)(port + 1) << " reset timeout";
                continue;
            }

            portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
            uint32_t speed = (portsc >> 10) & 0xF;
            WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE,
                    (portsc & PORTSC_PRESERVE) | PORTSC_CHANGE_BITS);

            const char* speedStr = "Unknown";
            switch (speed) {
                case SPEED_FULL:  speedStr = "Full (12 Mbps)"; break;
                case SPEED_LOW:   speedStr = "Low (1.5 Mbps)"; break;
                case SPEED_HIGH:  speedStr = "High (480 Mbps)"; break;
                case SPEED_SUPER: speedStr = "Super (5 Gbps)"; break;
            }

            KernelLogStream(OK, "xHCI") << "Port " << base::dec << (uint64_t)(port + 1)
                << ": reset complete, speed=" << speedStr;

            BusyWaitMs(10);
            UsbDevice::EnumerateDevice(port + 1, speed);
        }

        g_bootScanComplete = true;
        KernelLogStream(OK, "xHCI") << "Initialization complete";
        return true;
    }

    // -------------------------------------------------------------------------
    // Initialize (standalone fallback path)
    // -------------------------------------------------------------------------

    void Initialize() {
        KernelLogStream(INFO, "xHCI") << "Scanning for xHCI controller...";

        // -----------------------------------------------------------------
        // Step 1: Find xHCI controller on PCI bus
        // -----------------------------------------------------------------
        auto& devices = Pci::GetDevices();
        const Pci::PciDevice* foundDev = nullptr;

        for (uint64_t i = 0; i < devices.size(); i++) {
            if (devices[i].ClassCode == PCI_CLASS_SERIAL &&
                devices[i].SubClass == PCI_SUBCLASS_USB &&
                devices[i].ProgIf == PCI_PROGIF_XHCI) {
                foundDev = &devices[i];
                break;
            }
        }

        if (foundDev == nullptr) {
            KernelLogStream(WARNING, "xHCI") << "No xHCI controller found";
            return;
        }

        KernelLogStream(OK, "xHCI") << "Found controller at PCI "
            << base::hex << (uint64_t)foundDev->Bus << ":"
            << (uint64_t)foundDev->Device << "." << (uint64_t)foundDev->Function;

        // -----------------------------------------------------------------
        // Step 2: Read BAR0 and map MMIO region
        // -----------------------------------------------------------------
        uint64_t mmioPhys = Pci::ReadBar0(foundDev->Bus, foundDev->Device, foundDev->Function);

        KernelLogStream(INFO, "xHCI") << "BAR0 physical: " << base::hex << mmioPhys;

        // Map 64KB (16 pages) of MMIO space
        constexpr uint64_t MmioSize = 0x10000;
        for (uint64_t offset = 0; offset < MmioSize; offset += 0x1000) {
            Memory::VMM::g_paging->MapMMIO(mmioPhys + offset, Memory::HHDM(mmioPhys + offset));
        }

        g_mmioBase = (volatile uint8_t*)Memory::HHDM(mmioPhys);

        // -----------------------------------------------------------------
        // Step 3: Enable PCI bus master and memory space
        // -----------------------------------------------------------------
        Pci::EnableBusMaster(foundDev->Bus, foundDev->Device, foundDev->Function);

        KernelLogStream(OK, "xHCI") << "Bus mastering enabled";

        // -----------------------------------------------------------------
        // Step 4: Parse capability registers
        // -----------------------------------------------------------------
        g_capLength = *(volatile uint8_t*)(g_mmioBase + CAP_CAPLENGTH);

        uint32_t hciVersion = *(volatile uint16_t*)(g_mmioBase + CAP_HCIVERSION);
        KernelLogStream(INFO, "xHCI") << "Version: " << base::hex << (uint64_t)hciVersion
            << ", CapLength: " << (uint64_t)g_capLength;

        uint32_t hcsParams1 = ReadCap(CAP_HCSPARAMS1);
        g_maxSlots = hcsParams1 & 0xFF;
        g_maxPorts = (hcsParams1 >> 24) & 0xFF;

        uint32_t hcsParams2 = ReadCap(CAP_HCSPARAMS2);
        uint32_t scratchpadBufsHi = (hcsParams2 >> 21) & 0x1F;
        uint32_t scratchpadBufsLo = (hcsParams2 >> 27) & 0x1F;
        uint32_t maxScratchpadBufs = (scratchpadBufsHi << 5) | scratchpadBufsLo;

        uint32_t dbOff = ReadCap(CAP_DBOFF) & ~0x3u;
        uint32_t rtsOff = ReadCap(CAP_RTSOFF) & ~0x1Fu;

        g_opBase = g_mmioBase + g_capLength;
        g_rtBase = g_mmioBase + rtsOff;
        g_dbBase = g_mmioBase + dbOff;

        KernelLogStream(INFO, "xHCI") << "MaxSlots: " << base::dec << (uint64_t)g_maxSlots
            << ", MaxPorts: " << (uint64_t)g_maxPorts
            << ", ScratchpadBufs: " << (uint64_t)maxScratchpadBufs;

        // Cap slots to our maximum
        if (g_maxSlots > MAX_SLOTS) {
            g_maxSlots = MAX_SLOTS;
        }
        if (g_maxPorts > MAX_PORTS) {
            g_maxPorts = MAX_PORTS;
        }

        // -----------------------------------------------------------------
        // Step 5: Halt controller
        // -----------------------------------------------------------------
        uint32_t usbcmd = ReadOp(OP_USBCMD);
        usbcmd &= ~USBCMD_RS;
        WriteOp(OP_USBCMD, usbcmd);

        // Wait for HCH (Halted) to be set
        for (uint32_t i = 0; i < 100000; i++) {
            if (ReadOp(OP_USBSTS) & USBSTS_HCH) {
                break;
            }
            for (int j = 0; j < 10; j++) {
                asm volatile("" ::: "memory");
            }
        }

        if (!(ReadOp(OP_USBSTS) & USBSTS_HCH)) {
            KernelLogStream(WARNING, "xHCI") << "Controller failed to halt";
        }

        KernelLogStream(OK, "xHCI") << "Controller halted";

        // -----------------------------------------------------------------
        // Step 6: Reset controller
        // -----------------------------------------------------------------
        WriteOp(OP_USBCMD, USBCMD_HCRST);

        // Wait for HCRST to clear
        for (uint32_t i = 0; i < 100000; i++) {
            if (!(ReadOp(OP_USBCMD) & USBCMD_HCRST)) {
                break;
            }
            for (int j = 0; j < 10; j++) {
                asm volatile("" ::: "memory");
            }
        }

        // Wait for CNR (Controller Not Ready) to clear
        for (uint32_t i = 0; i < 100000; i++) {
            if (!(ReadOp(OP_USBSTS) & USBSTS_CNR)) {
                break;
            }
            for (int j = 0; j < 10; j++) {
                asm volatile("" ::: "memory");
            }
        }

        if (ReadOp(OP_USBSTS) & USBSTS_CNR) {
            KernelLogStream(WARNING, "xHCI") << "Controller not ready after reset";
        }

        KernelLogStream(OK, "xHCI") << "Controller reset complete";

        // -----------------------------------------------------------------
        // Step 7: Program CONFIG register (MaxSlotsEn)
        // -----------------------------------------------------------------
        WriteOp(OP_CONFIG, g_maxSlots);

        // -----------------------------------------------------------------
        // Step 8: Allocate DCBAA
        // -----------------------------------------------------------------
        g_dcbaa = (uint64_t*)AllocateDmaBuffer(g_dcbaaPhys);

        // Write DCBAAP (64-bit, split into two 32-bit writes)
        WriteOp(OP_DCBAAP, (uint32_t)(g_dcbaaPhys & 0xFFFFFFFF));
        WriteOp(OP_DCBAAP + 4, (uint32_t)(g_dcbaaPhys >> 32));

        KernelLogStream(OK, "xHCI") << "DCBAA at phys " << base::hex << g_dcbaaPhys;

        // -----------------------------------------------------------------
        // Step 9: Scratchpad buffers
        // -----------------------------------------------------------------
        if (maxScratchpadBufs > 0) {
            uint64_t spArrayPhys;
            g_scratchpadBufs = (uint64_t*)AllocateDmaBuffer(spArrayPhys);

            for (uint32_t i = 0; i < maxScratchpadBufs; i++) {
                uint64_t bufPhys;
                AllocateDmaBuffer(bufPhys);
                g_scratchpadBufs[i] = bufPhys;
            }

            // DCBAA[0] = physical address of the scratchpad buffer array
            g_dcbaa[0] = spArrayPhys;

            KernelLogStream(OK, "xHCI") << "Allocated " << base::dec << (uint64_t)maxScratchpadBufs << " scratchpad buffers";
        }

        // -----------------------------------------------------------------
        // Step 10: Command ring
        // -----------------------------------------------------------------
        g_cmdRing = (TRB*)AllocateDmaBuffer(g_cmdRingPhys);

        // Set up Link TRB at the last position
        TRB& linkTrb = g_cmdRing[CMD_RING_SIZE - 1];
        linkTrb.Parameter0 = (uint32_t)(g_cmdRingPhys & 0xFFFFFFFF);
        linkTrb.Parameter1 = (uint32_t)(g_cmdRingPhys >> 32);
        linkTrb.Status = 0;
        linkTrb.Control = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_ENT;
        // Toggle Cycle bit is bit 1 in the Link TRB control - use TRB_ENT which is bit 1

        // Write CRCR = command ring physical address | cycle bit 1
        uint64_t crcr = g_cmdRingPhys | TRB_CYCLE_BIT;
        WriteOp(OP_CRCR, (uint32_t)(crcr & 0xFFFFFFFF));
        WriteOp(OP_CRCR + 4, (uint32_t)(crcr >> 32));

        g_cmdRingCCS = true;
        g_cmdRingEnqueue = 0;

        KernelLogStream(OK, "xHCI") << "Command ring at phys " << base::hex << g_cmdRingPhys;

        // -----------------------------------------------------------------
        // Step 11: Event ring + ERST
        // -----------------------------------------------------------------
        g_evtRing = (TRB*)AllocateDmaBuffer(g_evtRingPhys);
        g_erst = (ERSTEntry*)AllocateDmaBuffer(g_erstPhys);

        // Set up ERST entry 0
        g_erst[0].RingSegmentBase = g_evtRingPhys;
        g_erst[0].RingSegmentSize = EVT_RING_SIZE;
        g_erst[0].Reserved = 0;

        // Write interrupter 0 registers
        // Order: ERSTSZ → ERDP → ERSTBA (ERSTBA triggers hardware read of ERST)
        WriteRt(IR0_ERSTSZ, 1);

        WriteRt(IR0_ERDP, (uint32_t)(g_evtRingPhys & 0xFFFFFFFF));
        WriteRt(IR0_ERDP + 4, (uint32_t)(g_evtRingPhys >> 32));

        // Write ERSTBA last (triggers hardware to read the ERST)
        WriteRt(IR0_ERSTBA, (uint32_t)(g_erstPhys & 0xFFFFFFFF));
        WriteRt(IR0_ERSTBA + 4, (uint32_t)(g_erstPhys >> 32));

        g_evtRingCCS = true;
        g_evtRingDequeue = 0;

        KernelLogStream(OK, "xHCI") << "Event ring at phys " << base::hex << g_evtRingPhys;

        // -----------------------------------------------------------------
        // Step 12: MSI setup
        // -----------------------------------------------------------------
        if (!SetupMsi(foundDev->Bus, foundDev->Device, foundDev->Function)) {
            KernelLogStream(WARNING, "xHCI") << "MSI not available, using poll mode";
        }

        // -----------------------------------------------------------------
        // Step 13: Enable interrupter 0
        // -----------------------------------------------------------------
        WriteRt(IR0_IMAN, IMAN_IE);
        WriteRt(IR0_IMOD, 0); // No moderation

        // -----------------------------------------------------------------
        // Step 14: Start controller
        // -----------------------------------------------------------------
        WriteOp(OP_USBCMD, USBCMD_RS | USBCMD_INTE | USBCMD_HSEE);

        // Wait for controller to start (HCH should clear)
        for (uint32_t i = 0; i < 100000; i++) {
            if (!(ReadOp(OP_USBSTS) & USBSTS_HCH)) {
                break;
            }
            for (int j = 0; j < 10; j++) {
                asm volatile("" ::: "memory");
            }
        }

        KernelLogStream(OK, "xHCI") << "Controller started";

        g_initialized = true;

        // -----------------------------------------------------------------
        // Step 14.5: Power on all ports
        // -----------------------------------------------------------------
        for (uint32_t port = 0; port < g_maxPorts; port++) {
            uint32_t portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
            if (!(portsc & PORTSC_PP)) {
                WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE, PORTSC_PP);
            }
        }
        // Wait for port power to stabilize (~20ms)
        BusyWaitMs(20);
        KernelLogStream(OK, "xHCI") << "All ports powered";

        // -----------------------------------------------------------------
        // Step 15: Port scanning
        // -----------------------------------------------------------------
        for (uint32_t port = 0; port < g_maxPorts; port++) {
            uint32_t portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);

            // Check if device is connected (CCS)
            if (!(portsc & PORTSC_CCS)) {
                continue;
            }

            KernelLogStream(INFO, "xHCI") << "Port " << base::dec << (uint64_t)(port + 1)
                << ": device connected, PORTSC=" << base::hex << (uint64_t)portsc;

            // Reset the port: preserve power, clear change bits, set port reset
            WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE,
                    (portsc & PORTSC_PRESERVE) | PORTSC_PR | PORTSC_CHANGE_BITS);

            // Wait for Port Reset Change (PRC) to be set
            bool resetDone = false;
            for (uint32_t i = 0; i < 100000; i++) {
                PollEvents();
                uint32_t ps = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
                if (ps & PORTSC_PRC) {
                    resetDone = true;
                    break;
                }
                for (int j = 0; j < 100; j++) {
                    asm volatile("" ::: "memory");
                }
            }

            if (!resetDone) {
                KernelLogStream(WARNING, "xHCI") << "Port " << base::dec << (uint64_t)(port + 1) << " reset timeout";
                continue;
            }

            // Re-read PORTSC after reset
            portsc = ReadOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE);
            uint32_t speed = (portsc >> 10) & 0xF;

            // Clear change bits
            WriteOp(OP_PORTSC_BASE + port * OP_PORTSC_STRIDE,
                    (portsc & PORTSC_PRESERVE) | PORTSC_CHANGE_BITS);

            const char* speedStr = "Unknown";
            switch (speed) {
                case SPEED_FULL:  speedStr = "Full (12 Mbps)"; break;
                case SPEED_LOW:   speedStr = "Low (1.5 Mbps)"; break;
                case SPEED_HIGH:  speedStr = "High (480 Mbps)"; break;
                case SPEED_SUPER: speedStr = "Super (5 Gbps)"; break;
            }

            KernelLogStream(OK, "xHCI") << "Port " << base::dec << (uint64_t)(port + 1)
                << ": reset complete, speed=" << speedStr;

            // Post-reset recovery delay (USB spec requires >= 10ms)
            BusyWaitMs(10);

            // Enumerate the device (port IDs are 1-based)
            UsbDevice::EnumerateDevice(port + 1, speed);
        }

        g_bootScanComplete = true;
        KernelLogStream(OK, "xHCI") << "Initialization complete";
    }

};
