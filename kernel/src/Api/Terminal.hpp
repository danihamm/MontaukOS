/*
    * Terminal.hpp
    * SYS_PRINT, SYS_PUTCHAR syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>
#include <Terminal/Terminal.hpp>

#include "Common.hpp"

namespace Montauk {

    static void Sys_Print(const char* text) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->outBuf) {
                for (int i = 0; text[i]; i++) {
                    RingWrite(target->outBuf, target->outHead, target->outTail, Sched::Process::IoBufSize, (uint8_t)text[i]);
                }
                return;
            }
        }
        Kt::Print(text);
    }

    static void Sys_Putchar(char c) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->outBuf) {
                RingWrite(target->outBuf, target->outHead, target->outTail, Sched::Process::IoBufSize, (uint8_t)c);
                return;
            }
        }
        Kt::Putchar(c);
    }
};