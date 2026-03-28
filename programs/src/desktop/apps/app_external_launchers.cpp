/*
    * app_external_launchers.cpp
    * Desktop launch wrappers for standalone Window Server apps
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

static void spawn_app(const char* path, const char* args = nullptr) {
    if (path && path[0]) {
        montauk::spawn(path, args);
    }
}

void open_terminal(DesktopState* ds) {
    const char* home = (ds && ds->home_dir[0]) ? ds->home_dir : nullptr;
    spawn_app("0:/apps/terminal/terminal.elf", home);
}

void open_calculator(DesktopState* ds) {
    (void)ds;
    spawn_app("0:/apps/calculator/calculator.elf");
}

void open_texteditor(DesktopState* ds) {
    (void)ds;
    spawn_app("0:/apps/texteditor/texteditor.elf");
}

void open_klog(DesktopState* ds) {
    (void)ds;
    spawn_app("0:/apps/klog/klog.elf");
}

void open_wordprocessor(DesktopState* ds) {
    (void)ds;
    spawn_app("0:/apps/wordprocessor/wordprocessor.elf");
}
