/*
 * render.cpp
 * MontaukOS Disk Tool — rendering
 * Copyright (c) 2026 Daniel Hammer
 */

#include "disks.h"

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
                    int x, int y, const char* text, Color c) {
    if (g_font)
        g_font->draw_to_buffer(px, bw, bh, x, y, text, c, FONT_SIZE);
}

static int text_w(const char* text) {
    return g_font ? g_font->measure_text(text, FONT_SIZE) : 0;
}

static int font_h() {
    if (!g_font) return 16;
    auto* cache = g_font->get_cache(FONT_SIZE);
    return cache->ascent - cache->descent;
}

// ============================================================================
// Button helper
// ============================================================================

static void px_button(uint32_t* px, int bw, int bh,
                      int x, int y, int w, int h,
                      const char* label, Color bg, Color fg, int r) {
    px_fill_rounded(px, bw, bh, x, y, w, h, r, bg);
    int tw = text_w(label);
    int fh = font_h();
    px_text(px, bw, bh, x + (w - tw) / 2, y + (h - fh) / 2, label, fg);
}

// ============================================================================
// Render: main view
// ============================================================================

static void render_toolbar(uint32_t* px) {
    auto& dt = g_state;
    int fh = font_h();

    px_fill(px, g_win_w, g_win_h, 0, 0, g_win_w, TOOLBAR_H, TOOLBAR_BG);
    px_hline(px, g_win_w, g_win_h, 0, TOOLBAR_H - 1, g_win_w, BORDER_COLOR);

    int bx = 8;
    for (int i = 0; i < dt.disk_count; i++) {
        char label[8];
        snprintf(label, sizeof(label), "Disk %d", i);
        int lw = text_w(label) + 16;
        Color bg = (i == dt.selected_disk)
            ? Color::from_rgb(0x42, 0x7A, 0xB5)
            : Color::from_rgb(0xDD, 0xDD, 0xDD);
        Color fg = (i == dt.selected_disk) ? WHITE : TEXT_COLOR;
        px_button(px, g_win_w, g_win_h, bx, TB_BTN_Y, lw, TB_BTN_H, label, bg, fg, TB_BTN_RAD);
        bx += lw + 6;
    }

    if (dt.disk_count == 0) {
        px_text(px, g_win_w, g_win_h, 8, (TOOLBAR_H - fh) / 2, "No disks detected", TEXT_COLOR);
        return;
    }

    // Right-side action buttons
    bool has_sel = dt.selected_part >= 0;
    int rx = g_win_w - 8;

    int ref_w = 64; rx -= ref_w;
    px_button(px, g_win_w, g_win_h, rx, TB_BTN_Y, ref_w, TB_BTN_H,
              "Refresh", Color::from_rgb(0xE0, 0xE0, 0xE0), TEXT_COLOR, TB_BTN_RAD);
    rx -= 6;

    int mnt_w = 60; rx -= mnt_w;
    px_button(px, g_win_w, g_win_h, rx, TB_BTN_Y, mnt_w, TB_BTN_H,
              "Mount",
              has_sel ? Color::from_rgb(0x42, 0x7A, 0xB5) : Color::from_rgb(0xBB, 0xBB, 0xBB),
              WHITE, TB_BTN_RAD);
    rx -= 6;

    int fmt_w = 64; rx -= fmt_w;
    px_button(px, g_win_w, g_win_h, rx, TB_BTN_Y, fmt_w, TB_BTN_H,
              "Format",
              has_sel ? Color::from_rgb(0x42, 0x7A, 0xB5) : Color::from_rgb(0xBB, 0xBB, 0xBB),
              WHITE, TB_BTN_RAD);
    rx -= 6;

    int np_w = 74; rx -= np_w;
    px_button(px, g_win_w, g_win_h, rx, TB_BTN_Y, np_w, TB_BTN_H,
              "New Part", Color::from_rgb(0x42, 0x7A, 0xB5), WHITE, TB_BTN_RAD);
}

