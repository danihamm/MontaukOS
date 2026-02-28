/*
    * Keyboard.hpp
    * SYS_ISKEYAVAILABLE, SYS_GETKEY, SYS_GETCHAR syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>
#include <Drivers/PS2/Keyboard.hpp>

#include "Common.hpp"

namespace Montauk {
    static bool Sys_IsKeyAvailable() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target) return target->keyHead != target->keyTail;
        }
        return Drivers::PS2::Keyboard::IsKeyAvailable();
    }

    static void Sys_GetKey(KeyEvent* outEvent) {
        if (outEvent == nullptr) return;
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target) {
                // Wait for key in target's keyBuf ring
                while (target->keyHead == target->keyTail) {
                    Sched::Schedule();
                }
                *outEvent = target->keyBuf[target->keyTail];
                target->keyTail = (target->keyTail + 1) % 64;
                return;
            }
        }
        auto k = Drivers::PS2::Keyboard::GetKey();
        outEvent->scancode = k.Scancode;
        outEvent->ascii    = k.Ascii;
        outEvent->pressed  = k.Pressed;
        outEvent->shift    = k.Shift;
        outEvent->ctrl     = k.Ctrl;
        outEvent->alt      = k.Alt;
    }

    static char Sys_GetChar() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            auto* target = GetRedirTarget(proc);
            if (target && target->inBuf) {
                // Wait for data in target's inBuf ring
                while (target->inTail == target->inHead) {
                    Sched::Schedule(); // yield until parent writes
                }
                uint8_t c = target->inBuf[target->inTail];
                target->inTail = (target->inTail + 1) % Sched::Process::IoBufSize;
                return (char)c;
            }
        }
        return Drivers::PS2::Keyboard::GetChar();
    }
};