/*
    * app_wiki.cpp
    * MontaukOS Desktop - Wikipedia launcher
    * Spawns wikipedia.elf as a standalone Window Server process
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

void open_wiki(DesktopState* ds) {
    (void)ds;
    montauk::spawn("0:/os/wikipedia.elf");
}
