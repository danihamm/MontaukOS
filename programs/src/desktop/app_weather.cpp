/*
 * app_weather.cpp
 * ZenithOS Desktop - Weather app launcher
 * Spawns weather.elf as a standalone Window Server process
 * Copyright (c) 2026 Daniel Hammer
 */

#include "apps_common.hpp"

void open_weather(DesktopState* ds) {
    (void)ds;
    zenith::spawn("0:/os/weather.elf");
}
