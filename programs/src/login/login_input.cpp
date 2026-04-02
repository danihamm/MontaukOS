/*
    * login_input.cpp
    * Input and session flow for the MontaukOS login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#include "login.hpp"

namespace {

void transition_to_login_mode(LoginState* ls) {
    ls->mode = MODE_LOGIN;
    ls->active_field = 0;
    ls->password[0] = '\0';
    ls->password_len = 0;
    ls->confirm[0] = '\0';
    ls->confirm_len = 0;
    reset_field_edit(&ls->password_edit);
    reset_field_edit(&ls->confirm_edit);
    clear_all_field_selections(ls);
    ls->show_error = false;
}

void finish_desktop_session(LoginState* ls) {
    montauk::user::clear_session();
    ls->password[0] = '\0';
    ls->password_len = 0;
    reset_field_edit(&ls->password_edit);
    clear_all_field_selections(ls);
    ls->active_field = 1;
    ls->show_error = false;
}

void launch_desktop_session(LoginState* ls) {
    int pid = montauk::spawn("0:/os/desktop.elf", ls->username);
    if (pid >= 0) {
        montauk::setuser(pid, ls->username);
        montauk::waitpid(pid);
    }
    finish_desktop_session(ls);
}

void insert_char(char* buf, int* len, int max_len, FieldEditState* edit, char c) {
    if (edit->has_selection) delete_field_selection(buf, len, edit);
    if (*len >= max_len) return;
    for (int i = *len; i > edit->cursor; i--) buf[i] = buf[i - 1];
    buf[edit->cursor] = c;
    (*len)++;
    buf[*len] = '\0';
    edit->cursor++;
    clear_field_selection(edit);
}

void backspace_char(char* buf, int* len, FieldEditState* edit) {
    if (edit->has_selection) {
        delete_field_selection(buf, len, edit);
        return;
    }
    if (edit->cursor <= 0) return;
    delete_field_range(buf, len, edit->cursor - 1, 1);
    edit->cursor--;
    clear_field_selection(edit);
}

void delete_char(char* buf, int* len, FieldEditState* edit) {
    if (edit->has_selection) {
        delete_field_selection(buf, len, edit);
        return;
    }
    if (edit->cursor >= *len) return;
    delete_field_range(buf, len, edit->cursor, 1);
    clear_field_selection(edit);
}

} // namespace

int max_fields(LoginState* ls) {
    return ls->mode == MODE_FIRST_BOOT ? 4 : 2;
}

bool try_first_boot_submit(LoginState* ls) {
    ls->show_error = false;

    if (ls->username_len == 0) {
        montauk::strcpy(ls->error_msg, "Username is required");
        ls->show_error = true;
        return false;
    }
    if (ls->password_len == 0) {
        montauk::strcpy(ls->error_msg, "Password is required");
        ls->show_error = true;
        return false;
    }
    if (!montauk::streq(ls->password, ls->confirm)) {
        montauk::strcpy(ls->error_msg, "Passwords do not match");
        ls->show_error = true;
        return false;
    }

    montauk::fmkdir("0:/users");

    const char* dname = ls->display_name_len > 0 ? ls->display_name : ls->username;
    if (!montauk::user::create_user(ls->username, dname, ls->password, "admin")) {
        montauk::strcpy(ls->error_msg, "Failed to create user");
        ls->show_error = true;
        return false;
    }

    return true;
}

bool try_login(LoginState* ls) {
    ls->show_error = false;

    if (ls->username_len == 0) {
        montauk::strcpy(ls->error_msg, "Username is required");
        ls->show_error = true;
        return false;
    }

    if (!montauk::user::authenticate(ls->username, ls->password)) {
        montauk::strcpy(ls->error_msg, "Invalid username or password");
        ls->show_error = true;
        return false;
    }

    montauk::user::set_session(ls->username);
    return true;
}

void handle_key(LoginState* ls, const Montauk::KeyEvent& key) {
    if (!key.pressed) return;

    if (key.scancode == 0x0F) {
        int mf = max_fields(ls);
        clear_all_field_selections(ls);
        if (key.shift) ls->active_field = (ls->active_field + mf - 1) % mf;
        else ls->active_field = (ls->active_field + 1) % mf;
        ls->show_error = false;
        return;
    }

    LoginField field = get_field(ls, ls->active_field);
    if (!field.text || !field.len || !field.edit) return;
    clamp_field_edit(field.edit, *field.len);

    if (key.ascii == '\n' || key.ascii == '\r') {
        if (ls->mode == MODE_FIRST_BOOT) {
            if (try_first_boot_submit(ls)) transition_to_login_mode(ls);
        } else if (try_login(ls)) {
            launch_desktop_session(ls);
        }
        return;
    }

    if (key.ctrl && (key.ascii == 'a' || key.ascii == 'A')) {
        field.edit->cursor = *field.len;
        field.edit->sel_anchor = 0;
        field.edit->sel_end = *field.len;
        field.edit->has_selection = (*field.len > 0);
        return;
    }

    if (key.scancode == 0x4B) {
        if (key.shift) {
            start_field_selection(field.edit);
            if (field.edit->cursor > 0) field.edit->cursor--;
            update_field_selection(field.edit);
        } else if (field.edit->has_selection) {
            int ss, se;
            field_sel_range(field.edit, &ss, &se);
            field.edit->cursor = ss;
            clear_field_selection(field.edit);
        } else if (field.edit->cursor > 0) {
            field.edit->cursor--;
        }
        return;
    }

    if (key.scancode == 0x4D) {
        if (key.shift) {
            start_field_selection(field.edit);
            if (field.edit->cursor < *field.len) field.edit->cursor++;
            update_field_selection(field.edit);
        } else if (field.edit->has_selection) {
            int ss, se;
            field_sel_range(field.edit, &ss, &se);
            field.edit->cursor = se;
            clear_field_selection(field.edit);
        } else if (field.edit->cursor < *field.len) {
            field.edit->cursor++;
        }
        return;
    }

    if (key.scancode == 0x47) {
        if (key.shift) start_field_selection(field.edit);
        else clear_field_selection(field.edit);
        field.edit->cursor = 0;
        if (key.shift) update_field_selection(field.edit);
        return;
    }

    if (key.scancode == 0x4F) {
        if (key.shift) start_field_selection(field.edit);
        else clear_field_selection(field.edit);
        field.edit->cursor = *field.len;
        if (key.shift) update_field_selection(field.edit);
        return;
    }

    if (key.scancode == 0x53) {
        delete_char(field.text, field.len, field.edit);
        return;
    }

    if (key.ascii == '\b' || key.scancode == 0x0E) {
        backspace_char(field.text, field.len, field.edit);
        return;
    }

    if (key.ascii >= 0x20 && key.ascii < 0x7F) {
        insert_char(field.text, field.len, field.max_len, field.edit, key.ascii);
    }
}

void handle_mouse(LoginState* ls) {
    int mx = ls->mouse.x;
    int my = ls->mouse.y;
    uint8_t buttons = ls->mouse.buttons;
    uint8_t prev = ls->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);

    if (!left_pressed) return;
    LoginLayout lo = layout_login_screen(ls);

    auto focus_field = [&](int field_index, const gui::Rect& rect) {
        LoginField field = get_field(ls, field_index);
        if (!field.text || !field.len || !field.edit) return;
        clamp_field_edit(field.edit, *field.len);
        int anchor_cursor = (field_index == ls->active_field) ? field.edit->cursor : *field.len;
        int rel_x = mx - (rect.x + FIELD_PAD);
        int max_text_w = rect.w - FIELD_PAD * 2 - 2;
        int cursor = field.is_password
            ? password_cursor_from_mouse_x(*field.len, max_text_w, anchor_cursor, rel_x)
            : field_cursor_from_mouse_x(field.text, *field.len, max_text_w, anchor_cursor, rel_x);
        clear_all_field_selections(ls);
        ls->active_field = field_index;
        field.edit->cursor = cursor;
        clear_field_selection(field.edit);
    };

    if (lo.username_field.contains(mx, my)) {
        focus_field(0, lo.username_field);
        return;
    }

    if (ls->mode == MODE_FIRST_BOOT) {
        if (lo.display_name_field.contains(mx, my)) {
            focus_field(1, lo.display_name_field);
            return;
        }
        if (lo.password_field.contains(mx, my)) {
            focus_field(2, lo.password_field);
            return;
        }
        if (lo.confirm_field.contains(mx, my)) {
            focus_field(3, lo.confirm_field);
            return;
        }
        if (lo.submit_button.contains(mx, my)) {
            if (try_first_boot_submit(ls)) transition_to_login_mode(ls);
            return;
        }
    } else {
        if (lo.password_field.contains(mx, my)) {
            focus_field(1, lo.password_field);
            return;
        }
        if (lo.submit_button.contains(mx, my)) {
            if (try_login(ls)) launch_desktop_session(ls);
            return;
        }
    }

    if (lo.shutdown_button.contains(mx, my)) {
        montauk::shutdown();
        return;
    }
    if (lo.reboot_button.contains(mx, my)) {
        montauk::reset();
    }
}
