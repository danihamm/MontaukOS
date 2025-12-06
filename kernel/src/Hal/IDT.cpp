/*
    * IDT.cpp
    * Intel Interrupt Descriptor Table implementation
    * Copyright (c) 2025 Daniel Hammer
*/

#include "IDT.hpp"

#include <Memory/Heap.hpp>
#include <Common/Panic.hpp>
#include <Platform/Registers.hpp>
#include <CppLib/Stream.hpp>
#include <Memory/PageFrameAllocator.hpp>

namespace Hal {
    constexpr auto InterruptGate = 0x8E;
    constexpr auto TrapGate = 0x8F;

    InterruptDescriptor* IDT;
    IDTRStruct IDTR{};

    const char* ExceptionStrings[] = {
        "Division Error",
        "Debug",
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "Bound Rage Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 Floating-Point Exception",
        "Alignment Check",
        "Machine Check",
        "SMID Floating-Point Exception",
        "Virtualization Exception",
        "Control Protection Exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor Injection Exception",
        "VMM Communication Exception",
        "Security Exception",
        "Reserved"
    };

    template<size_t i>
    __attribute__((interrupt)) void ExceptionHandler(System::PanicFrame* frame)
    {
        frame->InterruptVector = i;
        Panic(ExceptionStrings[i], frame);
    }

    void LoadIDT(IDTRStruct& idtr) {
        asm("lidt %0" : : "m"(idtr));
    }

    InterruptDescriptor* GetInterruptDescriptor(size_t index) {
        InterruptDescriptor* descriptor = (InterruptDescriptor*)(IDTR.Base + index * sizeof(InterruptDescriptor));
        return descriptor;
    }

    uint64_t GetHandlerAddress(InterruptDescriptor* descriptor) {
        uint64_t result{};

        result |= (uint64_t)descriptor->Offset1;
        result |= (uint64_t)descriptor->Offset2 << 16;
        result |= (uint64_t)descriptor->Offset3 << 32;

        return result;
    }

    void IDTEncodeInterrupt(size_t i, void* handler, uint8_t type_attr) {
        uint64_t offset = (uint64_t)handler;
        auto ptr = GetInterruptDescriptor(i);

        *ptr = InterruptDescriptor {
            .Offset1 = (uint16_t)(offset & 0x000000000000ffff),

            .Selector = 0x08,
            .IST = 0x00,
            .TypeAttributes = type_attr,

            .Offset2 = (uint16_t)((offset & 0x00000000ffff0000) >> 16),
            .Offset3 = (uint32_t)((offset & 0xffffffff00000000) >> 32),

            .Zero = 0x00
        };
    }

    template<int I, int N>
    struct SetHandler {
        static void run() {
            IDTEncodeInterrupt(I, (void*)ExceptionHandler<I>, TrapGate);
            SetHandler<I+1,N>::run();
        }
    };

    template<int N>
    struct SetHandler<N,N> {static void run() {}};

    void IDTInitialize() {
        IDT = (InterruptDescriptor*)Memory::g_pfa->Allocate();
        Kt::KernelLogStream(Kt::DEBUG, "IDT") << "Allocated IDT at " << base::hex << (uint64_t)IDT;
        IDTR.Limit = 0x0FF;
        IDTR.Base = (uint64_t)IDT;
        Kt::KernelLogStream(Kt::DEBUG, "IDT") << "Set IDTR Base to " << base::hex << IDTR.Base << " and Limit to " << base::hex << IDTR.Limit;

        SetHandler<0, 31>::run();

        Kt::KernelLogStream(Kt::OK, "Hal") << "Created exception interrupt vectors";

        LoadIDT(IDTR);

        Kt::KernelLogStream(Kt::OK, "Hal") << "Loaded new IDT";
    }
};