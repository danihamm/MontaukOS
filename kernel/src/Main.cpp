/*
    * main.cpp
    * Kernel entry point
    * Copyright (c) 2025 Daniel Hammer, Limine Contributors (via Limine C++ example)
*/

#include <cstdint>
#include <cstddef>
#include <limine.h>
#include <Hal/GDT.hpp>
#include <Terminal/Terminal.hpp>
#include <Efi/UEFI.hpp>
#include <Common/Panic.hpp>
#include <Memory/Memmap.hpp>
#include <Memory/Heap.hpp>
#include <Memory/HHDM.hpp>
#include <Platform/Limine.hpp>
#include <Platform/Util.hpp>
#include <Hal/IDT.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/Paging.hpp>
#include <ACPI/ACPI.hpp>
#include <ACPI/AcpiShutdown.hpp>
#include <ACPI/AcpiEvents.hpp>
#include <Hal/Apic/ApicInit.hpp>
#include <Pci/Pci.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Drivers/PS2/PS2Controller.hpp>
#include <Drivers/PS2/Keyboard.hpp>
#include <Drivers/PS2/Mouse.hpp>
#include <Drivers/Init.hpp>
#include <Graphics/Cursor.hpp>
#include <Hal/MSR.hpp>
#include <Hal/Cpu.hpp>
#include <Fs/Boot.hpp>
#include <Sched/Scheduler.hpp>
#include <Ipc/Ipc.hpp>
#include <Api/Syscall.hpp>
#include <Hal/SmpBoot.hpp>
using namespace Kt;

namespace Memory {
    HeapAllocator* g_heap;
    PageFrameAllocator* g_pfa;
    uint64_t HHDMBase;
};

KernelOutStream kout{};
KernelErrorStream kerr{};

// Extern declarations for global constructors array.
extern void (*__init_array[])();
extern void (*__init_array_end[])();

extern "C" uint64_t KernelStartSymbol;
extern "C" uint64_t KernelEndSymbol;

extern "C" void kmain() {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        Hal::Halt();
    }

    // Call global constructors.
    for (std::size_t i = 0; &__init_array[i] != __init_array_end; i++) {
        __init_array[i]();
    }

    if (framebuffer_request.response == nullptr
     || framebuffer_request.response->framebuffer_count < 1) {
        Hal::Halt();
    }
    
    limine_framebuffer *framebuffer{framebuffer_request.response->framebuffers[0]};

    Kt::Initialize(
        (uint32_t*)framebuffer->address,
        framebuffer->width,
        framebuffer->height,
        framebuffer->pitch,
        framebuffer->red_mask_size,
        framebuffer->red_mask_shift,
        framebuffer->green_mask_size,
        framebuffer->green_mask_shift,
        framebuffer->blue_mask_size,
        framebuffer->blue_mask_shift
    );


#if defined (__x86_64__)
    Hal::PrepareGDT();
    Hal::BridgeLoadGDT();

    Hal::EnableSSE();
#endif

    uint64_t hhdm_offset = hhdm_request.response->offset;
    Memory::HHDMBase = hhdm_offset;

    if (memmap_request.response == nullptr) {
        Panic("System memory map missing!", nullptr);
    }

    Kt::KernelLogStream(OK, "Mem") << "Creating PageFrameAllocator";
    Memory::PageFrameAllocator pmm(Memory::Scan(memmap_request.response));
    Memory::g_pfa = &pmm;

    Kt::KernelLogStream(OK, "Mem") << "Creating HeapAllocator";
    Memory::HeapAllocator heap{};
    Memory::g_heap = &heap;

    heap.Walk();


#if defined (__x86_64__)
    Hal::IDTInitialize();

    Memory::VMM::Paging g_paging{};
    Memory::VMM::g_paging = &g_paging;
    g_paging.Init((uint64_t)&KernelStartSymbol, ((uint64_t)&KernelEndSymbol - (uint64_t)&KernelStartSymbol), memmap_request.response);

    // Reprogram PAT so entry 1 = Write-Combining (default is Write-Through).
    // Must be done after paging init and before any WC mappings.
    Hal::InitializePAT();
    Kt::KernelLogStream(OK, "Hal") << "PAT reprogrammed (entry 1 = WC)";

#endif

    // Initialize Cursor early so we can WC-map the framebuffer before
    // the bulk of boot logging begins (ACPI, PCI, drivers, etc.)
    Graphics::Cursor::Initialize(framebuffer);

#if defined (__x86_64__)
    // Map framebuffer as Write-Combining immediately for faster screen writes.
    // All subsequent log output benefits from WC burst transfers.
    Graphics::Cursor::MapWriteCombining();
#endif

    Hal::ACPI g_acpi((Hal::ACPI::XSDP*)Memory::HHDM(rsdp_request.response->address));

#if defined (__x86_64__)
    if (g_acpi.GetXSDT() != nullptr) {
        Hal::AcpiShutdown::Initialize(g_acpi.GetXSDT());

        Hal::ApicInitialize(g_acpi.GetXSDT());

        // Set up BSP per-CPU data (GS base) before enabling interrupts.
        // ISR stubs use SWAPGS which requires GS base to point to CpuData.
        Smp::InitBsp();

        // Now safe to enable interrupts (SWAPGS-aware ISR stubs are installed)
        asm volatile("sti");

        // Initialize ACPI events (SCI, power button) after APIC is ready
        Hal::AcpiEvents::Initialize(g_acpi.GetXSDT());

        Pci::Initialize(g_acpi.GetXSDT());

        Drivers::ProbeEarly();
        Drivers::InitializeGraphics();

        Timekeeping::ApicTimerInitialize();

        Drivers::PS2::Initialize();
        Drivers::PS2::Keyboard::Initialize();
        Drivers::PS2::Mouse::Initialize();

        Drivers::ProbeNormal();
        Drivers::InitializeNetwork();
        Drivers::InitializeStorage();
        Drivers::InitializeAudio();
    }
#endif

    Efi::SystemTable* ST = (Efi::SystemTable*)Memory::HHDM(system_table_request.response->address);
    Efi::Init(ST, efi_memmap_request.response);

    Fs::InitializeBootFilesystems(module_request.response);

    Hal::LoadTSS();
    Montauk::InitializeSyscalls();

    Sched::Initialize();
    Ipc::Initialize();

    // Boot Application Processors (all subsystems ready, APs can schedule)
    Smp::BootAPs();

    // Flush any stale PS/2 mouse bytes that accumulated during boot
    // (edge-triggered IRQs can be lost while spinlocks disable interrupts)
    Drivers::PS2::Mouse::FlushState();

    Kt::SuppressKernelLog();
    Sched::Spawn("0:/os/init.elf");

    // Enable preemptive scheduling via the APIC timer
    Timekeeping::EnableSchedulerTick();

    // Main loop: idle until next interrupt.
    // Use MWAIT for deeper C-states if available, otherwise HLT.
    auto* bspCpu = Smp::GetCpuData(0);
    if (bspCpu && bspCpu->hasMwait) {
        static volatile uint64_t s_bspIdleMonitor = 0;
        for (;;) {
            Hal::IdleWait(&s_bspIdleMonitor);
        }
    } else {
        for (;;) {
            asm volatile("hlt");
        }
    }
}
