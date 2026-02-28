/*
    * app_doom.cpp
    * MontaukOS Desktop - DOOM launcher (spawns standalone doom.elf)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

void open_doom(DesktopState* ds) {
    (void)ds;
    montauk::spawn("0:/games/doom.elf");
}
