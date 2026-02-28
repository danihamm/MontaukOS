/*
    * app_devexplorer.cpp
    * MontaukOS Desktop - Device Explorer (lists hardware detected by the kernel)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "apps_common.hpp"

// ============================================================================
// Device Explorer state
// ============================================================================

static constexpr int DE_TOOLBAR_H = 36;
static constexpr int DE_CAT_H    = 28;
static constexpr int DE_ITEM_H   = 24;
static constexpr int DE_MAX_DEVS = 64;
static constexpr int DE_POLL_MS  = 2000;
static constexpr int DE_INDENT   = 28;

// Category names matching DevInfo.category values
static const char* category_names[] = {
    "CPU",          // 0
    "Interrupt",    // 1
    "Timer",        // 2
    "Input",        // 3
    "USB",          // 4
    "Network",      // 5
    "Display",      // 6
    "PCI",          // 7
};
static constexpr int NUM_CATEGORIES = 8;

static Color category_colors[] = {
    Color::from_rgb(0x33, 0x66, 0xCC),  // CPU - blue
    Color::from_rgb(0x88, 0x44, 0xAA),  // Interrupt - purple
    Color::from_rgb(0x22, 0x88, 0x22),  // Timer - green
    Color::from_rgb(0xCC, 0x88, 0x00),  // Input - amber
    Color::from_rgb(0x00, 0x88, 0x88),  // USB - teal
    Color::from_rgb(0xCC, 0x55, 0x22),  // Network - orange
    Color::from_rgb(0x44, 0x66, 0xCC),  // Display - indigo
    Color::from_rgb(0x66, 0x66, 0x66),  // PCI - gray
};

struct DevExplorerState {
    DesktopState* desktop;
    Montauk::DevInfo devs[DE_MAX_DEVS];
    int dev_count;
    bool collapsed[NUM_CATEGORIES]; // per-category collapse state
    int selected_row;               // index into visible display rows (-1 = none)
    int scroll_y;                   // scroll offset in display rows
    uint64_t last_poll_ms;
};

// ============================================================================
// Display row model
// ============================================================================

enum RowType { ROW_CATEGORY, ROW_DEVICE };

struct DisplayRow {
    RowType type;
    int category;   // category index (0..7)
    int dev_index;  // index into devs[] (only valid for ROW_DEVICE)
};

static constexpr int MAX_DISPLAY_ROWS = DE_MAX_DEVS + NUM_CATEGORIES;

static int build_display_rows(DevExplorerState* de, DisplayRow* rows) {
    int count = 0;
    for (int cat = 0; cat < NUM_CATEGORIES; cat++) {
        // Count devices in this category
        int cat_count = 0;
        for (int d = 0; d < de->dev_count; d++) {
            if (de->devs[d].category == cat) cat_count++;
        }
        if (cat_count == 0) continue;

        // Emit category header
        rows[count].type = ROW_CATEGORY;
        rows[count].category = cat;
        rows[count].dev_index = -1;
        count++;

        // Emit device rows if expanded
        if (!de->collapsed[cat]) {
            for (int d = 0; d < de->dev_count; d++) {
                if (de->devs[d].category == cat) {
                    rows[count].type = ROW_DEVICE;
                    rows[count].category = cat;
                    rows[count].dev_index = d;
                    count++;
                }
            }
        }
    }
    return count;
}

// ============================================================================
// Triangle drawing helpers
// ============================================================================

static void draw_triangle_right(Canvas& c, int x, int y, int size, Color col) {
    // Right-pointing filled triangle (▶)
    int half = size / 2;
    for (int row = 0; row < size; row++) {
        int dist = (row <= half) ? row : (size - 1 - row);
        for (int col_px = 0; col_px <= dist; col_px++) {
            c.put_pixel(x + col_px, y + row, col);
        }
    }
}

static void draw_triangle_down(Canvas& c, int x, int y, int size, Color col) {
    // Down-pointing filled triangle (▼)
    int half = size / 2;
    for (int row = 0; row < half + 1; row++) {
        int w = size - row * 2;
        for (int col_px = 0; col_px < w; col_px++) {
            c.put_pixel(x + row + col_px, y + row, col);
        }
    }
}

// ============================================================================
// Callbacks
// ============================================================================

static void devexplorer_on_poll(Window* win) {
    DevExplorerState* de = (DevExplorerState*)win->app_data;
    if (!de) return;

    uint64_t now = montauk::get_milliseconds();
    if (now - de->last_poll_ms < DE_POLL_MS) return;
    de->last_poll_ms = now;

    de->dev_count = montauk::devlist(de->devs, DE_MAX_DEVS);
}

static void devexplorer_on_draw(Window* win, Framebuffer& fb) {
    DevExplorerState* de = (DevExplorerState*)win->app_data;
    if (!de) return;

    Canvas c(win);
    c.fill(colors::WINDOW_BG);

    int fh = system_font_height();

    // --- Toolbar ---
    c.fill_rect(0, 0, c.w, DE_TOOLBAR_H, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.hline(0, DE_TOOLBAR_H - 1, c.w, colors::BORDER);

    // "Refresh" button
    int btn_w = 80;
    int btn_h = 26;
    int btn_x = 8;
    int btn_y = (DE_TOOLBAR_H - btn_h) / 2;
    c.button(btn_x, btn_y, btn_w, btn_h, "Refresh",
             Color::from_rgb(0x33, 0x66, 0xCC), colors::WHITE, 4);

    // Device count on right
    char count_str[24];
    snprintf(count_str, sizeof(count_str), "%d devices", de->dev_count);
    int cw = text_width(count_str);
    c.text(c.w - cw - 12, (DE_TOOLBAR_H - fh) / 2, count_str, colors::TEXT_COLOR);

    // --- Build display rows ---
    DisplayRow rows[MAX_DISPLAY_ROWS];
    int row_count = build_display_rows(de, rows);

    // --- Compute visible area ---
    int list_y = DE_TOOLBAR_H;
    int list_h = c.h - list_y;
    if (list_h < 1) return;

    // Clamp scroll
    // Calculate total content height
    int total_h = 0;
    for (int i = 0; i < row_count; i++) {
        total_h += (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
    }
    int max_scroll_px = total_h - list_h;
    if (max_scroll_px < 0) max_scroll_px = 0;

    // Convert scroll_y (in rows) to pixel offset for simplicity,
    // but keep the row-based model: scroll_y = row offset
    // We'll compute cumulative pixel offsets
    int scroll_px = 0;
    for (int i = 0; i < de->scroll_y && i < row_count; i++) {
        scroll_px += (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
    }
    if (scroll_px > max_scroll_px) {
        scroll_px = max_scroll_px;
        // Recalculate scroll_y to match
        de->scroll_y = 0;
        int acc = 0;
        for (int i = 0; i < row_count; i++) {
            int rh = (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
            if (acc + rh > max_scroll_px) break;
            acc += rh;
            de->scroll_y = i + 1;
        }
    }
    if (de->scroll_y < 0) de->scroll_y = 0;

    // Clamp selected_row
    if (de->selected_row >= row_count) de->selected_row = row_count - 1;

    // --- Draw rows ---
    int draw_y = list_y - scroll_px;
    // Advance to first visible area by recomputing from scroll offset
    draw_y = list_y;
    int first_visible = de->scroll_y;

    // Compute starting y by accumulating heights of skipped rows
    // Actually, let's just iterate all rows and skip those above the viewport
    draw_y = list_y;
    for (int i = 0; i < first_visible && i < row_count; i++) {
        draw_y -= (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
    }

    // Draw from first_visible onward
    int cur_y = list_y;
    for (int i = first_visible; i < row_count; i++) {
        int row_h = (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
        if (cur_y >= c.h) break; // Past bottom of window

        if (rows[i].type == ROW_CATEGORY) {
            // Category header
            int cat = rows[i].category;
            c.fill_rect(0, cur_y, c.w, DE_CAT_H, Color::from_rgb(0xF0, 0xF0, 0xF0));

            // Highlight if selected
            if (i == de->selected_row) {
                c.fill_rect(0, cur_y, c.w, DE_CAT_H, colors::MENU_HOVER);
            }

            // Expand/collapse triangle
            int tri_x = 10;
            int tri_y = cur_y + (DE_CAT_H - 8) / 2;
            Color tri_color = Color::from_rgb(0x55, 0x55, 0x55);
            if (de->collapsed[cat]) {
                draw_triangle_right(c, tri_x, tri_y, 8, tri_color);
            } else {
                draw_triangle_down(c, tri_x, tri_y, 8, tri_color);
            }

            // Colored dot
            int dot_x = 24;
            int dot_y = cur_y + (DE_CAT_H - 8) / 2;
            Color cat_col = (cat >= 0 && cat < NUM_CATEGORIES)
                            ? category_colors[cat] : colors::TEXT_COLOR;
            c.fill_rounded_rect(dot_x, dot_y, 8, 8, 4, cat_col);

            // Category name
            const char* cat_name = (cat >= 0 && cat < NUM_CATEGORIES)
                                   ? category_names[cat] : "?";
            int text_y = cur_y + (DE_CAT_H - fh) / 2;
            c.text(36, text_y, cat_name, Color::from_rgb(0x33, 0x33, 0x33));

            // Device count in parentheses on right
            int cat_count = 0;
            for (int d = 0; d < de->dev_count; d++) {
                if (de->devs[d].category == cat) cat_count++;
            }
            char cnt_buf[16];
            snprintf(cnt_buf, sizeof(cnt_buf), "(%d)", cat_count);
            int name_w = text_width(cat_name);
            c.text(36 + name_w + 8, text_y, cnt_buf,
                   Color::from_rgb(0x88, 0x88, 0x88));

            // Bottom border
            c.hline(0, cur_y + DE_CAT_H - 1, c.w, Color::from_rgb(0xE0, 0xE0, 0xE0));
        } else {
            // Device item row
            int di = rows[i].dev_index;

            // Selected row highlight
            if (i == de->selected_row) {
                c.fill_rect(0, cur_y, c.w, DE_ITEM_H, colors::MENU_HOVER);
            }

            int text_y = cur_y + (DE_ITEM_H - fh) / 2;

            // Device name (indented)
            c.text(DE_INDENT + 10, text_y, de->devs[di].name, colors::TEXT_COLOR);

            // Detail string (right column, gray)
            int detail_x = c.w / 2 + 20;
            c.text(detail_x, text_y, de->devs[di].detail,
                   Color::from_rgb(0x66, 0x66, 0x66));
        }

        cur_y += row_h;
    }

    // --- Scrollbar ---
    if (total_h > list_h) {
        int sb_x = c.w - 6;
        int thumb_h = (list_h * list_h) / total_h;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = list_y + (scroll_px * (list_h - thumb_h)) / max_scroll_px;
        c.fill_rect(sb_x, list_y, 4, list_h, Color::from_rgb(0xE0, 0xE0, 0xE0));
        c.fill_rect(sb_x, thumb_y, 4, thumb_h, Color::from_rgb(0xAA, 0xAA, 0xAA));
    }
}

static void devexplorer_on_mouse(Window* win, MouseEvent& ev) {
    DevExplorerState* de = (DevExplorerState*)win->app_data;
    if (!de) return;

    Rect cr = win->content_rect();
    int lx = ev.x - cr.x;
    int ly = ev.y - cr.y;

    // Scroll
    if (ev.scroll != 0) {
        de->scroll_y += ev.scroll;
        if (de->scroll_y < 0) de->scroll_y = 0;
        return;
    }

    if (ev.left_pressed()) {
        // Check "Refresh" button click
        int btn_w = 80;
        int btn_h = 26;
        int btn_x = 8;
        int btn_y = (DE_TOOLBAR_H - btn_h) / 2;
        Rect btn_rect = {btn_x, btn_y, btn_w, btn_h};
        if (btn_rect.contains(lx, ly)) {
            de->last_poll_ms = 0; // force refresh
            return;
        }

        // Check row click in list area
        int list_y = DE_TOOLBAR_H;
        if (ly >= list_y) {
            // Build display rows to map click to row
            DisplayRow rows[MAX_DISPLAY_ROWS];
            int row_count = build_display_rows(de, rows);

            // Walk rows from scroll offset, accumulating y
            int cur_y = list_y;
            for (int i = de->scroll_y; i < row_count; i++) {
                int row_h = (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
                if (ly >= cur_y && ly < cur_y + row_h) {
                    // Hit this row
                    if (rows[i].type == ROW_CATEGORY) {
                        // Toggle collapse
                        int cat = rows[i].category;
                        de->collapsed[cat] = !de->collapsed[cat];
                        de->selected_row = -1;
                    } else {
                        de->selected_row = i;
                    }
                    return;
                }
                cur_y += row_h;
                if (cur_y >= win->content_h) break;
            }
            // Clicked below all rows
            de->selected_row = -1;
        }
    }
}

static void devexplorer_on_key(Window* win, const Montauk::KeyEvent& key) {
    DevExplorerState* de = (DevExplorerState*)win->app_data;
    if (!de || !key.pressed) return;

    DisplayRow rows[MAX_DISPLAY_ROWS];
    int row_count = build_display_rows(de, rows);
    if (row_count == 0) return;

    if (key.scancode == 0x48) { // Up arrow
        if (de->selected_row <= 0) {
            de->selected_row = 0;
        } else {
            de->selected_row--;
        }
        // Scroll to keep selection visible
        if (de->selected_row < de->scroll_y) {
            de->scroll_y = de->selected_row;
        }
    } else if (key.scancode == 0x50) { // Down arrow
        if (de->selected_row < row_count - 1) {
            de->selected_row++;
        }
        // Scroll to keep selection visible — estimate visible rows
        int list_h = win->content_h - DE_TOOLBAR_H;
        int cur_h = 0;
        int last_visible = de->scroll_y;
        for (int i = de->scroll_y; i < row_count; i++) {
            int rh = (rows[i].type == ROW_CATEGORY) ? DE_CAT_H : DE_ITEM_H;
            if (cur_h + rh > list_h) break;
            cur_h += rh;
            last_visible = i;
        }
        if (de->selected_row > last_visible) {
            de->scroll_y += (de->selected_row - last_visible);
        }
    } else if (key.scancode == 0x4B) { // Left arrow — collapse
        if (de->selected_row >= 0 && de->selected_row < row_count) {
            int cat = rows[de->selected_row].category;
            if (!de->collapsed[cat]) {
                de->collapsed[cat] = true;
                // If we were on a device row, move selection to the category header
                if (rows[de->selected_row].type == ROW_DEVICE) {
                    // Find the category header row for this category
                    for (int i = de->selected_row - 1; i >= 0; i--) {
                        if (rows[i].type == ROW_CATEGORY && rows[i].category == cat) {
                            de->selected_row = i;
                            break;
                        }
                    }
                    // After collapsing, rebuild and clamp
                    int new_count = build_display_rows(de, rows);
                    if (de->selected_row >= new_count)
                        de->selected_row = new_count - 1;
                }
            }
        }
    } else if (key.scancode == 0x4D) { // Right arrow — expand
        if (de->selected_row >= 0 && de->selected_row < row_count) {
            int cat = rows[de->selected_row].category;
            de->collapsed[cat] = false;
        }
    } else if (key.scancode == 0x1C) { // Enter — toggle on category row
        if (de->selected_row >= 0 && de->selected_row < row_count) {
            if (rows[de->selected_row].type == ROW_CATEGORY) {
                int cat = rows[de->selected_row].category;
                de->collapsed[cat] = !de->collapsed[cat];
            }
        }
    }
}

static void devexplorer_on_close(Window* win) {
    if (win->app_data) {
        montauk::mfree(win->app_data);
        win->app_data = nullptr;
    }
}

// ============================================================================
// Device Explorer launcher
// ============================================================================

void open_devexplorer(DesktopState* ds) {
    int idx = desktop_create_window(ds, "Devices", 140, 70, 640, 460);
    if (idx < 0) return;

    Window* win = &ds->windows[idx];

    DevExplorerState* de = (DevExplorerState*)montauk::malloc(sizeof(DevExplorerState));
    montauk::memset(de, 0, sizeof(DevExplorerState));
    de->desktop = ds;
    de->selected_row = -1;
    de->scroll_y = 0;
    de->last_poll_ms = 0;

    // All categories start expanded
    for (int i = 0; i < NUM_CATEGORIES; i++)
        de->collapsed[i] = false;

    // Initial poll
    de->dev_count = montauk::devlist(de->devs, DE_MAX_DEVS);

    win->app_data = de;
    win->on_draw = devexplorer_on_draw;
    win->on_mouse = devexplorer_on_mouse;
    win->on_key = devexplorer_on_key;
    win->on_close = devexplorer_on_close;
    win->on_poll = devexplorer_on_poll;
}
