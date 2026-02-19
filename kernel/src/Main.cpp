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
#include <Libraries/String.hpp>
#include <Efi/UEFI.hpp>
#include <Common/Panic.hpp>
#include <Memory/Memmap.hpp>
#include <Memory/Heap.hpp>
#include <Memory/HHDM.hpp>
#include <CppLib/Stream.hpp>
#include <CppLib/Vector.hpp>
#include <CppLib/CString.hpp>
#include <Platform/Limine.hpp>
#include <Platform/Util.hpp>
#include <Libraries/Memory.hpp>
#include <Hal/IDT.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/HHDM.hpp>
#include <Io/IoPort.hpp>
#include <Memory/Paging.hpp>
#include <ACPI/ACPI.hpp>
#include <Hal/Apic/ApicInit.hpp>
#include <Pci/Pci.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Drivers/PS2/PS2Controller.hpp>
#include <Drivers/PS2/Keyboard.hpp>
#include <Drivers/PS2/Mouse.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Net/Net.hpp>
#include <CppLib/BoxUI.hpp>
#include <Graphics/Cursor.hpp>
#include <Fs/Ramdisk.hpp>
#include <Fs/Vfs.hpp>
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

    // Enable SSE/SSE2 — required for userspace programs compiled with SSE
    // CR0: clear EM (bit 2), set MP (bit 1)
    {
        uint64_t cr0;
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2);  // Clear EM
        cr0 |=  (1ULL << 1);  // Set MP
        asm volatile("mov %0, %%cr0" :: "r"(cr0));

        // CR4: set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);   // OSFXSR
        cr4 |= (1ULL << 10);  // OSXMMEXCPT
        asm volatile("mov %0, %%cr4" :: "r"(cr4));
    }
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

#endif
    Hal::ACPI g_acpi((Hal::ACPI::XSDP*)Memory::HHDM(rsdp_request.response->address));

#if defined (__x86_64__)
    if (g_acpi.GetXSDT() != nullptr) {
        Hal::ApicInitialize(g_acpi.GetXSDT());

        Pci::Initialize(g_acpi.GetXSDT());

        Timekeeping::ApicTimerInitialize();

        Drivers::PS2::Initialize();
        Drivers::PS2::Keyboard::Initialize();
        Drivers::PS2::Mouse::Initialize();

        Drivers::Net::E1000::Initialize();
        if (!Drivers::Net::E1000::IsInitialized()) {
            KernelLogStream(INFO, "Init") << "E1000 not found, trying E1000E...";
            Drivers::Net::E1000E::Initialize();
        }
        Net::Initialize();
    }
#endif

    Efi::SystemTable* ST = (Efi::SystemTable*)Memory::HHDM(system_table_request.response->address);
    Efi::Init(ST);

    // Initialize ramdisk from Limine modules
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
            }
        }
    } else {
        Kt::KernelLogStream(WARNING, "Modules") << "No modules loaded (ramdisk unavailable)";
    }

    // Initialize VFS and register ramdisk as drive 0
    Fs::Vfs::Initialize();

    static Fs::Vfs::FsDriver ramdiskDriver = {
        Fs::Ramdisk::Open,
        Fs::Ramdisk::Read,
        Fs::Ramdisk::GetSize,
        Fs::Ramdisk::Close,
        Fs::Ramdisk::ReadDir,
        Fs::Ramdisk::Write,
        Fs::Ramdisk::Create
    };
    Fs::Vfs::RegisterDrive(0, &ramdiskDriver);

    Graphics::Cursor::Initialize(framebuffer);

    Hal::LoadTSS();
    Zenith::InitializeSyscalls();

    Sched::Initialize();
    Sched::Spawn("0:/os/init.elf");

    // Enable preemptive scheduling via the APIC timer
    Timekeeping::EnableSchedulerTick();

    // Main loop: halt until next interrupt
    for (;;) {
        asm volatile ("hlt");
    }
}
