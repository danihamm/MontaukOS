/*
    * printdialog.hpp
    * Print dialog state and declarations
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <gui/gui.hpp>
#include <gui/dialogs.hpp>

struct PrintDialogLayout {
    gui::Rect header_rect;
    gui::Rect printers_rect;
    gui::Rect detail_rect;
    gui::Rect copy_minus_btn;
    gui::Rect copy_value_rect;
    gui::Rect copy_plus_btn;
    gui::Rect cancel_btn;
    gui::Rect printers_btn;
    gui::Rect refresh_btn;
    gui::Rect confirm_btn;
};

namespace printdialog {

constexpr int PRINT_INIT_W = 640;
constexpr int PRINT_INIT_H = 400;

constexpr int MAX_DISCOVERED_PRINTERS = 8;
constexpr int BUTTON_H = 30;
constexpr int BUTTON_W = 88;

void init(const gui::dialogs::Request& req);
void draw();
void handle_mouse(const Montauk::WinEvent& ev);
void handle_key(const Montauk::KeyEvent& key);
void cleanup();

} // namespace printdialog