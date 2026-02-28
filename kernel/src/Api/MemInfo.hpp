/*
    * MemInfo.hpp
    * SYS_MEMSTATS syscall
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Memory/PageFrameAllocator.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static void Sys_MemStats(MemStats* out) {
        if (out == nullptr) return;
        Memory::g_pfa->GetStats(out);
    }
};
