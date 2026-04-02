/*
    * main.cpp
    * MontaukOS graphical login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#include <cstdint>
#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/user.h>
#include <montauk/config.h>
#include <gui/gui.hpp>
#include <gui/framebuffer.hpp>
#include <gui/draw.hpp>
#include <gui/svg.hpp>
#include <gui/font.hpp>

// Forward-declare stb_image functions (implementation in libjpeg.a).
extern "C" {
    unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                         int* x, int* y, int* channels_in_file,
                                         int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
}

using namespace gui;

// Placement new
inline void* operator new(unsigned long, void* p) { return p; }

// ---- Minimal snprintf ----

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState { char* buf; int pos; int max; };
static void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}
static void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24]; int i = 0;
    const char* digits = "0123456789abcdef";
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    int total = (neg ? 1 : 0) + i;
    if (neg && pad == '0') pf_putc(st, '-');
    for (int w = total; w < width; w++) pf_putc(st, pad);
    if (neg && pad != '0') pf_putc(st, '-');
    while (i > 0) pf_putc(st, tmp[--i]);
}
static int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    PfState st; st.buf = buf; st.pos = 0; st.max = size > 0 ? size - 1 : 0;
    while (*fmt) {
        if (*fmt != '%') { pf_putc(&st, *fmt++); continue; }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            long val = va_arg(ap, int);
            int neg = 0; unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); } else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg); break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            for (int w = slen; w < width; w++) pf_putc(&st, ' ');
            for (int j = 0; j < slen; j++) pf_putc(&st, s[j]);
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); pf_putc(&st, c); break; }
        case '%': pf_putc(&st, '%'); break;
        default: pf_putc(&st, '%'); pf_putc(&st, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (size > 0) { if (st.pos < size) st.buf[st.pos] = '\0'; else st.buf[size - 1] = '\0'; }
    va_end(ap); return st.pos;
}

// ============================================================================
// Login state
// ============================================================================

enum LoginMode {
    MODE_FIRST_BOOT,  // Create admin account
    MODE_LOGIN,       // Normal login
};

struct LoginState {
    Framebuffer fb;
    int screen_w, screen_h;

    LoginMode mode;
    int active_field;  // 0=username, 1=password, 2=confirm (first-boot only)

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

    // Power option icons (loaded once at startup)
    SvgIcon icon_shutdown;
    SvgIcon icon_reboot;

    // Background wallpaper (loaded from config)
    uint32_t* bg_wallpaper;
    int bg_wallpaper_w, bg_wallpaper_h;
    bool has_wallpaper;
};

// ============================================================================
// Wallpaper loading
// ============================================================================

static bool load_login_wallpaper(LoginState* ls) {
    // Load system-wide desktop config and read wallpaper.path
    auto doc = montauk::config::load("desktop");
    const char* wp = doc.get_string("wallpaper.path", "");
    if (wp[0] == '\0') return false;

    // Read JPEG file
    int fd = montauk::open(wp);
    if (fd < 0) return false;

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > 16 * 1024 * 1024) {
        montauk::close(fd);
        return false;
    }

    uint8_t* filedata = (uint8_t*)montauk::malloc(size);
    if (!filedata) { montauk::close(fd); return false; }

    int bytes_read = montauk::read(fd, filedata, 0, size);
    montauk::close(fd);
    if (bytes_read <= 0) { montauk::mfree(filedata); return false; }

    // Decode JPEG
    int img_w, img_h, channels;
    unsigned char* rgb = stbi_load_from_memory(filedata, bytes_read,
                                               &img_w, &img_h, &channels, 3);
    montauk::mfree(filedata);
    if (!rgb) return false;

    // Scale to cover screen (same algorithm as desktop wallpaper.hpp)
    int dst_w = ls->screen_w;
    int dst_h = ls->screen_h;

    uint32_t* scaled = (uint32_t*)montauk::malloc((uint64_t)dst_w * dst_h * 4);
    if (!scaled) { stbi_image_free(rgb); return false; }

    int src_crop_w, src_crop_h, src_x0, src_y0;
    if ((int64_t)img_w * dst_h > (int64_t)img_h * dst_w) {
        src_crop_h = img_h;
        src_crop_w = (int)((int64_t)img_h * dst_w / dst_h);
        src_x0 = (img_w - src_crop_w) / 2;
        src_y0 = 0;
    } else {
        src_crop_w = img_w;
        src_crop_h = (int)((int64_t)img_w * dst_h / dst_w);
        src_x0 = 0;
        src_y0 = (img_h - src_crop_h) / 2;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = src_y0 + (int)((int64_t)y * src_crop_h / dst_h);
        if (sy < 0) sy = 0;
        if (sy >= img_h) sy = img_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = src_x0 + (int)((int64_t)x * src_crop_w / dst_w);
            if (sx < 0) sx = 0;
            if (sx >= img_w) sx = img_w - 1;
            int si = (sy * img_w + sx) * 3;
            scaled[y * dst_w + x] = 0xFF000000u
                | ((uint32_t)rgb[si] << 16)
                | ((uint32_t)rgb[si + 1] << 8)
                | (uint32_t)rgb[si + 2];
        }
    }

    stbi_image_free(rgb);

    ls->bg_wallpaper = scaled;
    ls->bg_wallpaper_w = dst_w;
    ls->bg_wallpaper_h = dst_h;
    ls->has_wallpaper = true;
    return true;
}

// ============================================================================
// Drawing helpers
// ============================================================================

static constexpr Color BG_COLOR = Color::from_rgb(0x00, 0x00, 0x00);
static constexpr Color CARD_BG = colors::WINDOW_BG;
static constexpr Color CARD_BORDER = colors::BORDER;
static constexpr Color TITLEBAR_BG = colors::TITLEBAR_BG;
static constexpr Color FOOTER_BG = Color::from_rgb(0xF7, 0xF7, 0xF7);
static constexpr Color FIELD_BG = colors::WHITE;
static constexpr Color FIELD_BG_ACTIVE = Color::from_rgb(0xFA, 0xFC, 0xFF);
static constexpr Color FIELD_BORDER = colors::BORDER;
static constexpr Color FIELD_HOVER = Color::from_rgb(0xB7, 0xC7, 0xE8);
static constexpr Color FIELD_ACTIVE = colors::ACCENT;
static constexpr Color BTN_COLOR = colors::ACCENT;
static constexpr Color BTN_HOVER = Color::from_rgb(0x2B, 0x6B, 0xE0);
static constexpr Color BTN_TEXT = colors::WHITE;
static constexpr Color SECONDARY_BTN_BG = Color::from_rgb(0xE8, 0xE8, 0xE8);
static constexpr Color SECONDARY_BTN_HOVER = Color::from_rgb(0xDC, 0xDC, 0xDC);
static constexpr Color SECONDARY_BTN_BORDER = colors::BORDER;
static constexpr Color TEXT_COLOR = colors::TEXT_COLOR;
static constexpr Color LABEL_COLOR = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color ERROR_COLOR = Color::from_rgb(0xC0, 0x33, 0x33);

static constexpr int CARD_W = 432;
static constexpr int TITLEBAR_H = 30;
static constexpr int FOOTER_H = 58;
static constexpr int FIELD_H = 34;
static constexpr int FIELD_PAD = 10;
static constexpr int BTN_H = 30;
static constexpr int LABEL_GAP = 4;
static constexpr int CONTENT_PAD_X = 24;
static constexpr int CONTENT_TOP_PAD = 18;
static constexpr int BODY_BOTTOM_PAD = 18;
static constexpr int FIELD_GAP = 12;
static constexpr int POWER_ICON_SZ = 16;
static constexpr int POWER_BTN_W = 104;
static constexpr int POWER_BTN_GAP = 8;
static constexpr int FOOTER_PAD = 16;

struct LoginLayout {
    Rect card;
    Rect titlebar;
    Rect footer;
    Rect username_field;
    Rect display_name_field;
    Rect password_field;
    Rect confirm_field;
    Rect submit_button;
    Rect shutdown_button;
    Rect reboot_button;
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

static Rect offset_rect(Rect rect, int dx, int dy) {
    rect.x += dx;
    rect.y += dy;
    return rect;
}

static void draw_rounded_frame(Framebuffer& fb, const Rect& rect, int radius,
                               Color border, Color fill) {
    fill_rounded_rect(fb, rect.x, rect.y, rect.w, rect.h, radius, border);
    if (rect.w > 2 && rect.h > 2) {
        int inner_radius = radius > 0 ? radius - 1 : 0;
        fill_rounded_rect(fb, rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2,
                          inner_radius, fill);
    }
}

static LoginLayout layout_login_screen(LoginState* ls) {
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

static void draw_field(Framebuffer& fb, const Rect& rect, const char* text,
                       bool is_password, bool active, bool hovered) {
    Color border = active ? FIELD_ACTIVE : (hovered ? FIELD_HOVER : FIELD_BORDER);
    Color bg = active ? FIELD_BG_ACTIVE : FIELD_BG;
    draw_rounded_frame(fb, rect, 6, border, bg);

    int ty = rect.y + (rect.h - system_font_height()) / 2;
    int max_text_w = rect.w - FIELD_PAD * 2 - 2;
    if (is_password) {
        int len = montauk::slen(text);
        if (len > 64) len = 64;
        char full[65];
        for (int i = 0; i < len; i++) full[i] = '*';
        full[len] = '\0';
        // Show only trailing portion that fits
        int vis = len;
        while (vis > 0 && text_width(full + (len - vis)) > max_text_w)
            vis--;
        char masked[65];
        for (int i = 0; i < vis; i++) masked[i] = '*';
        masked[vis] = '\0';
        draw_text(fb, rect.x + FIELD_PAD, ty, masked, TEXT_COLOR);
        if (active) {
            int cx = rect.x + FIELD_PAD + text_width(masked);
            fb.fill_rect(cx, ty, 2, system_font_height(), FIELD_ACTIVE);
        }
    } else {
        int len = montauk::slen(text);
        int offset = 0;
        while (text_width(text + offset) > max_text_w && offset < len)
            offset++;
        draw_text(fb, rect.x + FIELD_PAD, ty, text + offset, TEXT_COLOR);
        if (active) {
            int cx = rect.x + FIELD_PAD + text_width(text + offset);
            fb.fill_rect(cx, ty, 2, system_font_height(), FIELD_ACTIVE);
        }
    }
}

static void draw_labeled_field(Framebuffer& fb, const Rect& rect, const char* label,
                               const char* text, bool is_password,
                               bool active, bool hovered) {
    draw_text(fb, rect.x, rect.y - LABEL_GAP - system_font_height(), label, LABEL_COLOR);
    draw_field(fb, rect, text, is_password, active, hovered);
}

static void draw_button(Framebuffer& fb, const Rect& rect, const char* label,
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

static void draw_power_button(Framebuffer& fb, const Rect& rect, const SvgIcon& icon,
                              const char* label, bool hovered) {
    draw_rounded_frame(fb, rect, 4,
                       hovered ? FIELD_HOVER : SECONDARY_BTN_BORDER,
                       hovered ? SECONDARY_BTN_HOVER : SECONDARY_BTN_BG);

    int icon_w = icon.pixels ? icon.width : 0;
    int gap = icon_w > 0 ? 8 : 0;
    int tw = text_width(label);
    int total_w = icon_w + gap + tw;
    int sx = rect.x + (rect.w - total_w) / 2;
    if (icon.pixels) {
        int iy = rect.y + (rect.h - icon.height) / 2;
        fb.blit_alpha(sx, iy, icon.width, icon.height, icon.pixels);
        sx += icon.width + gap;
    }
    int ty = rect.y + (rect.h - system_font_height()) / 2;
    draw_text(fb, sx, ty, label, TEXT_COLOR);
}

// ============================================================================
// Screen drawing
// ============================================================================

static void draw_login_screen(LoginState* ls) {
    Framebuffer& fb = ls->fb;
    LoginLayout lo = layout_login_screen(ls);
    int sfh = system_font_height();

    // Background
    if (ls->has_wallpaper) {
        fb.blit(0, 0, ls->bg_wallpaper_w, ls->bg_wallpaper_h, ls->bg_wallpaper);
        fb.fill_rect_alpha(0, 0, ls->screen_w, ls->screen_h, Color::from_rgba(0, 0, 0, 0x38));
    } else {
        fb.clear(BG_COLOR);
    }

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

    draw_labeled_field(fb, lo.username_field, "Username", ls->username, false,
                       ls->active_field == 0,
                       lo.username_field.contains(ls->mouse.x, ls->mouse.y));
    if (ls->mode == MODE_FIRST_BOOT) {
        draw_labeled_field(fb, lo.display_name_field, "Display Name", ls->display_name, false,
                           ls->active_field == 1,
                           lo.display_name_field.contains(ls->mouse.x, ls->mouse.y));
        draw_labeled_field(fb, lo.password_field, "Password", ls->password, true,
                           ls->active_field == 2,
                           lo.password_field.contains(ls->mouse.x, ls->mouse.y));
        draw_labeled_field(fb, lo.confirm_field, "Confirm Password", ls->confirm, true,
                           ls->active_field == 3,
                           lo.confirm_field.contains(ls->mouse.x, ls->mouse.y));
    } else {
        draw_labeled_field(fb, lo.password_field, "Password", ls->password, true,
                           ls->active_field == 1,
                           lo.password_field.contains(ls->mouse.x, ls->mouse.y));
    }

    if (ls->show_error && lo.error_y >= 0) {
        draw_text(fb, lo.content_x, lo.error_y, ls->error_msg, ERROR_COLOR);
    }

    draw_power_button(fb, lo.shutdown_button, ls->icon_shutdown, "Shut Down",
                      lo.shutdown_button.contains(ls->mouse.x, ls->mouse.y));
    draw_power_button(fb, lo.reboot_button, ls->icon_reboot, "Restart",
                      lo.reboot_button.contains(ls->mouse.x, ls->mouse.y));
    draw_button(fb, lo.submit_button, lo.submit_label,
                lo.submit_button.contains(ls->mouse.x, ls->mouse.y), true);

    // Mouse cursor (shared with desktop)
    draw_cursor(fb, ls->mouse.x, ls->mouse.y);

    fb.flip();
}

// ============================================================================
// Input handling
// ============================================================================

static void append_char(char* buf, int* len, int max, char c) {
    if (*len < max - 1) {
        buf[*len] = c;
        (*len)++;
        buf[*len] = '\0';
    }
}

static void backspace(char* buf, int* len) {
    if (*len > 0) {
        (*len)--;
        buf[*len] = '\0';
    }
}

static int max_fields(LoginState* ls) {
    return ls->mode == MODE_FIRST_BOOT ? 4 : 2;
}

static bool try_first_boot_submit(LoginState* ls) {
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

    // Create the users directory
    montauk::fmkdir("0:/users");

    const char* dname = ls->display_name_len > 0 ? ls->display_name : ls->username;
    if (!montauk::user::create_user(ls->username, dname, ls->password, "admin")) {
        montauk::strcpy(ls->error_msg, "Failed to create user");
        ls->show_error = true;
        return false;
    }

    return true;
}

static bool try_login(LoginState* ls) {
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

    // Persist login session state for the active desktop session.
    montauk::user::set_session(ls->username);
    return true;
}

static void handle_key(LoginState* ls, const Montauk::KeyEvent& key) {
    if (!key.pressed) return;

    // Tab switches fields
    if (key.scancode == 0x0F) {
        int mf = max_fields(ls);
        if (key.shift) {
            ls->active_field = (ls->active_field + mf - 1) % mf;
        } else {
            ls->active_field = (ls->active_field + 1) % mf;
        }
        ls->show_error = false;
        return;
    }

    // Enter submits
    if (key.ascii == '\n' || key.ascii == '\r') {
        if (ls->mode == MODE_FIRST_BOOT) {
            if (try_first_boot_submit(ls)) {
                // Switch to login mode
                ls->mode = MODE_LOGIN;
                ls->active_field = 0;
                // Keep username, clear password fields
                ls->password[0] = '\0'; ls->password_len = 0;
                ls->confirm[0] = '\0'; ls->confirm_len = 0;
                ls->show_error = false;
            }
        } else {
            if (try_login(ls)) {
                // Spawn desktop with username
                int pid = montauk::spawn("0:/os/desktop.elf", ls->username);
                if (pid >= 0) {
                    montauk::setuser(pid, ls->username);
                    montauk::waitpid(pid);
                }
                // Desktop exited (logout) — clear session and fields
                montauk::user::clear_session();
                ls->password[0] = '\0'; ls->password_len = 0;
                ls->active_field = 1;  // Focus password field
                ls->show_error = false;
            }
        }
        return;
    }

    // Backspace
    if (key.ascii == '\b' || key.scancode == 0x0E) {
        if (ls->mode == MODE_FIRST_BOOT) {
            switch (ls->active_field) {
            case 0: backspace(ls->username, &ls->username_len); break;
            case 1: backspace(ls->display_name, &ls->display_name_len); break;
            case 2: backspace(ls->password, &ls->password_len); break;
            case 3: backspace(ls->confirm, &ls->confirm_len); break;
            }
        } else {
            switch (ls->active_field) {
            case 0: backspace(ls->username, &ls->username_len); break;
            case 1: backspace(ls->password, &ls->password_len); break;
            }
        }
        return;
    }

    // Printable characters
    if (key.ascii >= 0x20 && key.ascii < 0x7F) {
        if (ls->mode == MODE_FIRST_BOOT) {
            switch (ls->active_field) {
            case 0: append_char(ls->username, &ls->username_len, 31, key.ascii); break;
            case 1: append_char(ls->display_name, &ls->display_name_len, 63, key.ascii); break;
            case 2: append_char(ls->password, &ls->password_len, 63, key.ascii); break;
            case 3: append_char(ls->confirm, &ls->confirm_len, 63, key.ascii); break;
            }
        } else {
            switch (ls->active_field) {
            case 0: append_char(ls->username, &ls->username_len, 31, key.ascii); break;
            case 1: append_char(ls->password, &ls->password_len, 63, key.ascii); break;
            }
        }
    }
}

static void handle_mouse(LoginState* ls) {
    int mx = ls->mouse.x;
    int my = ls->mouse.y;
    uint8_t buttons = ls->mouse.buttons;
    uint8_t prev = ls->prev_buttons;
    bool left_pressed = (buttons & 0x01) && !(prev & 0x01);

    if (!left_pressed) return;
    LoginLayout lo = layout_login_screen(ls);

    if (lo.username_field.contains(mx, my)) {
        ls->active_field = 0;
        return;
    }

    if (ls->mode == MODE_FIRST_BOOT) {
        if (lo.display_name_field.contains(mx, my)) {
            ls->active_field = 1;
            return;
        }
        if (lo.password_field.contains(mx, my)) {
            ls->active_field = 2;
            return;
        }
        if (lo.confirm_field.contains(mx, my)) {
            ls->active_field = 3;
            return;
        }
        if (lo.submit_button.contains(mx, my)) {
            if (try_first_boot_submit(ls)) {
                ls->mode = MODE_LOGIN;
                ls->active_field = 0;
                ls->password[0] = '\0'; ls->password_len = 0;
                ls->confirm[0] = '\0'; ls->confirm_len = 0;
                ls->show_error = false;
            }
            return;
        }
    } else {
        if (lo.password_field.contains(mx, my)) {
            ls->active_field = 1;
            return;
        }
        if (lo.submit_button.contains(mx, my)) {
            if (try_login(ls)) {
                int pid = montauk::spawn("0:/os/desktop.elf", ls->username);
                if (pid >= 0) {
                    montauk::setuser(pid, ls->username);
                    montauk::waitpid(pid);
                }
                montauk::user::clear_session();
                ls->password[0] = '\0'; ls->password_len = 0;
                ls->active_field = 1;
                ls->show_error = false;
            }
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

// ============================================================================
// Entry point
// ============================================================================

extern "C" void _start() {
    LoginState* ls = (LoginState*)montauk::malloc(sizeof(LoginState));
    montauk::memset(ls, 0, sizeof(LoginState));

    new (&ls->fb) Framebuffer();
    ls->screen_w = ls->fb.width();
    ls->screen_h = ls->fb.height();

    // Load TrueType fonts
    fonts::init();

    // Set mouse bounds
    montauk::set_mouse_bounds(ls->screen_w - 1, ls->screen_h - 1);

    // Load power option icons
    Color icon_color = colors::ICON_COLOR;
    ls->icon_shutdown = svg_load("0:/icons/system-shutdown.svg", POWER_ICON_SZ, POWER_ICON_SZ, icon_color);
    ls->icon_reboot = svg_load("0:/icons/system-reboot.svg", POWER_ICON_SZ, POWER_ICON_SZ, icon_color);

    // Load wallpaper from system desktop config
    load_login_wallpaper(ls);

    // Check for setup environment (installation medium / ramdisk boot).
    // If setup.toml exists with environment.mode = "setup", skip login
    // entirely and launch the desktop in a passwordless live session.
    {
        auto doc = montauk::config::load("setup");
        const char* mode = doc.get_string("environment.mode", "");
        if (montauk::streq(mode, "setup")) {
            const char* user = doc.get_string("session.username", "liveuser");
            const char* display = doc.get_string("session.display_name", "Live User");
            const char* role = doc.get_string("session.role", "admin");

            // Create a temporary user entry so the desktop and apps can
            // query session info normally.
            montauk::fmkdir("0:/users");
            montauk::user::create_user(user, display, "", role);
            montauk::user::set_session(user);
            doc.destroy();

            // Launch desktop directly -- no login required
            int pid = montauk::spawn("0:/os/desktop.elf", user);
            if (pid >= 0) {
                montauk::setuser(pid, user);
                montauk::waitpid(pid);
            }
            // Desktop exited (reboot/shutdown expected in setup mode).
            // Clear session and fall through to normal login in case
            // setup.toml was removed during installation.
            montauk::user::clear_session();
        } else {
            doc.destroy();
        }
    }

    // Check if users.toml exists (first boot detection)
    int fh = montauk::open("0:/config/users.toml");
    if (fh < 0) {
        ls->mode = MODE_FIRST_BOOT;
        ls->active_field = 0;
        // Pre-fill username with "admin"
        montauk::strcpy(ls->username, "admin");
        ls->username_len = 5;
    } else {
        montauk::close(fh);
        ls->mode = MODE_LOGIN;
        ls->active_field = 0;
    }

    // Main loop
    bool first_frame = true;
    for (;;) {
        bool mouse_changed = false;
        bool key_changed = false;

        // Poll mouse
        int prev_mouse_x = ls->mouse.x;
        int prev_mouse_y = ls->mouse.y;
        uint8_t prev_mouse_buttons = ls->mouse.buttons;
        ls->prev_buttons = ls->mouse.buttons;
        montauk::mouse_state(&ls->mouse);
        mouse_changed = ls->mouse.x != prev_mouse_x
                     || ls->mouse.y != prev_mouse_y
                     || ls->mouse.buttons != prev_mouse_buttons
                     || ls->mouse.scrollDelta != 0;

        // Poll keyboard
        while (montauk::is_key_available()) {
            Montauk::KeyEvent key;
            montauk::getkey(&key);
            handle_key(ls, key);
            key_changed = true;
        }

        // Handle mouse
        if (mouse_changed) {
            handle_mouse(ls);
        }

        if (first_frame || mouse_changed || key_changed) {
            draw_login_screen(ls);
            first_frame = false;
            montauk::sleep_ms(4);
        } else {
            montauk::sleep_ms(16);
        }
    }
}
