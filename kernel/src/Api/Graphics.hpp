/*
    * Graphics.hpp
    * SYS_FBINFO, SYS_FBMAP, SYS_TERMSIZE, SYS_TERMSCALE syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Sched/Scheduler.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Graphics/Cursor.hpp>
#include <Terminal/Terminal.hpp>

#include "Syscall.hpp"
#include "Common.hpp"
#include "../Libraries/flanterm/src/flanterm.h"

namespace Montauk {

    static void Sys_FbInfo(FbInfo* out) {
        if (out == nullptr) return;
        out->width    = Graphics::Cursor::GetFramebufferWidth();
        out->height   = Graphics::Cursor::GetFramebufferHeight();
        out->pitch    = Graphics::Cursor::GetFramebufferPitch();
        out->bpp      = 32;
        out->userAddr = 0;
    }

    static uint64_t Sys_FbMap() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;

        uint32_t* fbBase = Graphics::Cursor::GetFramebufferBase();
        if (fbBase == nullptr) return 0;

        uint64_t fbPhys = Memory::SubHHDM((uint64_t)fbBase);
        uint64_t fbSize = Graphics::Cursor::GetFramebufferHeight()
                        * Graphics::Cursor::GetFramebufferPitch();
        uint64_t numPages = (fbSize + 0xFFF) / 0x1000;

        Kt::KernelLogStream(Kt::INFO, "FbMap") << "fbPhys=" << kcp::hex << fbPhys
            << " size=" << kcp::dec << fbSize
            << " pages=" << numPages
            << " (" << Graphics::Cursor::GetFramebufferWidth()
            << "x" << Graphics::Cursor::GetFramebufferHeight()
            << " pitch=" << Graphics::Cursor::GetFramebufferPitch() << ")";

        // Map at a fixed user VA
        constexpr uint64_t userVa = 0x50000000ULL;

        for (uint64_t i = 0; i < numPages; i++) {
            Memory::VMM::Paging::MapUserInWC(
                proc->pml4Phys,
                fbPhys + i * 0x1000,
                userVa + i * 0x1000
            );
        }

        return userVa;
    }

    static uint64_t Sys_TermSize() {
        // If the process is redirected to a GUI terminal, return those dimensions
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->termCols > 0 && target->termRows > 0) {
                return ((uint64_t)target->termRows << 32) | ((uint64_t)target->termCols & 0xFFFFFFFF);
            }
        }
        size_t cols = 0, rows = 0;
        flanterm_get_dimensions(Kt::ctx, &cols, &rows);
        return (rows << 32) | (cols & 0xFFFFFFFF);
    }

    static int64_t Sys_TermScale(uint64_t scale_x, uint64_t scale_y) {
        if (scale_x == 0) {
            return (int64_t)((Kt::GetFontScaleY() << 32) | (Kt::GetFontScaleX() & 0xFFFFFFFF));
        }
        Kt::Rescale((size_t)scale_x, (size_t)scale_y);
        size_t cols = 0, rows = 0;
        flanterm_get_dimensions(Kt::ctx, &cols, &rows);
        return (int64_t)((rows << 32) | (cols & 0xFFFFFFFF));
    }
};
