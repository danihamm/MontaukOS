/*
    * filedialog.hpp
    * File open/save dialog state and declarations
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <gui/gui.hpp>
#include <gui/dialogs.hpp>

struct FileDialogLayout {
    gui::Rect back_btn;
    gui::Rect forward_btn;
    gui::Rect up_btn;
    gui::Rect home_btn;
    gui::Rect view_btn;
    gui::Rect path_rect;
    gui::Rect content_rect;
    gui::Rect list_rect;
    gui::Rect footer_rect;
    gui::Rect name_rect;
    gui::Rect cancel_btn;
    gui::Rect confirm_btn;
};

namespace filedialog {

constexpr int FILE_INIT_W = 720;
constexpr int FILE_INIT_H = 520;

constexpr int TOOLBAR_H = 32;
constexpr int PATHBAR_H = 32;
constexpr int FOOTER_H_OPEN = 64;
constexpr int FOOTER_H_SAVE = 74;
constexpr int HEADER_H = 20;
constexpr int ITEM_H = 24;
constexpr int SCROLLBAR_W = 12;
constexpr int GRID_CELL_W = 80;
constexpr int GRID_CELL_H = 80;
constexpr int GRID_ICON = 48;
constexpr int GRID_PAD = 4;

constexpr int MAX_ENTRIES = 64;
constexpr int MAX_HISTORY = 16;
constexpr int MAX_DRIVES = 16;
constexpr int BUTTON_H = 30;
constexpr int BUTTON_W = 88;

void init(const gui::dialogs::Request& req);
void draw();
void handle_mouse(const Montauk::WinEvent& ev);
void handle_key(const Montauk::KeyEvent& key);
void cleanup();

} // namespace filedialog