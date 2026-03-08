/*
 * render.cpp
 * MontaukOS Installer — rendering
 * Copyright (c) 2026 Daniel Hammer
 */

#include "installer.h"

// ============================================================================
// Pixel helpers
// ============================================================================

static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x,   y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

static void px_hline(uint32_t* px, int bw, int bh, int x, int y, int w, Color c) {
    if (y < 0 || y >= bh) return;
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > bw ? bw : x + w;
    for (int col = x0; col < x1; col++)
        px[y * bw + col] = v;
}

static void px_fill_rounded(uint32_t* px, int bw, int bh,
                             int x, int y, int w, int h, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            bool skip = false;
            int cx, cy;
            if (col < r && row < r) { cx = r - col - 1; cy = r - row - 1; if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col >= w - r && row < r) { cx = col - (w - r); cy = r - row - 1; if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col < r && row >= h - r) { cx = r - col - 1; cy = row - (h - r); if (cx*cx + cy*cy >= r*r) skip = true; }
            else if (col >= w - r && row >= h - r) { cx = col - (w - r); cy = row - (h - r); if (cx*cx + cy*cy >= r*r) skip = true; }
            if (!skip) px[dy * bw + dx] = v;
        }
    }
}

static void px_rect(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, Color c) {
    px_hline(px, bw, bh, x, y, w, c);
    px_hline(px, bw, bh, x, y + h - 1, w, c);
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= bh) continue;
        if (x >= 0 && x < bw) px[row * bw + x] = c.to_pixel();
        int rx = x + w - 1;
        if (rx >= 0 && rx < bw) px[row * bw + rx] = c.to_pixel();
    }
}

static void px_text(uint32_t* px, int bw, int bh,
                    int x, int y, const char* text, Color c, int size = FONT_SIZE) {
    if (g_font)
        g_font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}

static int text_w(const char* text, int size = FONT_SIZE) {
    return g_font ? g_font->measure_text(text, size) : 0;
}

static int font_h(int size = FONT_SIZE) {
    if (!g_font) return 16;
    auto* cache = g_font->get_cache(size);
    return cache->ascent - cache->descent;
}

static void px_stroke_rounded(uint32_t* px, int bw, int bh,
                              int x, int y, int w, int h, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            // Check if pixel is on the edge (within 1px of border)
            bool on_edge = (row == 0 || row == h - 1 || col == 0 || col == w - 1);
            // For corner regions, check the rounded boundary
            bool in_shape = true;
            int cx, cy;
            if (col < r && row < r) { cx = r - col - 1; cy = r - row - 1; in_shape = cx*cx + cy*cy < r*r; }
            else if (col >= w - r && row < r) { cx = col - (w - r); cy = r - row - 1; in_shape = cx*cx + cy*cy < r*r; }
            else if (col < r && row >= h - r) { cx = r - col - 1; cy = row - (h - r); in_shape = cx*cx + cy*cy < r*r; }
            else if (col >= w - r && row >= h - r) { cx = col - (w - r); cy = row - (h - r); in_shape = cx*cx + cy*cy < r*r; }
            if (!in_shape) continue;
            // Check if the pixel one step inward is still in shape
            bool inner = true;
            if (on_edge) { inner = false; }
            else {
                // Check neighbors — if any neighbor is outside, we're on the edge
                int offsets[][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                for (auto& o : offsets) {
                    int nc = col + o[0], nr = row + o[1];
                    if (nc < 0 || nc >= w || nr < 0 || nr >= h) { inner = false; break; }
                    bool nb_in = true;
                    if (nc < r && nr < r) { int a = r-nc-1, b = r-nr-1; nb_in = a*a+b*b < r*r; }
                    else if (nc >= w-r && nr < r) { int a = nc-(w-r), b = r-nr-1; nb_in = a*a+b*b < r*r; }
                    else if (nc < r && nr >= h-r) { int a = r-nc-1, b = nr-(h-r); nb_in = a*a+b*b < r*r; }
                    else if (nc >= w-r && nr >= h-r) { int a = nc-(w-r), b = nr-(h-r); nb_in = a*a+b*b < r*r; }
                    if (!nb_in) { inner = false; break; }
                }
            }
            if (!inner) px[dy * bw + dx] = v;
        }
    }
}

static void px_button(uint32_t* px, int bw, int bh,
                      int x, int y, int w, int h,
                      const char* label, Color bg, Color fg, int r) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    int tw = text_w(label);
    int fh = font_h();
    px_text(px, bw, bh, x + (w - tw) / 2, y + (h - fh) / 2, label, fg);
}

