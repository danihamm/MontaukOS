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

static constexpr Color BG_COLOR = Color::from_rgb(0x2B, 0x3E, 0x50);
static constexpr Color CARD_BG = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color FIELD_BG = Color::from_rgb(0xF5, 0xF5, 0xF5);
static constexpr Color FIELD_BORDER = Color::from_rgb(0xCC, 0xCC, 0xCC);
static constexpr Color FIELD_ACTIVE = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color BTN_COLOR = Color::from_rgb(0x36, 0x7B, 0xF0);
static constexpr Color BTN_TEXT = Color::from_rgb(0xFF, 0xFF, 0xFF);
static constexpr Color TEXT_COLOR = Color::from_rgb(0x33, 0x33, 0x33);
static constexpr Color LABEL_COLOR = Color::from_rgb(0x66, 0x66, 0x66);
static constexpr Color ERROR_COLOR = Color::from_rgb(0xE0, 0x40, 0x40);
static constexpr Color TITLE_COLOR = Color::from_rgb(0xFF, 0xFF, 0xFF);

static constexpr int CARD_W = 360;
static constexpr int FIELD_H = 36;
static constexpr int FIELD_PAD = 10;
static constexpr int BTN_H = 40;
static constexpr int LABEL_GAP = 4;
static constexpr int POWER_ICON_SZ = 32;
static constexpr int POWER_GAP = 48;     // horizontal spacing between icon centers
static constexpr int POWER_TOP_PAD = 24; // padding below error message area

static void draw_field(Framebuffer& fb, int x, int y, int w, const char* text,
                        bool is_password, bool active) {
    // Background
    fb.fill_rect(x, y, w, FIELD_H, FIELD_BG);

    // Border
    Color border = active ? FIELD_ACTIVE : FIELD_BORDER;
    // Top
    for (int i = 0; i < w; i++) fb.put_pixel(x + i, y, border);
    // Bottom
    for (int i = 0; i < w; i++) fb.put_pixel(x + i, y + FIELD_H - 1, border);
    // Left
    for (int i = 0; i < FIELD_H; i++) fb.put_pixel(x, y + i, border);
    // Right
    for (int i = 0; i < FIELD_H; i++) fb.put_pixel(x + w - 1, y + i, border);

    if (active) {
        // Draw 2px border for active field
        for (int i = 0; i < w; i++) fb.put_pixel(x + i, y + 1, border);
        for (int i = 0; i < w; i++) fb.put_pixel(x + i, y + FIELD_H - 2, border);
        for (int i = 0; i < FIELD_H; i++) fb.put_pixel(x + 1, y + i, border);
        for (int i = 0; i < FIELD_H; i++) fb.put_pixel(x + w - 2, y + i, border);
    }

    // Text (clipped to field width, showing trailing portion)
    int ty = y + (FIELD_H - system_font_height()) / 2;
    int max_text_w = w - FIELD_PAD * 2 - 2; // left pad, right pad, cursor
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
        draw_text(fb, x + FIELD_PAD, ty, masked, TEXT_COLOR);
        if (active) {
            int cx = x + FIELD_PAD + text_width(masked);
            fb.fill_rect(cx, ty, 2, system_font_height(), FIELD_ACTIVE);
        }
    } else {
        int len = montauk::slen(text);
        // Show only trailing portion that fits
        int offset = 0;
        while (text_width(text + offset) > max_text_w && offset < len)
            offset++;
        draw_text(fb, x + FIELD_PAD, ty, text + offset, TEXT_COLOR);
        if (active) {
            int cx = x + FIELD_PAD + text_width(text + offset);
            fb.fill_rect(cx, ty, 2, system_font_height(), FIELD_ACTIVE);
        }
    }
}

static void draw_button(Framebuffer& fb, int x, int y, int w, int h,
                          const char* label, Color bg) {
    fb.fill_rect(x, y, w, h, bg);
    int tw = text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - system_font_height()) / 2;
    draw_text(fb, tx, ty, label, BTN_TEXT);
}

// ============================================================================
// Screen drawing
// ============================================================================

