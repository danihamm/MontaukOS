/*
    * login_render.cpp
    * Layout and drawing for the MontaukOS login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#include "login.hpp"

using namespace gui;

namespace {

void draw_login_background(LoginState* ls) {
    Framebuffer& fb = ls->fb;
    if (!ls->has_wallpaper) {
        fb.clear(BG_COLOR);
        return;
    }

    uint32_t* dst = fb.buffer();
    int pitch = fb.pitch();
    int row_bytes = ls->screen_w * (int)sizeof(uint32_t);
    for (int y = 0; y < ls->screen_h; y++) {
        uint32_t* dst_row = (uint32_t*)((uint8_t*)dst + y * pitch);
        const uint32_t* src_row = ls->bg_wallpaper + y * ls->bg_wallpaper_w;
        montauk::memcpy(dst_row, src_row, row_bytes);
    }
}

Rect offset_rect(Rect rect, int dx, int dy) {
    rect.x += dx;
    rect.y += dy;
    return rect;
}

void build_display_text(char* out, int cap, const char* text, int len) {
    if (cap <= 0) return;
    if (len < 0) len = 0;
    int pos = 0;
    while (pos < len && pos < cap - 1) {
        out[pos] = text[pos];
        pos++;
    }
    out[pos] = '\0';
}

void draw_text_slice(Framebuffer& fb, int x, int y, const char* text,
                     int start, int end, Color fg,
                     bool highlight = false, Color bg = colors::TRANSPARENT) {
    char tmp[65];
    if (copy_slice(tmp, sizeof(tmp), text, start, end) <= 0) return;
    if (highlight) draw_text_bg(fb, x, y, tmp, fg, bg);
    else draw_text(fb, x, y, tmp, fg);
}

void draw_password_mask_char(Framebuffer& fb, int x, int y, Color fg,
                             bool highlight = false, Color bg = colors::TRANSPARENT) {
    if (highlight) draw_text_bg(fb, x, y, "*", fg, bg);
    else draw_text(fb, x, y, "*", fg);
}

void draw_rounded_frame(Framebuffer& fb, const Rect& rect, int radius,
                        Color border, Color fill) {
    fill_rounded_rect(fb, rect.x, rect.y, rect.w, rect.h, radius, border);
    if (rect.w > 2 && rect.h > 2) {
        int inner_radius = radius > 0 ? radius - 1 : 0;
        fill_rounded_rect(fb, rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2,
                          inner_radius, fill);
    }
}

void draw_field(Framebuffer& fb, const Rect& rect, const char* text, int len,
                const FieldEditState* edit, bool is_password,
                bool active, bool hovered) {
    Color border = active ? FIELD_ACTIVE : (hovered ? FIELD_HOVER : FIELD_BORDER);
    Color bg = active ? FIELD_BG_ACTIVE : FIELD_BG;
    draw_rounded_frame(fb, rect, 6, border, bg);

    int ty = rect.y + (rect.h - system_font_height()) / 2;
    int max_text_w = rect.w - FIELD_PAD * 2 - 2;
    int cursor = active && edit ? edit->cursor : len;

    if (is_password) {
        int view_start = compute_password_view_start(len, max_text_w, cursor);
        int view_end = compute_password_view_end(len, max_text_w, view_start);
        int advance = password_mask_advance();
        int sel_s = 0;
        int sel_e = 0;
        bool has_selection = active && edit && edit->has_selection;
        if (has_selection) field_sel_range(edit, &sel_s, &sel_e);

        for (int i = view_start; i < view_end; i++) {
            int cx = rect.x + FIELD_PAD + (i - view_start) * advance;
            bool selected = has_selection && i >= sel_s && i < sel_e;
            draw_password_mask_char(fb, cx, ty, TEXT_COLOR, selected, FIELD_SEL_BG);
        }

        if (active && edit) {
            int cx = rect.x + FIELD_PAD + password_slice_width(view_start, edit->cursor);
            fb.fill_rect(cx, ty, 2, system_font_height(), FIELD_ACTIVE);
        }
        return;
    }

    char display[65];
    build_display_text(display, sizeof(display), text, len);

    int view_start = compute_view_start(display, len, max_text_w, cursor);
    int view_end = compute_view_end(display, len, max_text_w, view_start);
    int text_x = rect.x + FIELD_PAD;

    if (active && edit && edit->has_selection) {
        int sel_s, sel_e;
        field_sel_range(edit, &sel_s, &sel_e);
        if (sel_s < view_end && sel_e > view_start) {
            int prefix_end = sel_s < view_start ? view_start : sel_s;
            int hi_start = sel_s > view_start ? sel_s : view_start;
            int hi_end = sel_e < view_end ? sel_e : view_end;

            draw_text_slice(fb, text_x, ty, display, view_start, prefix_end, TEXT_COLOR);
            text_x += slice_width(display, view_start, prefix_end);
            draw_text_slice(fb, text_x, ty, display, hi_start, hi_end, TEXT_COLOR, true, FIELD_SEL_BG);
            text_x += slice_width(display, hi_start, hi_end);
            draw_text_slice(fb, text_x, ty, display, hi_end, view_end, TEXT_COLOR);
        } else {
            draw_text_slice(fb, text_x, ty, display, view_start, view_end, TEXT_COLOR);
        }
    } else {
        draw_text_slice(fb, text_x, ty, display, view_start, view_end, TEXT_COLOR);
    }

    if (active && edit) {
        int cx = rect.x + FIELD_PAD + slice_width(display, view_start, edit->cursor);
        fb.fill_rect(cx, ty, 2, system_font_height(), FIELD_ACTIVE);
    }
}

void draw_labeled_field(Framebuffer& fb, const Rect& rect, const char* label,
                        const char* text, int len, const FieldEditState* edit,
                        bool is_password, bool active, bool hovered) {
    draw_text(fb, rect.x, rect.y - LABEL_GAP - system_font_height(), label, LABEL_COLOR);
    draw_field(fb, rect, text, len, edit, is_password, active, hovered);
}

void draw_button(Framebuffer& fb, const Rect& rect, const char* label,
                 bool hovered, bool primary) {
    if (primary) {
        fill_rounded_rect(fb, rect.x, rect.y, rect.w, rect.h, 4,
                          hovered ? BTN_HOVER : BTN_COLOR);
    } else {
        draw_rounded_frame(fb, rect, 4,
                           hovered ? FIELD_HOVER : SECONDARY_BTN_BORDER,
                           hovered ? SECONDARY_BTN_HOVER : SECONDARY_BTN_BG);
    }
    int tw = text_width(label);
    int tx = rect.x + (rect.w - tw) / 2;
    int ty = rect.y + (rect.h - system_font_height()) / 2;
    draw_text(fb, tx, ty, label, primary ? BTN_TEXT : TEXT_COLOR);
}

void draw_power_button(Framebuffer& fb, const Rect& rect, const char* label,
                       bool hovered) {
    draw_rounded_frame(fb, rect, 4,
                       hovered ? FIELD_HOVER : SECONDARY_BTN_BORDER,
                       hovered ? SECONDARY_BTN_HOVER : SECONDARY_BTN_BG);
    int tw = text_width(label);
    int tx = rect.x + (rect.w - tw) / 2;
    int ty = rect.y + (rect.h - system_font_height()) / 2;
    draw_text(fb, tx, ty, label, TEXT_COLOR);
}

} // namespace

LoginLayout layout_login_screen(LoginState* ls) {
    LoginLayout lo = {};
    int sfh = system_font_height();

    lo.window_title = ls->mode == MODE_FIRST_BOOT ? "MontaukOS Setup" : "MontaukOS";
    lo.heading = ls->mode == MODE_FIRST_BOOT ? "Create Administrator Account" : "Log In";
    lo.subtitle = ls->mode == MODE_FIRST_BOOT
        ? "Create the first administrator account."
        : "Sign in to continue to the desktop.";
    lo.submit_label = ls->mode == MODE_FIRST_BOOT ? "Create Account" : "Log In";

    lo.content_x = CONTENT_PAD_X;
    lo.content_w = CARD_W - CONTENT_PAD_X * 2;

    int y = TITLEBAR_H + CONTENT_TOP_PAD;
    lo.heading_y = y;
    y += sfh + 6;
    lo.subtitle_y = y;
    y += sfh + 18;

    lo.username_field = {lo.content_x, y + sfh + LABEL_GAP, lo.content_w, FIELD_H};
    y = lo.username_field.y + FIELD_H + FIELD_GAP;

    if (ls->mode == MODE_FIRST_BOOT) {
        lo.display_name_field = {lo.content_x, y + sfh + LABEL_GAP, lo.content_w, FIELD_H};
        y = lo.display_name_field.y + FIELD_H + FIELD_GAP;

        lo.password_field = {lo.content_x, y + sfh + LABEL_GAP, lo.content_w, FIELD_H};
        y = lo.password_field.y + FIELD_H + FIELD_GAP;

        lo.confirm_field = {lo.content_x, y + sfh + LABEL_GAP, lo.content_w, FIELD_H};
        y = lo.confirm_field.y + FIELD_H + FIELD_GAP;
    } else {
        lo.password_field = {lo.content_x, y + sfh + LABEL_GAP, lo.content_w, FIELD_H};
        y = lo.password_field.y + FIELD_H + FIELD_GAP;
    }

    if (ls->show_error) {
        lo.error_y = y;
        y += sfh + 12;
    } else {
        lo.error_y = -1;
    }

    int footer_y = y + BODY_BOTTOM_PAD;
    int card_h = footer_y + FOOTER_H;
    int card_x = (ls->screen_w - CARD_W) / 2;
    int card_y = (ls->screen_h - card_h) / 2;

    lo.card = {card_x, card_y, CARD_W, card_h};
    lo.titlebar = {card_x, card_y, CARD_W, TITLEBAR_H};
    lo.footer = {card_x, card_y + footer_y, CARD_W, FOOTER_H};

    int submit_w = text_width(lo.submit_label) + 32;
    if (submit_w < 100) submit_w = 100;
    int footer_btn_y = footer_y + (FOOTER_H - BTN_H) / 2;
    lo.shutdown_button = {FOOTER_PAD, footer_btn_y, POWER_BTN_W, BTN_H};
    lo.reboot_button = {FOOTER_PAD + POWER_BTN_W + POWER_BTN_GAP, footer_btn_y, POWER_BTN_W, BTN_H};
    lo.submit_button = {CARD_W - FOOTER_PAD - submit_w, footer_btn_y, submit_w, BTN_H};

    lo.username_field = offset_rect(lo.username_field, card_x, card_y);
    lo.display_name_field = offset_rect(lo.display_name_field, card_x, card_y);
    lo.password_field = offset_rect(lo.password_field, card_x, card_y);
    lo.confirm_field = offset_rect(lo.confirm_field, card_x, card_y);
    lo.shutdown_button = offset_rect(lo.shutdown_button, card_x, card_y);
    lo.reboot_button = offset_rect(lo.reboot_button, card_x, card_y);
    lo.submit_button = offset_rect(lo.submit_button, card_x, card_y);

    lo.content_x += card_x;
    lo.heading_y += card_y;
    lo.subtitle_y += card_y;
    if (lo.error_y >= 0) lo.error_y += card_y;
    return lo;
}

void draw_login_screen(LoginState* ls) {
    Framebuffer& fb = ls->fb;
    LoginLayout lo = layout_login_screen(ls);
    int sfh = system_font_height();

    draw_login_background(ls);

    draw_shadow(fb, lo.card.x, lo.card.y, lo.card.w, lo.card.h, 4, colors::SHADOW);
    fb.fill_rect(lo.card.x, lo.card.y, lo.card.w, lo.card.h, CARD_BG);
    fb.fill_rect(lo.titlebar.x, lo.titlebar.y, lo.titlebar.w, lo.titlebar.h, TITLEBAR_BG);
    fb.fill_rect(lo.footer.x, lo.footer.y, lo.footer.w, lo.footer.h, FOOTER_BG);
    draw_rect(fb, lo.card.x, lo.card.y, lo.card.w, lo.card.h, CARD_BORDER);
    draw_hline(fb, lo.titlebar.x, lo.titlebar.y + lo.titlebar.h - 1, lo.titlebar.w, CARD_BORDER);
    draw_hline(fb, lo.footer.x, lo.footer.y, lo.footer.w, CARD_BORDER);

    int window_tw = text_width(lo.window_title);
    draw_text(fb, lo.titlebar.x + (lo.titlebar.w - window_tw) / 2,
              lo.titlebar.y + (TITLEBAR_H - sfh) / 2, lo.window_title, TEXT_COLOR);
    draw_text(fb, lo.content_x, lo.heading_y, lo.heading, TEXT_COLOR);
    draw_text(fb, lo.content_x, lo.subtitle_y, lo.subtitle, LABEL_COLOR);

    draw_labeled_field(fb, lo.username_field, "Username", ls->username, ls->username_len,
                       &ls->username_edit, false,
                       ls->active_field == 0,
                       lo.username_field.contains(ls->mouse.x, ls->mouse.y));
    if (ls->mode == MODE_FIRST_BOOT) {
        draw_labeled_field(fb, lo.display_name_field, "Display Name", ls->display_name,
                           ls->display_name_len, &ls->display_name_edit, false,
                           ls->active_field == 1,
                           lo.display_name_field.contains(ls->mouse.x, ls->mouse.y));
        draw_labeled_field(fb, lo.password_field, "Password", ls->password,
                           ls->password_len, &ls->password_edit, true,
                           ls->active_field == 2,
                           lo.password_field.contains(ls->mouse.x, ls->mouse.y));
        draw_labeled_field(fb, lo.confirm_field, "Confirm Password", ls->confirm,
                           ls->confirm_len, &ls->confirm_edit, true,
                           ls->active_field == 3,
                           lo.confirm_field.contains(ls->mouse.x, ls->mouse.y));
    } else {
        draw_labeled_field(fb, lo.password_field, "Password", ls->password,
                           ls->password_len, &ls->password_edit, true,
                           ls->active_field == 1,
                           lo.password_field.contains(ls->mouse.x, ls->mouse.y));
    }

    if (ls->show_error && lo.error_y >= 0) {
        draw_text(fb, lo.content_x, lo.error_y, ls->error_msg, ERROR_COLOR);
    }

    draw_power_button(fb, lo.shutdown_button, "Shut Down",
                      lo.shutdown_button.contains(ls->mouse.x, ls->mouse.y));
    draw_power_button(fb, lo.reboot_button, "Restart",
                      lo.reboot_button.contains(ls->mouse.x, ls->mouse.y));
    draw_button(fb, lo.submit_button, lo.submit_label,
                lo.submit_button.contains(ls->mouse.x, ls->mouse.y), true);

    draw_cursor(fb, ls->mouse.x, ls->mouse.y);
    fb.flip();
}
