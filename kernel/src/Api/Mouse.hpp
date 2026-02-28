/*
    * Mouse.hpp
    * SYS_MOUSESTATE, SYS_SETMOUSEBOUNDS syscalls
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Drivers/PS2/Mouse.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static void Sys_MouseState(MouseState* out) {
        if (out == nullptr) return;
        auto state = Drivers::PS2::Mouse::GetMouseState();
        out->x = state.X;
        out->y = state.Y;
        out->scrollDelta = state.ScrollDelta;
        out->buttons = state.Buttons;
    }

    static void Sys_SetMouseBounds(int32_t maxX, int32_t maxY) {
        Drivers::PS2::Mouse::SetBounds(maxX, maxY);
    }
};
