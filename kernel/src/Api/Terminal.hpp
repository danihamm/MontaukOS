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
            Ipc::Stream* stream = GetRedirOutStream(proc);
            if (stream != nullptr) {
                int len = 0;
                while (text[len]) len++;
                if (len > 0) {
                    WriteAllToStream(stream, (const uint8_t*)text, len);
                }
                return;
            }
        }
        // Don't draw over the framebuffer once the GUI is active
        if (Kt::g_suppressKernelLog) return;
        Kt::Print(text);
    }

    static void Sys_Putchar(char c) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            Ipc::Stream* stream = GetRedirOutStream(proc);
            if (stream != nullptr) {
                uint8_t byte = (uint8_t)c;
                WriteAllToStream(stream, &byte, 1);
                return;
            }
        }
        // Don't draw over the framebuffer once the GUI is active
        if (Kt::g_suppressKernelLog) return;
        Kt::Putchar(c);
    }
};
