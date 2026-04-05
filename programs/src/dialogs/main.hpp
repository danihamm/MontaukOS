/*
    * main.hpp
    * Shared globals for dialogs app
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <gui/standalone.hpp>
#include <gui/dialogs.hpp>
#include <cstdint>

struct AppState {
    gui::WsWindow win;
    gui::dialogs::Request request;
    gui::dialogs::Result result;
    bool running;
    bool is_print;
};

extern AppState g_app;

void dialog_finish(const char* status, const char* path, const char* job_id, const char* message,
                   const char* printer_uri = "", const char* printer_name = "", uint32_t copies = 0);
void dialog_cancel(const char* message = "");