static void px_button_outline(uint32_t* px, int bw, int bh,
                              int x, int y, int w, int h,
                              const char* label, Color border, Color fg, int r) {
    px_stroke_rounded(px, bw, bh, x, y, w, h, r, border);
    int tw = text_w(label);
    int fh = font_h();
    px_text(px, bw, bh, x + (w - tw) / 2, y + (h - fh) / 2, label, fg);
}

// ============================================================================
// Render: disk selection step
// ============================================================================

static void render_select_disk(uint32_t* px) {
    auto& st = g_state;
    int fh = font_h();

    int y = CONTENT_TOP + 16;
    px_text(px, g_win_w, g_win_h, 16, y, "Select a target disk", TEXT_COLOR);
    y += fh + 4;
    px_text(px, g_win_w, g_win_h, 16, y, "MontaukOS will be installed to this disk.", DIM_TEXT, FONT_SM);
    y += font_h(FONT_SM) + 12;

    if (st.disk_count == 0) {
        px_text(px, g_win_w, g_win_h, 16, y, "No disks detected", FAINT_TEXT);
    }

    for (int i = 0; i < st.disk_count; i++) {
        int item_h = 48;
        int item_x = 16;
        int item_w = g_win_w - 32;
        bool sel = (i == st.selected_disk);

        Color bg = sel
            ? Color::from_rgb(0xD8, 0xE8, 0xF8)
            : Color::from_rgb(0xF4, 0xF4, 0xF4);

        px_fill_rounded(px, g_win_w, g_win_h, item_x, y, item_w, item_h, 6, bg);

        if (sel)
            px_fill_rounded(px, g_win_w, g_win_h, item_x, y, 4, item_h, 2, ACCENT);

        px_text(px, g_win_w, g_win_h, item_x + 12, y + 6, st.disks[i].model, TEXT_COLOR);

        char info[64], sz[24];
        format_disk_size(sz, sizeof(sz), st.disks[i].sectorCount, st.disks[i].sectorSizeLog);
        const char* dtype = st.disks[i].rpm == 1 ? "SSD" : "HDD";
        snprintf(info, sizeof(info), "%s  %s  (Disk %d)", sz, dtype, i);
        px_text(px, g_win_w, g_win_h, item_x + 12, y + 6 + fh + 2, info, DIM_TEXT, FONT_SM);

        y += item_h + 4;
    }

    // Buttons
    int btn_w = 120, btn_h = 34;
    int btn_y = g_win_h - STATUS_H - btn_h - 12;
    int next_x = g_win_w - btn_w - 16;

    Color next_bg = (st.selected_disk >= 0) ? ACCENT : DISABLED_BG;
    px_button(px, g_win_w, g_win_h, next_x, btn_y, btn_w, btn_h,
              "Next", next_bg, WHITE, TB_BTN_RAD);

    int ref_w = 80;
    int ref_x = next_x - ref_w - 8;
    px_button_outline(px, g_win_w, g_win_h, ref_x, btn_y, ref_w, btn_h,
              "Refresh", BORDER_COLOR, DIM_TEXT, TB_BTN_RAD);
}

// ============================================================================
// Render: partition scheme step
// ============================================================================

static void render_partition_scheme(uint32_t* px) {
    auto& st = g_state;
    int fh = font_h();
    int fh_sm = font_h(FONT_SM);

    int y = CONTENT_TOP + 16;
    px_text(px, g_win_w, g_win_h, 16, y, "Partition scheme", TEXT_COLOR);
    y += fh + 4;
    px_text(px, g_win_w, g_win_h, 16, y, "How should the disk be partitioned?", DIM_TEXT, FONT_SM);
    y += fh_sm + 12;

    for (int i = 0; i < SCHEME_COUNT; i++) {
        int item_h = 52;
        int item_x = 16;
        int item_w = g_win_w - 32;

        bool selected = (i == st.partition_scheme);
        Color bg = selected
            ? Color::from_rgb(0xD8, 0xE8, 0xF8)
            : Color::from_rgb(0xF4, 0xF4, 0xF4);

        px_fill_rounded(px, g_win_w, g_win_h, item_x, y, item_w, item_h, 6, bg);

        if (selected)
            px_fill_rounded(px, g_win_w, g_win_h, item_x, y, 4, item_h, 2, ACCENT);

        // Radio indicator
        int rx = item_x + 14;
        int ry = y + item_h / 2;
        px_fill_rounded(px, g_win_w, g_win_h, rx - 6, ry - 6, 12, 12, 6, DIM_TEXT);
        px_fill_rounded(px, g_win_w, g_win_h, rx - 5, ry - 5, 10, 10, 5, BG_COLOR);
        if (selected)
            px_fill_rounded(px, g_win_w, g_win_h, rx - 3, ry - 3, 6, 6, 3, ACCENT);

        px_text(px, g_win_w, g_win_h, item_x + 32, y + 8, scheme_names[i], TEXT_COLOR);
        px_text(px, g_win_w, g_win_h, item_x + 32, y + 8 + fh + 2, scheme_descs[i], DIM_TEXT, FONT_SM);

        y += item_h + 4;
    }

    // Buttons
    int btn_w = 120, btn_h = 34;
    int btn_y = g_win_h - STATUS_H - btn_h - 12;
    int next_x = g_win_w - btn_w - 16;

    px_button(px, g_win_w, g_win_h, next_x, btn_y, btn_w, btn_h,
              "Next", ACCENT, WHITE, TB_BTN_RAD);

    int back_x = next_x - btn_w - 8;
    px_button_outline(px, g_win_w, g_win_h, back_x, btn_y, btn_w, btn_h,
              "Back", BORDER_COLOR, DIM_TEXT, TB_BTN_RAD);
}