static void draw_login_screen(LoginState* ls) {
    Framebuffer& fb = ls->fb;

    // Background
    if (ls->has_wallpaper) {
        fb.blit(0, 0, ls->bg_wallpaper_w, ls->bg_wallpaper_h, ls->bg_wallpaper);
    } else {
        fb.clear(BG_COLOR);
    }

    int card_h;
    const char* title;
    int sfh = system_font_height();
    int power_section_h = POWER_ICON_SZ + sfh + 6 + 4; // icon + label + gap + padding
    int error_section_h = ls->show_error ? sfh + 8 : 0;  // error text + padding
    if (ls->mode == MODE_FIRST_BOOT) {
        card_h = 420 + error_section_h + power_section_h;
        title = "Create Administrator Account";
    } else {
        card_h = 284 + error_section_h + power_section_h;
        title = "Log In";
    }

    int card_x = (ls->screen_w - CARD_W) / 2;
    int card_y = (ls->screen_h - card_h) / 2;

    // Card background
    fb.fill_rect(card_x, card_y, CARD_W, card_h, CARD_BG);

    int x = card_x + 24;
    int content_w = CARD_W - 48;
    int y = card_y + 20;

    // Card title
    {
        int tw = text_width(title);
        draw_text(fb, card_x + (CARD_W - tw) / 2, y, title, TEXT_COLOR);
        y += sfh + 16;
    }

    if (ls->mode == MODE_FIRST_BOOT) {
        // Username field
        draw_text(fb, x, y, "Username", LABEL_COLOR);
        y += sfh + LABEL_GAP;
        draw_field(fb, x, y, content_w, ls->username, false, ls->active_field == 0);
        y += FIELD_H + 12;

        // Display name field
        draw_text(fb, x, y, "Display Name", LABEL_COLOR);
        y += sfh + LABEL_GAP;
        draw_field(fb, x, y, content_w, ls->display_name, false, ls->active_field == 1);
        y += FIELD_H + 12;

        // Password field
        draw_text(fb, x, y, "Password", LABEL_COLOR);
        y += sfh + LABEL_GAP;
        draw_field(fb, x, y, content_w, ls->password, true, ls->active_field == 2);
        y += FIELD_H + 12;

        // Confirm password field
        draw_text(fb, x, y, "Confirm Password", LABEL_COLOR);
        y += sfh + LABEL_GAP;
        draw_field(fb, x, y, content_w, ls->confirm, true, ls->active_field == 3);
        y += FIELD_H + 16;

        // Create button
        draw_button(fb, x, y, content_w, BTN_H, "Create Account", BTN_COLOR);
        y += BTN_H;
    } else {
        // Username field
        draw_text(fb, x, y, "Username", LABEL_COLOR);
        y += sfh + LABEL_GAP;
        draw_field(fb, x, y, content_w, ls->username, false, ls->active_field == 0);
        y += FIELD_H + 12;

        // Password field
        draw_text(fb, x, y, "Password", LABEL_COLOR);
        y += sfh + LABEL_GAP;
        draw_field(fb, x, y, content_w, ls->password, true, ls->active_field == 1);
        y += FIELD_H + 16;

        // Login button
        draw_button(fb, x, y, content_w, BTN_H, "Log In", BTN_COLOR);
        y += BTN_H;
    }

    // Error message (inside card, between button and separator)
    if (ls->show_error) {
        y += 8;
        int tw = text_width(ls->error_msg);
        draw_text(fb, card_x + (CARD_W - tw) / 2, y, ls->error_msg, ERROR_COLOR);
        y += sfh;
    }

    // Separator line
    y += 16;
    fb.fill_rect(x, y, content_w, 1, FIELD_BORDER);
    y += 16;

    // Power options (inside card, centered)
    {
        int total_w = POWER_ICON_SZ + POWER_GAP + POWER_ICON_SZ;
        int start_x = card_x + (CARD_W - total_w) / 2;

        // Shutdown icon
        if (ls->icon_shutdown.pixels) {
            int ix = start_x;
            fb.blit_alpha(ix, y, ls->icon_shutdown.width, ls->icon_shutdown.height,
                          ls->icon_shutdown.pixels);
            const char* lbl = "Shut Down";
            int lw = text_width(lbl);
            draw_text(fb, ix + (POWER_ICON_SZ - lw) / 2, y + POWER_ICON_SZ + 6,
                      lbl, LABEL_COLOR);
        }

        // Reboot icon
        if (ls->icon_reboot.pixels) {
            int ix = start_x + POWER_ICON_SZ + POWER_GAP;
            fb.blit_alpha(ix, y, ls->icon_reboot.width, ls->icon_reboot.height,
                          ls->icon_reboot.pixels);
            const char* lbl = "Restart";
            int lw = text_width(lbl);
            draw_text(fb, ix + (POWER_ICON_SZ - lw) / 2, y + POWER_ICON_SZ + 6,
                      lbl, LABEL_COLOR);
        }
    }

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

    // Write session so any app can query the current user
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

    int sfh = system_font_height();
    int power_section_h = POWER_ICON_SZ + sfh + 6 + 8;
    int error_section_h = ls->show_error ? sfh + 8 : 0;
    int card_h;
    if (ls->mode == MODE_FIRST_BOOT) {
        card_h = 420 + error_section_h + power_section_h;
    } else {
        card_h = 284 + error_section_h + power_section_h;
    }
    int card_x = (ls->screen_w - CARD_W) / 2;
    int card_y = (ls->screen_h - card_h) / 2;
    int x = card_x + 24;
    int content_w = CARD_W - 48;
    int y = card_y + 20;

    // Skip title
    y += sfh + 16;

    if (ls->mode == MODE_FIRST_BOOT) {
        // Username label + field
        y += sfh + LABEL_GAP;
        if (mx >= x && mx < x + content_w && my >= y && my < y + FIELD_H) {
            ls->active_field = 0;
            return;
        }
        y += FIELD_H + 12;

        // Display name label + field
        y += sfh + LABEL_GAP;
        if (mx >= x && mx < x + content_w && my >= y && my < y + FIELD_H) {
            ls->active_field = 1;
            return;
        }
        y += FIELD_H + 12;

        // Password label + field
        y += sfh + LABEL_GAP;
        if (mx >= x && mx < x + content_w && my >= y && my < y + FIELD_H) {
            ls->active_field = 2;
            return;
        }
        y += FIELD_H + 12;

        // Confirm label + field
        y += sfh + LABEL_GAP;
        if (mx >= x && mx < x + content_w && my >= y && my < y + FIELD_H) {
            ls->active_field = 3;
            return;
        }
        y += FIELD_H + 16;

        // Create button
        if (mx >= x && mx < x + content_w && my >= y && my < y + BTN_H) {
            if (try_first_boot_submit(ls)) {
                ls->mode = MODE_LOGIN;
                ls->active_field = 0;
                ls->password[0] = '\0'; ls->password_len = 0;
                ls->confirm[0] = '\0'; ls->confirm_len = 0;
                ls->show_error = false;
            }
            return;
        }
        y += BTN_H;
    } else {
        // Username label + field
        y += sfh + LABEL_GAP;
        if (mx >= x && mx < x + content_w && my >= y && my < y + FIELD_H) {
            ls->active_field = 0;
            return;
        }
        y += FIELD_H + 12;

        // Password label + field
        y += sfh + LABEL_GAP;
        if (mx >= x && mx < x + content_w && my >= y && my < y + FIELD_H) {
            ls->active_field = 1;
            return;
        }
        y += FIELD_H + 16;

        // Login button
        if (mx >= x && mx < x + content_w && my >= y && my < y + BTN_H) {
            if (try_login(ls)) {
                int pid = montauk::spawn("0:/os/desktop.elf", ls->username);
                if (pid >= 0) {
                    montauk::waitpid(pid);
                }
                montauk::user::clear_session();
                ls->password[0] = '\0'; ls->password_len = 0;
                ls->active_field = 1;
                ls->show_error = false;
            }
            return;
        }
        y += BTN_H;
    }

    // Skip error section if visible
    if (ls->show_error) y += error_section_h;

    // Power options (inside card, after separator)
    y += 16 + 1 + 16; // padding + separator + padding
    {
        int total_w = POWER_ICON_SZ + POWER_GAP + POWER_ICON_SZ;
        int start_x = card_x + (CARD_W - total_w) / 2;
        int hit_h = POWER_ICON_SZ + sfh + 6;

        // Shutdown
        if (mx >= start_x && mx < start_x + POWER_ICON_SZ &&
            my >= y && my < y + hit_h) {
            montauk::shutdown();
        }

        // Reboot
        int rx = start_x + POWER_ICON_SZ + POWER_GAP;
        if (mx >= rx && mx < rx + POWER_ICON_SZ &&
            my >= y && my < y + hit_h) {
            montauk::reset();
        }
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
    Color icon_color = Color::from_rgb(0x66, 0x66, 0x66);
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
    for (;;) {
        // Poll mouse
        ls->prev_buttons = ls->mouse.buttons;
        montauk::mouse_state(&ls->mouse);

        // Poll keyboard
        while (montauk::is_key_available()) {
            Montauk::KeyEvent key;
            montauk::getkey(&key);
            handle_key(ls, key);
        }

        // Handle mouse
        handle_mouse(ls);

        // Draw
        draw_login_screen(ls);
    }
}
