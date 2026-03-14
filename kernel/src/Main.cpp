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
#include <Fs/Ramdisk.hpp>
#include <Fs/Vfs.hpp>
#include <Fs/Fat32.hpp>
#include <Fs/Ext2.hpp>
#include <Fs/FsProbe.hpp>
#include <Sched/Scheduler.hpp>
#include <Api/Syscall.hpp>
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

    // Initialize ramdisk from Limine modules
    bool hasRamdisk = false;
    if (module_request.response != nullptr && module_request.response->module_count > 0) {
        Kt::KernelLogStream(OK, "Modules") << "Found " << (uint64_t)module_request.response->module_count << " module(s)";
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            limine_file* mod = module_request.response->modules[i];
            const char* modString = mod->string;

            // Find "ramdisk" module by its string
            if (modString != nullptr &&
                modString[0] == 'r' && modString[1] == 'a' && modString[2] == 'm' &&
                modString[3] == 'd' && modString[4] == 'i' && modString[5] == 's' &&
                modString[6] == 'k' && modString[7] == '\0') {
                Kt::KernelLogStream(OK, "Modules") << "Ramdisk module at " << kcp::hex << (uint64_t)mod->address << kcp::dec << ", size=" << mod->size;
                Fs::Ramdisk::Initialize(mod->address, mod->size);
                hasRamdisk = true;
            }
        }
    } else {
        Kt::KernelLogStream(WARNING, "Modules") << "No modules loaded (ramdisk unavailable)";
    }

    // Initialize VFS and register ramdisk as drive 0 only if present
    Fs::Vfs::Initialize();

    if (hasRamdisk) {
        static Fs::Vfs::FsDriver ramdiskDriver = {
            Fs::Ramdisk::Open,
            Fs::Ramdisk::Read,
            Fs::Ramdisk::GetSize,
            Fs::Ramdisk::Close,
            Fs::Ramdisk::ReadDir,
            Fs::Ramdisk::Write,
            Fs::Ramdisk::Create,
            Fs::Ramdisk::Delete,
            Fs::Ramdisk::Mkdir
        };
        Fs::Vfs::RegisterDrive(0, &ramdiskDriver);
    }

    // Register filesystem probes and auto-mount partitions.
    // When no ramdisk, disk partitions start at drive 0 so init.elf is found there.
    Fs::Fat32::RegisterProbe();
    Fs::Ext2::RegisterProbe();
    Fs::FsProbe::MountPartitions(hasRamdisk ? 1 : 0);

    Hal::LoadTSS();
    Montauk::InitializeSyscalls();

    Sched::Initialize();

    Kt::SuppressKernelLog();
    Sched::Spawn("0:/os/init.elf");

    // Enable preemptive scheduling via the APIC timer
    Timekeeping::EnableSchedulerTick();

    // Main loop: halt until next interrupt
    for (;;) {
        asm volatile ("hlt");
    }
}
