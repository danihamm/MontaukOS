/*
    * Time.hpp
    * SYS_GETTICKS, SYS_GETMILLISECONDS, SYS_GETTIME syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Timekeeping/ApicTimer.hpp>
#include <Timekeeping/Time.hpp>

#include "Syscall.hpp"

namespace Montauk {
    static uint64_t Sys_GetTicks() {
        return Timekeeping::GetTicks();
    }

    static uint64_t Sys_GetMilliseconds() {
        return Timekeeping::GetMilliseconds();
    }

    static void Sys_GetTime(DateTime* out) {
        if (out == nullptr) return;
        Timekeeping::DateTime dt = Timekeeping::GetDateTime();
        out->Year   = dt.Year;
        out->Month  = dt.Month;
        out->Day    = dt.Day;
        out->Hour   = dt.Hour;
        out->Minute = dt.Minute;
        out->Second = dt.Second;
    }
};