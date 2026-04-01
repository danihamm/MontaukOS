/*
    * Keyboard.hpp
    * SYS_ISKEYAVAILABLE, SYS_GETKEY, SYS_GETCHAR syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>
#include <Drivers/PS2/Keyboard.hpp>
#include <Libraries/Memory.hpp>

#include "Common.hpp"

namespace Montauk {
    static bool Sys_IsKeyAvailable() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            Ipc::Mailbox* mailbox = GetRedirKeyMailbox(proc);
            if (mailbox != nullptr) return Ipc::MailboxHasMessage(mailbox);
        }
        return Drivers::PS2::Keyboard::IsKeyAvailable();
    }

    static void Sys_GetKey(KeyEvent* outEvent) {
        if (outEvent == nullptr) return;
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc && proc->redirected) {
            Ipc::Mailbox* mailbox = GetRedirKeyMailbox(proc);
            if (mailbox != nullptr) {
                for (;;) {
                    uint16_t len = sizeof(KeyEvent);
                    int rc = Ipc::MailboxRecv(mailbox, nullptr, outEvent, &len, true);
                    if (rc > 0) return;
                    if (rc < 0) {
                        memset(outEvent, 0, sizeof(KeyEvent));
                        return;
                    }
                    Sched::BlockOnObject(mailbox, 0);
                }
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
            Ipc::Stream* input = GetRedirInStream(proc);
            Ipc::Mailbox* mailbox = GetRedirKeyMailbox(proc);
            if (input != nullptr || mailbox != nullptr) {
                for (;;) {
                    if (input != nullptr) {
                        uint8_t c = 0;
                        int rc = Ipc::StreamRead(input, &c, 1, true);
                        if (rc > 0) return (char)c;
                        if (rc < 0 && mailbox == nullptr) return 0;
                    }

                    if (mailbox != nullptr) {
                        KeyEvent ev{};
                        uint16_t len = sizeof(ev);
                        int rc = Ipc::MailboxRecv(mailbox, nullptr, &ev, &len, true);
                        if (rc > 0) {
                            if (ev.pressed && ev.ascii != 0) return ev.ascii;
                            continue;
                        }
                        if (rc < 0 && input == nullptr) return 0;
                    }

                    if (proc->ioWaitsetHandle >= 0) {
                        Ipc::WaitsetReady ready{};
                        if (Ipc::WaitsetWaitHandle(proc->ioWaitsetHandle, &ready, ~0ULL) < 0) {
                            return 0;
                        }
                    } else if (input != nullptr) {
                        Sched::BlockOnObject(input, 0);
                    } else if (mailbox != nullptr) {
                        Sched::BlockOnObject(mailbox, 0);
                    } else {
                        return 0;
                    }
                }
            }
        }
        return Drivers::PS2::Keyboard::GetChar();
    }
};
