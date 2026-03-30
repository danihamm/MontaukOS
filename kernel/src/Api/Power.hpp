/*
    * Power.hpp
    * SYS_RESET, SYS_SHUTDOWN, SYS_SUSPEND syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Efi/UEFI.hpp>
#include <Memory/Paging.hpp>
#include <Io/IoPort.hpp>
#include <Graphics/Cursor.hpp>
#include <ACPI/AcpiShutdown.hpp>
#include <ACPI/AcpiSleep.hpp>

namespace Montauk {

    static void Sys_Reset() {
        if (Efi::g_ResetSystem) {
            /* Switch to kernel PML4 which has identity-mapped UEFI runtime regions */
            Memory::VMM::LoadCR3(Memory::VMM::g_paging->PML4);
            Efi::g_ResetSystem(Efi::EfiResetCold, 0, 0, nullptr);
        }

        /* Fallback: triple fault via null IDT */
        struct [[gnu::packed]] { uint16_t limit; uint64_t base; } nullIdt = {0, 0};
        asm volatile("lidt %0; int $0x03" :: "m"(nullIdt));
        __builtin_unreachable();
    }

    static void Sys_Shutdown() {
        /* Primary: ACPI S5 shutdown via PM1 control registers */
        if (Hal::AcpiShutdown::IsAvailable()) {
            Hal::AcpiShutdown::Shutdown();
        }

        /* Fallback: EFI ResetSystem */
        if (Efi::g_ResetSystem) {
            Memory::VMM::LoadCR3(Memory::VMM::g_paging->PML4);
            Efi::g_ResetSystem(Efi::EfiResetShutdown, 0, 0, nullptr);
        }

        /* Last resort: halt the CPU */
        asm volatile("cli; hlt");
        __builtin_unreachable();
    }

    static int64_t Sys_Suspend() {
        if (!Hal::AcpiSleep::IsS3Available()) {
            return -1; // S3 not supported
        }
        return (int64_t)Hal::AcpiSleep::Suspend();
    }
};