static void render_content(uint32_t* px) {
    auto& dt = g_state;
    int fh = font_h();

    if (dt.disk_count == 0 || dt.selected_disk < 0 || dt.selected_disk >= dt.disk_count)
        return;

    Montauk::DiskInfo& disk = dt.disks[dt.selected_disk];
    int y = TOOLBAR_H + 8;

    // Disk model name
    px_text(px, g_win_w, g_win_h, MAP_PAD, y, disk.model, TEXT_COLOR);

    // Size and type on the right
    char info_str[64], size_str[24];
    format_disk_size(size_str, sizeof(size_str), disk.sectorCount, disk.sectorSizeLog);
    const char* dtype = disk.rpm == 1 ? "SSD" : "HDD";
    snprintf(info_str, sizeof(info_str), "%s  %s", size_str, dtype);
    int iw = text_w(info_str);
    px_text(px, g_win_w, g_win_h, g_win_w - iw - MAP_PAD, y, info_str, DIM_TEXT);

    y += fh + 8;

    // Partition map bar
    int map_x = MAP_PAD;
    int map_w = g_win_w - MAP_PAD * 2;
    px_fill_rounded(px, g_win_w, g_win_h, map_x, y, map_w, MAP_H, 6,
                    Color::from_rgb(0xE4, 0xE4, 0xE4));

    int part_indices[MAX_PARTS];
    int nparts = get_disk_parts(part_indices, MAX_PARTS);
    uint64_t total_sectors = disk.sectorCount;

    if (total_sectors > 0 && nparts > 0) {
        for (int pi = 0; pi < nparts; pi++) {
            Montauk::PartInfo& p = dt.parts[part_indices[pi]];
            int ppx = map_x + (int)((p.startLba * (uint64_t)map_w) / total_sectors);
            int pw = (int)((p.sectorCount * (uint64_t)map_w) / total_sectors);
            if (pw < 2) pw = 2;
            if (ppx + pw > map_x + map_w) pw = map_x + map_w - ppx;

            Color col = part_colors[pi % NUM_PART_COLORS];
            if (pi == dt.selected_part) {
                col = Color::from_rgb((col.r + 255) / 2, (col.g + 255) / 2, (col.b + 255) / 2);
            }
            px_fill_rounded(px, g_win_w, g_win_h, ppx, y + 2, pw, MAP_H - 4, 3, col);

            char plabel[8];
            snprintf(plabel, sizeof(plabel), "%d", pi);
            int plw = text_w(plabel);
            if (pw > plw + 4)
                px_text(px, g_win_w, g_win_h, ppx + (pw - plw) / 2, y + (MAP_H - fh) / 2, plabel, WHITE);
        }
    }

    y += MAP_H + 8;

    // Column header
    px_fill(px, g_win_w, g_win_h, 0, y, g_win_w, HEADER_H, HEADER_BG);
    int ty = y + (HEADER_H - fh) / 2;

    int col_idx  = 12;
    int col_name = 40;
    int col_type = g_win_w / 2 - 20;
    int col_size = g_win_w - 160;
    int col_lba  = g_win_w - 80;

    px_text(px, g_win_w, g_win_h, col_idx, ty, "#", DIM_TEXT);
    px_text(px, g_win_w, g_win_h, col_name, ty, "Name", DIM_TEXT);
    px_text(px, g_win_w, g_win_h, col_type, ty, "Type", DIM_TEXT);
    px_text(px, g_win_w, g_win_h, col_size, ty, "Size", DIM_TEXT);
    px_text(px, g_win_w, g_win_h, col_lba, ty, "LBA", DIM_TEXT);
    px_hline(px, g_win_w, g_win_h, 0, y + HEADER_H - 1, g_win_w, BORDER_COLOR);
    y += HEADER_H;

    // Partition rows
    int list_bottom = g_win_h - STATUS_H;
    int list_y = y;
    for (int pi = 0; pi < nparts; pi++) {
        int row_y = list_y + pi * ITEM_H - dt.scroll_y;
        if (row_y + ITEM_H <= list_y) continue;
        if (row_y >= list_bottom) break;

        Montauk::PartInfo& p = dt.parts[part_indices[pi]];

        if (pi == dt.selected_part)
            px_fill(px, g_win_w, g_win_h, 0, row_y, g_win_w, ITEM_H, HOVER_BG);

        px_fill_rounded(px, g_win_w, g_win_h, col_idx, row_y + (ITEM_H - 10) / 2, 10, 10, 5,
                        part_colors[pi % NUM_PART_COLORS]);

        int ry = row_y + (ITEM_H - fh) / 2;
        px_text(px, g_win_w, g_win_h, col_name, ry, p.name[0] ? p.name : "(unnamed)", TEXT_COLOR);
        px_text(px, g_win_w, g_win_h, col_type, ry, p.typeName, DIM_TEXT);

        char sz[24];
        format_disk_size(sz, sizeof(sz), p.sectorCount, disk.sectorSizeLog);
        px_text(px, g_win_w, g_win_h, col_size, ry, sz, TEXT_COLOR);

        char lba_str[24];
        snprintf(lba_str, sizeof(lba_str), "%lu", (unsigned)p.startLba);
        px_text(px, g_win_w, g_win_h, col_lba, ry, lba_str, FAINT_TEXT);
    }

    if (nparts == 0)
        px_text(px, g_win_w, g_win_h, col_name, list_y + 8, "No partitions found", FAINT_TEXT);
}