// ============================================================================
// Render: confirmation step
// ============================================================================

static void render_confirm(uint32_t* px) {
    auto& st = g_state;
    int fh = font_h();
    int y = CONTENT_TOP + 24;

    const char* title = "Confirm Installation";
    px_text(px, g_win_w, g_win_h, (g_win_w - text_w(title)) / 2, y, title, TEXT_COLOR);
    y += fh + 16;

    const char* warn1 = "This will ERASE ALL DATA on:";
    px_text(px, g_win_w, g_win_h, (g_win_w - text_w(warn1)) / 2, y, warn1, DANGER);
    y += fh + 8;

    char desc[128], sz[24];
    format_disk_size(sz, sizeof(sz), st.disks[st.selected_disk].sectorCount,
                     st.disks[st.selected_disk].sectorSizeLog);
    snprintf(desc, sizeof(desc), "Disk %d: %s (%s)",
             st.selected_disk, st.disks[st.selected_disk].model, sz);
    px_text(px, g_win_w, g_win_h, (g_win_w - text_w(desc)) / 2, y, desc, TEXT_COLOR);
    y += fh + 16;

    // Scheme summary
    const char* scheme_label = scheme_names[st.partition_scheme];
    px_text(px, g_win_w, g_win_h, (g_win_w - text_w(scheme_label, FONT_SM)) / 2, y,
            scheme_label, DIM_TEXT, FONT_SM);
    y += font_h(FONT_SM) + 4;

    const char* warn3 = "The entire root filesystem will be installed.";
    px_text(px, g_win_w, g_win_h, (g_win_w - text_w(warn3, FONT_SM)) / 2, y, warn3, DIM_TEXT, FONT_SM);

    // Buttons
    int btn_w = 120, btn_h = 34;
    int center_x = g_win_w / 2;
    int btn_y = g_win_h - STATUS_H - btn_h - 12;
    int gap = 16;

    px_button(px, g_win_w, g_win_h, center_x - btn_w - gap / 2, btn_y, btn_w, btn_h,
              "Install", DANGER, WHITE, TB_BTN_RAD);
    px_button_outline(px, g_win_w, g_win_h, center_x + gap / 2, btn_y, btn_w, btn_h,
              "Back", BORDER_COLOR, DIM_TEXT, TB_BTN_RAD);
}

// ============================================================================
// Render: installing / done / error steps
// ============================================================================

static void render_progress(uint32_t* px) {
    auto& st = g_state;
    int fh = font_h();
    int fh_sm = font_h(FONT_SM);
    int y = CONTENT_TOP + 16;

    const char* title;
    Color title_color;
    if (st.step == STEP_INSTALLING) {
        title = "Installing...";
        title_color = TEXT_COLOR;
    } else if (st.step == STEP_DONE) {
        title = "Installation Complete";
        title_color = SUCCESS_COLOR;
    } else {
        title = "Installation Failed";
        title_color = DANGER;
    }

    px_text(px, g_win_w, g_win_h, (g_win_w - text_w(title)) / 2, y, title, title_color);
    y += fh + 12;

    // Log area
    int log_x = 16;
    int log_w = g_win_w - 32;
    int log_h = st.log_count * (fh_sm + 2) + 16;
    int max_log_h = g_win_h - y - STATUS_H - 60;
    if (log_h < 60) log_h = 60;
    if (log_h > max_log_h) log_h = max_log_h;

    px_fill_rounded(px, g_win_w, g_win_h, log_x, y, log_w, log_h, 4,
                    Color::from_rgb(0xF4, 0xF4, 0xF4));

    int ly = y + 8;
    for (int i = 0; i < st.log_count; i++) {
        if (ly + fh_sm > y + log_h - 4) break;
        px_text(px, g_win_w, g_win_h, log_x + 8, ly, st.log[i], TEXT_COLOR, FONT_SM);
        ly += fh_sm + 2;
    }

    if (st.step == STEP_DONE || st.step == STEP_ERROR) {
        int btn_w = 100, btn_h = 34;
        int btn_x = (g_win_w - btn_w) / 2;
        int btn_y = g_win_h - STATUS_H - btn_h - 12;
        if (st.step == STEP_DONE)
            px_button(px, g_win_w, g_win_h, btn_x, btn_y, btn_w, btn_h,
                      "Close", ACCENT, WHITE, TB_BTN_RAD);
        else
            px_button_outline(px, g_win_w, g_win_h, btn_x, btn_y, btn_w, btn_h,
                      "Close", BORDER_COLOR, DIM_TEXT, TB_BTN_RAD);
    }
}

