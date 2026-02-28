/*
    * Power.hpp
    * SYS_RESET, SYS_SHUTDOWN syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Efi/UEFI.hpp>
#include <Memory/Paging.hpp>

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
        if (Efi::g_ResetSystem) {
            /* Switch to kernel PML4 which has identity-mapped UEFI runtime regions */
            Memory::VMM::LoadCR3(Memory::VMM::g_paging->PML4);
            Efi::g_ResetSystem(Efi::EfiResetShutdown, 0, 0, nullptr);
        }

        /* No fallback for shutdown; halt the CPU */
        asm volatile("cli; hlt");
        __builtin_unreachable();
    }
};