static void render_status(uint32_t* px) {
    auto& dt = g_state;
    int fh = font_h();

    int sy = g_win_h - STATUS_H;
    px_fill(px, g_win_w, g_win_h, 0, sy, g_win_w, STATUS_H, STATUS_BG_COL);
    px_hline(px, g_win_w, g_win_h, 0, sy, g_win_w, BORDER_COLOR);
    if (dt.status[0]) {
        uint64_t age = montauk::get_milliseconds() - dt.status_time;
        Color sc = (age < 5000)
            ? Color::from_rgb(0x33, 0x33, 0x33)
            : Color::from_rgb(0xAA, 0xAA, 0xAA);
        px_text(px, g_win_w, g_win_h, 8, sy + (STATUS_H - fh) / 2, dt.status, sc);
    }
}

// ============================================================================
// Render: format dialog window
// ============================================================================

void render_format_window() {
    auto& dlg = g_state.fmt_dlg;
    if (!dlg.open) return;

    uint32_t* px = dlg.pixels;
    int dw = FMT_DLG_W, dh = FMT_DLG_H;
    int fh = font_h();

    px_fill(px, dw, dh, 0, 0, dw, dh, BG_COLOR);

    // Title
    const char* title = "Format Partition";
    int tw = text_w(title);
    px_text(px, dw, dh, (dw - tw) / 2, 12, title, TEXT_COLOR);

    // Partition description
    int ddw = text_w(dlg.part_desc);
    px_text(px, dw, dh, (dw - ddw) / 2, 12 + fh + 6, dlg.part_desc, DIM_TEXT);

    // Filesystem selector
    int sel_y = 12 + fh * 2 + 18;
    px_text(px, dw, dh, 16, sel_y, "Filesystem:", TEXT_COLOR);

    int opt_y = sel_y + fh + 8;
    for (int i = 0; i < NUM_FS_TYPES; i++) {
        int ox = 24;
        int ow = dw - 48;
        int oh = 28;
        int iy = opt_y + i * (oh + 4);

        if (i == dlg.selected_fs) {
            px_fill_rounded(px, dw, dh, ox, iy, ow, oh, 6,
                            Color::from_rgb(0xD0, 0xE0, 0xF0));
            px_fill_rounded(px, dw, dh, ox - 1, iy - 1, ow + 2, oh + 2, 7, Color::from_rgb(0x42, 0x7A, 0xB5));
            px_fill_rounded(px, dw, dh, ox, iy, ow, oh, 6, Color::from_rgb(0xD0, 0xE0, 0xF0));
        } else {
            px_fill_rounded(px, dw, dh, ox - 1, iy - 1, ow + 2, oh + 2, 7, BORDER_COLOR);
            px_fill_rounded(px, dw, dh, ox, iy, ow, oh, 6, BG_COLOR);
        }

        // Radio indicator (circular)
        int cx = ox + 14;
        int cy = iy + oh / 2;
        px_fill_rounded(px, dw, dh, cx - 5, cy - 5, 10, 10, 5, DIM_TEXT);
        px_fill_rounded(px, dw, dh, cx - 4, cy - 4, 8, 8, 4, BG_COLOR);
        if (i == dlg.selected_fs)
            px_fill_rounded(px, dw, dh, cx - 3, cy - 3, 6, 6, 3, Color::from_rgb(0x42, 0x7A, 0xB5));

        px_text(px, dw, dh, cx + 12, iy + (oh - fh) / 2, g_fsTypes[i].name, TEXT_COLOR);
    }

    // Buttons
    int btn_w = 90, btn_h = 30;
    int btn_y = dh - btn_h - 16;
    int gap = 16;
    int total_w = btn_w * 2 + gap;
    int bx = (dw - total_w) / 2;

    Color fmt_bg = dlg.hover_format
        ? Color::from_rgb(0xDD, 0x44, 0x44)
        : Color::from_rgb(0xCC, 0x33, 0x33);
    px_button(px, dw, dh, bx, btn_y, btn_w, btn_h, "Format", fmt_bg, WHITE, 6);

    Color can_bg = dlg.hover_cancel
        ? Color::from_rgb(0x99, 0x99, 0x99)
        : Color::from_rgb(0x88, 0x88, 0x88);
    px_button(px, dw, dh, bx + btn_w + gap, btn_y, btn_w, btn_h, "Cancel", can_bg, WHITE, 6);
}

