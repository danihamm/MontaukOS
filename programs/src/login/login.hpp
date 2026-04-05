/*
    * login.hpp
    * Shared types and helpers for the MontaukOS login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <cstdint>
#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/user.h>
#include <montauk/config.h>
#include <gui/gui.hpp>
#include <gui/framebuffer.hpp>
#include <gui/draw.hpp>
#include <gui/font.hpp>

extern "C" {
    unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                         int* x, int* y, int* channels_in_file,
                                         int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
}

enum LoginMode {
    MODE_FIRST_BOOT,
    MODE_LOGIN,
};

struct FieldEditState {
    int cursor;
    int sel_anchor;
    int sel_end;
    bool has_selection;
};

struct LoginState {
    gui::Framebuffer fb;
    int screen_w, screen_h;

    LoginMode mode;
    int active_field;

    char username[32];
    int username_len;
    char display_name[64];
    int display_name_len;
    char password[64];
    int password_len;
    char confirm[64];
    int confirm_len;

    char error_msg[128];
    bool show_error;

    Montauk::MouseState mouse;
    uint8_t prev_buttons;
    FieldEditState username_edit;
    FieldEditState display_name_edit;
    FieldEditState password_edit;
    FieldEditState confirm_edit;

    uint32_t* bg_wallpaper;
    int bg_wallpaper_w, bg_wallpaper_h;
    bool has_wallpaper;
};

inline constexpr gui::Color BG_COLOR = gui::Color::from_rgb(0x00, 0x00, 0x00);
inline constexpr gui::Color CARD_BG = gui::colors::WINDOW_BG;
inline constexpr gui::Color CARD_BORDER = gui::colors::BORDER;
inline constexpr gui::Color TITLEBAR_BG = gui::colors::TITLEBAR_BG;
inline constexpr gui::Color FOOTER_BG = gui::Color::from_rgb(0xF7, 0xF7, 0xF7);
inline constexpr gui::Color FIELD_BG = gui::colors::WHITE;
inline constexpr gui::Color FIELD_BG_ACTIVE = gui::Color::from_rgb(0xFA, 0xFC, 0xFF);
inline constexpr gui::Color FIELD_BORDER = gui::colors::BORDER;
inline constexpr gui::Color FIELD_HOVER = gui::Color::from_rgb(0xB7, 0xC7, 0xE8);
inline constexpr gui::Color FIELD_ACTIVE = gui::colors::ACCENT;
inline constexpr gui::Color BTN_COLOR = gui::colors::ACCENT;
inline constexpr gui::Color BTN_HOVER = gui::Color::from_rgb(0x2B, 0x6B, 0xE0);
inline constexpr gui::Color BTN_TEXT = gui::colors::WHITE;
inline constexpr gui::Color SECONDARY_BTN_BG = gui::Color::from_rgb(0xE8, 0xE8, 0xE8);
inline constexpr gui::Color SECONDARY_BTN_HOVER = gui::Color::from_rgb(0xDC, 0xDC, 0xDC);
inline constexpr gui::Color SECONDARY_BTN_BORDER = gui::colors::BORDER;
inline constexpr gui::Color TEXT_COLOR = gui::colors::TEXT_COLOR;
inline constexpr gui::Color LABEL_COLOR = gui::Color::from_rgb(0x66, 0x66, 0x66);
inline constexpr gui::Color ERROR_COLOR = gui::Color::from_rgb(0xC0, 0x33, 0x33);
inline constexpr gui::Color FIELD_SEL_BG = gui::Color::from_rgb(0xB0, 0xD0, 0xF0);

inline constexpr int CARD_W = 432;
inline constexpr int TITLEBAR_H = 30;
inline constexpr int FOOTER_H = 58;
inline constexpr int FIELD_H = 34;
inline constexpr int FIELD_PAD = 10;
inline constexpr int BTN_H = 30;
inline constexpr int LABEL_GAP = 4;
inline constexpr int CONTENT_PAD_X = 24;
inline constexpr int CONTENT_TOP_PAD = 18;
inline constexpr int BODY_BOTTOM_PAD = 18;
inline constexpr int FIELD_GAP = 12;
inline constexpr int POWER_BTN_W = 104;
inline constexpr int POWER_BTN_GAP = 8;
inline constexpr int FOOTER_PAD = 16;
struct LoginLayout {
    gui::Rect card;
    gui::Rect titlebar;
    gui::Rect footer;
    gui::Rect username_field;
    gui::Rect display_name_field;
    gui::Rect password_field;
    gui::Rect confirm_field;
    gui::Rect submit_button;
    gui::Rect shutdown_button;
    gui::Rect reboot_button;
    int content_x;
    int content_w;
    int heading_y;
    int subtitle_y;
    int error_y;
    const char* window_title;
    const char* heading;
    const char* subtitle;
    const char* submit_label;
};

struct LoginField {
    char* text;
    int* len;
    int max_len;
    bool is_password;
    FieldEditState* edit;
};

bool load_login_wallpaper(LoginState* ls);

LoginField get_field(LoginState* ls, int field);
void reset_field_edit(FieldEditState* edit);
void clear_field_selection(FieldEditState* edit);
void clear_all_field_selections(LoginState* ls);
void clamp_field_edit(FieldEditState* edit, int len);
void field_sel_range(const FieldEditState* edit, int* out_s, int* out_e);
void start_field_selection(FieldEditState* edit);
void update_field_selection(FieldEditState* edit);
int copy_slice(char* out, int cap, const char* text, int start, int end);
int slice_width(const char* text, int start, int end);
int compute_view_start(const char* text, int len, int max_text_w, int cursor);
int compute_view_end(const char* text, int len, int max_text_w, int view_start);
int field_cursor_from_mouse_x(const char* text, int len, int max_text_w,
                              int anchor_cursor, int rel_x);
void delete_field_range(char* buf, int* len, int start, int count);
void delete_field_selection(char* buf, int* len, FieldEditState* edit);
int password_mask_advance();
int password_slice_width(int start, int end);
int compute_password_view_start(int len, int max_text_w, int cursor);
int compute_password_view_end(int len, int max_text_w, int view_start);
int password_cursor_from_mouse_x(int len, int max_text_w, int anchor_cursor, int rel_x);

LoginLayout layout_login_screen(LoginState* ls);
void draw_login_screen(LoginState* ls);

int max_fields(LoginState* ls);
bool try_first_boot_submit(LoginState* ls);
bool try_login(LoginState* ls);
void handle_key(LoginState* ls, const Montauk::KeyEvent& key);
void handle_mouse(LoginState* ls);
