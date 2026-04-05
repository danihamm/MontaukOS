/*
    * login_shared.cpp
    * Shared field-edit helpers for the MontaukOS login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#include "login.hpp"

using namespace gui;

LoginField get_field(LoginState* ls, int field) {
    if (ls->mode == MODE_FIRST_BOOT) {
        switch (field) {
        case 0: return {ls->username, &ls->username_len, 31, false, &ls->username_edit};
        case 1: return {ls->display_name, &ls->display_name_len, 63, false, &ls->display_name_edit};
        case 2: return {ls->password, &ls->password_len, 63, true, &ls->password_edit};
        case 3: return {ls->confirm, &ls->confirm_len, 63, true, &ls->confirm_edit};
        default: break;
        }
    } else {
        switch (field) {
        case 0: return {ls->username, &ls->username_len, 31, false, &ls->username_edit};
        case 1: return {ls->password, &ls->password_len, 63, true, &ls->password_edit};
        default: break;
        }
    }
    return {nullptr, nullptr, 0, false, nullptr};
}

void reset_field_edit(FieldEditState* edit) {
    edit->cursor = 0;
    edit->sel_anchor = 0;
    edit->sel_end = 0;
    edit->has_selection = false;
}

void clear_field_selection(FieldEditState* edit) {
    edit->has_selection = false;
    edit->sel_anchor = edit->cursor;
    edit->sel_end = edit->cursor;
}

void clear_all_field_selections(LoginState* ls) {
    clear_field_selection(&ls->username_edit);
    clear_field_selection(&ls->display_name_edit);
    clear_field_selection(&ls->password_edit);
    clear_field_selection(&ls->confirm_edit);
}

void clamp_field_edit(FieldEditState* edit, int len) {
    if (edit->cursor < 0) edit->cursor = 0;
    if (edit->cursor > len) edit->cursor = len;
    if (edit->sel_anchor < 0) edit->sel_anchor = 0;
    if (edit->sel_anchor > len) edit->sel_anchor = len;
    if (edit->sel_end < 0) edit->sel_end = 0;
    if (edit->sel_end > len) edit->sel_end = len;
    if (!edit->has_selection || edit->sel_anchor == edit->sel_end)
        clear_field_selection(edit);
}

void field_sel_range(const FieldEditState* edit, int* out_s, int* out_e) {
    if (edit->sel_anchor < edit->sel_end) {
        *out_s = edit->sel_anchor;
        *out_e = edit->sel_end;
    } else {
        *out_s = edit->sel_end;
        *out_e = edit->sel_anchor;
    }
}

void start_field_selection(FieldEditState* edit) {
    if (!edit->has_selection) {
        edit->sel_anchor = edit->cursor;
        edit->sel_end = edit->cursor;
        edit->has_selection = true;
    }
}

void update_field_selection(FieldEditState* edit) {
    edit->sel_end = edit->cursor;
    if (edit->sel_anchor == edit->sel_end)
        clear_field_selection(edit);
}

int copy_slice(char* out, int cap, const char* text, int start, int end) {
    if (cap <= 0) return 0;
    if (start < 0) start = 0;
    if (end < start) end = start;
    int pos = 0;
    for (int i = start; i < end && pos < cap - 1; i++) out[pos++] = text[i];
    out[pos] = '\0';
    return pos;
}

int slice_width(const char* text, int start, int end) {
    char tmp[65];
    copy_slice(tmp, sizeof(tmp), text, start, end);
    return text_width(tmp);
}

int compute_view_start(const char* text, int len, int max_text_w, int cursor) {
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;
    int start = 0;
    while (start < cursor && slice_width(text, start, cursor) > max_text_w) start++;
    return start;
}

int compute_view_end(const char* text, int len, int max_text_w, int view_start) {
    int end = view_start;
    while (end < len && slice_width(text, view_start, end + 1) <= max_text_w) end++;
    return end;
}

int field_cursor_from_mouse_x(const char* text, int len, int max_text_w,
                              int anchor_cursor, int rel_x) {
    int view_start = compute_view_start(text, len, max_text_w, anchor_cursor);
    if (rel_x <= 0) return view_start;
    int view_end = compute_view_end(text, len, max_text_w, view_start);
    int prev_w = 0;
    for (int i = view_start; i < view_end; i++) {
        int next_w = slice_width(text, view_start, i + 1);
        int mid = prev_w + (next_w - prev_w) / 2;
        if (rel_x < mid) return i;
        prev_w = next_w;
    }
    return view_end;
}

void delete_field_range(char* buf, int* len, int start, int count) {
    if (count <= 0 || start < 0 || start >= *len) return;
    if (start + count > *len) count = *len - start;
    for (int i = start; i + count < *len; i++) buf[i] = buf[i + count];
    *len -= count;
    buf[*len] = '\0';
}

void delete_field_selection(char* buf, int* len, FieldEditState* edit) {
    if (!edit->has_selection) return;
    int ss, se;
    field_sel_range(edit, &ss, &se);
    delete_field_range(buf, len, ss, se - ss);
    edit->cursor = ss;
    clear_field_selection(edit);
}

int password_mask_advance() {
    return text_width("*");
}

int password_slice_width(int start, int end) {
    if (end < start) end = start;
    return (end - start) * password_mask_advance();
}

int compute_password_view_start(int len, int max_text_w, int cursor) {
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;
    int start = 0;
    while (start < cursor && password_slice_width(start, cursor) > max_text_w) start++;
    return start;
}

int compute_password_view_end(int len, int max_text_w, int view_start) {
    int end = view_start;
    while (end < len && password_slice_width(view_start, end + 1) <= max_text_w) end++;
    return end;
}

int password_cursor_from_mouse_x(int len, int max_text_w, int anchor_cursor, int rel_x) {
    int view_start = compute_password_view_start(len, max_text_w, anchor_cursor);
    if (rel_x <= 0) return view_start;
    int view_end = compute_password_view_end(len, max_text_w, view_start);
    int advance = password_mask_advance();
    int prev_w = 0;
    for (int i = view_start; i < view_end; i++) {
        int next_w = prev_w + advance;
        int mid = prev_w + (next_w - prev_w) / 2;
        if (rel_x < mid) return i;
        prev_w = next_w;
    }
    return view_end;
}