// ============================================================================
// Render: toolbar, step bar, and status bar
// ============================================================================

static void render_toolbar(uint32_t* px) {
    px_fill(px, g_win_w, g_win_h, 0, 0, g_win_w, TOOLBAR_H, TOOLBAR_BG);
    px_hline(px, g_win_w, g_win_h, 0, TOOLBAR_H - 1, g_win_w, BORDER_COLOR);

    const char* title = "MontaukOS Installer";
    int fh = font_h();
    px_text(px, g_win_w, g_win_h, 12, (TOOLBAR_H - fh) / 2, title, TEXT_COLOR);
}

static void render_step_bar(uint32_t* px) {
    int bar_y = TOOLBAR_H;
    px_fill(px, g_win_w, g_win_h, 0, bar_y, g_win_w, STEP_BAR_H,
            Color::from_rgb(0xFA, 0xFA, 0xFA));
    px_hline(px, g_win_w, g_win_h, 0, bar_y + STEP_BAR_H - 1, g_win_w, BORDER_COLOR);

    static const char* step_labels[] = { "Disk", "Partition", "Confirm", "Install" };
    static constexpr int STEP_COUNT = 4;

    // Map current state to step index (0-3)
    int cur = 0;
    if (g_state.step == STEP_PARTITION_SCHEME) cur = 1;
    else if (g_state.step == STEP_CONFIRM) cur = 2;
    else if (g_state.step >= STEP_INSTALLING) cur = 3;

    int fh = font_h();

    // Measure total width to center the bar
    int total_w = 0;
    int label_ws[STEP_COUNT];
    int arrow_w = text_w(">");
    for (int i = 0; i < STEP_COUNT; i++) {
        label_ws[i] = text_w(step_labels[i]);
        total_w += label_ws[i];
        if (i < STEP_COUNT - 1) total_w += arrow_w + 16; // padding around arrow
    }

    int x = (g_win_w - total_w) / 2;
    int ty = bar_y + (STEP_BAR_H - fh) / 2;

    for (int i = 0; i < STEP_COUNT; i++) {
        Color col;
        if (i == cur)      col = ACCENT;
        else if (i < cur)  col = Color::from_rgb(0x88, 0xAA, 0xDD);
        else               col = Color::from_rgb(0xAA, 0xAA, 0xAA);

        px_text(px, g_win_w, g_win_h, x, ty, step_labels[i], col);
        x += label_ws[i];

        if (i < STEP_COUNT - 1) {
            x += 8;
            px_text(px, g_win_w, g_win_h, x, ty, ">", Color::from_rgb(0xCC, 0xCC, 0xCC));
            x += arrow_w + 8;
        }
    }
}

static void render_status(uint32_t* px) {
    auto& st = g_state;
    int fh = font_h();
    int sy = g_win_h - STATUS_H;
    px_fill(px, g_win_w, g_win_h, 0, sy, g_win_w, STATUS_H, Color::from_rgb(0xF0, 0xF0, 0xF0));
    px_hline(px, g_win_w, g_win_h, 0, sy, g_win_w, BORDER_COLOR);
    if (st.status[0]) {
        uint64_t age = montauk::get_milliseconds() - st.status_time;
        Color sc = (age < 5000)
            ? Color::from_rgb(0x33, 0x33, 0x33)
            : Color::from_rgb(0xAA, 0xAA, 0xAA);
        px_text(px, g_win_w, g_win_h, 8, sy + (STATUS_H - fh) / 2, st.status, sc);
    }
}

// ============================================================================
// Top-level render
// ============================================================================

void render(uint32_t* pixels) {
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, g_win_h, BG_COLOR);
    render_toolbar(pixels);
    render_step_bar(pixels);

    switch (g_state.step) {
        case STEP_SELECT_DISK:      render_select_disk(pixels); break;
        case STEP_PARTITION_SCHEME: render_partition_scheme(pixels); break;
        case STEP_CONFIRM:          render_confirm(pixels); break;
        case STEP_INSTALLING:
        case STEP_DONE:
        case STEP_ERROR:            render_progress(pixels); break;
    }

    render_status(pixels);
}