// ============================================================================
// Render: new partition confirmation dialog
// ============================================================================

void render_newpart_window() {
    auto& dlg = g_state.np_dlg;
    if (!dlg.open) return;

    uint32_t* px = dlg.pixels;
    int dw = NP_DLG_W, dh = NP_DLG_H;
    int fh = font_h();

    px_fill(px, dw, dh, 0, 0, dw, dh, BG_COLOR);

    // Title
    const char* title = dlg.will_init_gpt ? "Initialize Disk" : "New Partition";
    int tw = text_w(title);
    px_text(px, dw, dh, (dw - tw) / 2, 12, title, TEXT_COLOR);

    // Disk description
    int ddw = text_w(dlg.disk_desc);
    px_text(px, dw, dh, (dw - ddw) / 2, 12 + fh + 6, dlg.disk_desc, DIM_TEXT);

    // Warning lines
    Color warn_color = dlg.will_init_gpt
        ? Color::from_rgb(0xCC, 0x22, 0x22)
        : TEXT_COLOR;

    int wy = 12 + fh * 2 + 20;
    int w1w = text_w(dlg.warn_line1);
    px_text(px, dw, dh, (dw - w1w) / 2, wy, dlg.warn_line1, warn_color);
    int w2w = text_w(dlg.warn_line2);
    px_text(px, dw, dh, (dw - w2w) / 2, wy + fh + 4, dlg.warn_line2, warn_color);

    // Buttons
    int btn_w = 90, btn_h = 30;
    int btn_y = dh - btn_h - 16;
    int gap = 16;
    int total_w = btn_w * 2 + gap;
    int bx = (dw - total_w) / 2;

    Color confirm_bg = dlg.will_init_gpt
        ? (dlg.hover_confirm ? Color::from_rgb(0xDD, 0x44, 0x44)
                             : Color::from_rgb(0xCC, 0x33, 0x33))
        : (dlg.hover_confirm ? Color::from_rgb(0x4A, 0x88, 0xC8)
                             : Color::from_rgb(0x42, 0x7A, 0xB5));
    const char* confirm_label = dlg.will_init_gpt ? "Initialize" : "Create";
    px_button(px, dw, dh, bx, btn_y, btn_w, btn_h, confirm_label, confirm_bg, WHITE, 6);

    Color can_bg = dlg.hover_cancel
        ? Color::from_rgb(0x99, 0x99, 0x99)
        : Color::from_rgb(0x88, 0x88, 0x88);
    px_button(px, dw, dh, bx + btn_w + gap, btn_y, btn_w, btn_h, "Cancel", can_bg, WHITE, 6);
}

// ============================================================================
// Top-level render
// ============================================================================

void render(uint32_t* pixels) {
    px_fill(pixels, g_win_w, g_win_h, 0, 0, g_win_w, g_win_h, BG_COLOR);
    render_toolbar(pixels);
    render_content(pixels);
    render_status(pixels);
}
