/*
    * main.cpp
    * MontaukOS graphical login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#include "login.hpp"

inline void* operator new(unsigned long, void* p) { return p; }

static void maybe_run_setup_session() {
    auto doc = montauk::config::load("setup");
    const char* mode = doc.get_string("environment.mode", "");
    if (!montauk::streq(mode, "setup")) {
        doc.destroy();
        return;
    }

    const char* user = doc.get_string("session.username", "liveuser");
    const char* display = doc.get_string("session.display_name", "Live User");
    const char* role = doc.get_string("session.role", "admin");

    montauk::fmkdir("0:/users");
    montauk::user::create_user(user, display, "", role);
    montauk::user::set_session(user);
    doc.destroy();

    int pid = montauk::spawn("0:/os/desktop.elf", user);
    if (pid >= 0) {
        montauk::setuser(pid, user);
        montauk::waitpid(pid);
    }

    montauk::user::clear_session();
}

static void initialize_login_mode(LoginState* ls) {
    int fh = montauk::open("0:/config/users.toml");
    if (fh < 0) {
        ls->mode = MODE_FIRST_BOOT;
        ls->active_field = 0;
        montauk::strcpy(ls->username, "admin");
        ls->username_len = 5;
        ls->username_edit.cursor = ls->username_len;
        clear_field_selection(&ls->username_edit);
        return;
    }

    montauk::close(fh);
    ls->mode = MODE_LOGIN;
    ls->active_field = 0;
}

extern "C" void _start() {
    LoginState* ls = (LoginState*)montauk::malloc(sizeof(LoginState));
    montauk::memset(ls, 0, sizeof(LoginState));

    new (&ls->fb) gui::Framebuffer();
    ls->screen_w = ls->fb.width();
    ls->screen_h = ls->fb.height();

    gui::fonts::init();
    montauk::set_mouse_bounds(ls->screen_w - 1, ls->screen_h - 1);
    load_login_wallpaper(ls);

    maybe_run_setup_session();
    initialize_login_mode(ls);

    bool first_frame = true;
    for (;;) {
        bool mouse_changed = false;
        bool key_changed = false;

        int prev_mouse_x = ls->mouse.x;
        int prev_mouse_y = ls->mouse.y;
        uint8_t prev_mouse_buttons = ls->mouse.buttons;
        ls->prev_buttons = ls->mouse.buttons;
        montauk::mouse_state(&ls->mouse);
        mouse_changed = ls->mouse.x != prev_mouse_x
                     || ls->mouse.y != prev_mouse_y
                     || ls->mouse.buttons != prev_mouse_buttons
                     || ls->mouse.scrollDelta != 0;

        while (montauk::is_key_available()) {
            Montauk::KeyEvent key;
            montauk::getkey(&key);
            handle_key(ls, key);
            key_changed = true;
        }

        if (mouse_changed) handle_mouse(ls);

        if (first_frame || mouse_changed || key_changed) {
            draw_login_screen(ls);
            first_frame = false;
            montauk::sleep_ms(4);
        } else {
            montauk::sleep_ms(16);
        }
    }
}
