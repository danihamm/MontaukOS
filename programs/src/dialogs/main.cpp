/*
    * main.cpp
    * Shared modal dialogs for MontaukOS desktop apps
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/user.h>
#include <gui/gui.hpp>
#include <gui/standalone.hpp>
#include <gui/font.hpp>
#include <gui/canvas.hpp>
#include <gui/svg.hpp>
#include <gui/widgets.hpp>
#include <gui/dialogs.hpp>
#include <print/print.hpp>

extern "C" {
#include <stdio.h>
}

using namespace gui;
namespace dlg = gui::dialogs;

namespace {

constexpr int FILE_INIT_W = 720;
constexpr int FILE_INIT_H = 520;
constexpr int PRINT_INIT_W = 640;
constexpr int PRINT_INIT_H = 400;

constexpr int TOOLBAR_H = 32;
constexpr int PATHBAR_H = 32;
constexpr int FOOTER_H_OPEN = 64;
constexpr int FOOTER_H_SAVE = 74;
constexpr int HEADER_H = 20;
constexpr int ITEM_H = 24;
constexpr int SCROLLBAR_W = 12;
constexpr int GRID_CELL_W = 80;
constexpr int GRID_CELL_H = 80;
constexpr int GRID_ICON = 48;
constexpr int GRID_PAD = 4;

constexpr int MAX_ENTRIES = 64;
constexpr int MAX_HISTORY = 16;
constexpr int MAX_DRIVES = 16;
constexpr int MAX_DISCOVERED_PRINTERS = 8;
constexpr int BUTTON_H = 30;
constexpr int BUTTON_W = 88;

constexpr const char* SPECIAL_FOLDER_NAMES[] = {
    "Documents", "Desktop", "Music", "Videos", "Pictures", "Downloads"
};
constexpr const char* SPECIAL_FOLDER_ICONS[] = {
    "folder-blue-documents.svg",
    "folder-blue-desktop.svg",
    "folder-blue-music.svg",
    "folder-blue-videos.svg",
    "folder-blue-pictures.svg",
    "folder-blue-downloads.svg"
};
constexpr int SPECIAL_FOLDER_COUNT = 6;

enum EntryType : uint8_t {
    ENTRY_FILE = 0,
    ENTRY_DIR = 1,
    ENTRY_DRIVE = 3,
    ENTRY_HOME = 4,
    ENTRY_SPECIAL = 7,
};

enum FocusField : uint8_t {
    FOCUS_LIST = 0,
    FOCUS_PATH = 1,
    FOCUS_NAME = 2,
};

struct FileDialogIcons {
    SvgIcon icon_go_back;
    SvgIcon icon_go_forward;
    SvgIcon icon_go_up;
    SvgIcon icon_home;
    SvgIcon icon_folder;
    SvgIcon icon_folder_lg;
    SvgIcon icon_file;
    SvgIcon icon_file_lg;
    SvgIcon icon_drive;
    SvgIcon icon_drive_lg;
    SvgIcon icon_home_folder;
    SvgIcon icon_home_folder_lg;
    SvgIcon icon_special[ SPECIAL_FOLDER_COUNT ];
    SvgIcon icon_special_lg[ SPECIAL_FOLDER_COUNT ];
};

struct FileDialogState {
    FileDialogIcons icons;

    char home_dir[256];
    char current_path[256];
    char history[MAX_HISTORY][256];
    int history_pos;
    int history_count;

    char entry_names[MAX_ENTRIES][64];
    uint8_t entry_types[MAX_ENTRIES];
    int entry_sizes[MAX_ENTRIES];
    bool is_dir[MAX_ENTRIES];
    int drive_indices[MAX_ENTRIES];
    int entry_count;

    int selected;
    int last_click_item;
    uint64_t last_click_time;

    Scrollbar scrollbar;
    bool grid_view;
    bool at_drives_root;
    FocusField focus;

    char path_buf[256];
    int path_len;
    int path_cursor;

    char filename_buf[128];
    int filename_len;
    int filename_cursor;

    char pending_select[64];
    char error[160];

    int mouse_x;
    int mouse_y;
};

struct PrintDialogState {
    SvgIcon printer_icon;
    char source_path[256];
    char job_name[128];
    struct PrinterRow {
        char uri[256];
        char name[128];
        char status[128];
        char seen_at[32];
        bool ok;
        bool is_default;
    } printers[MAX_DISCOVERED_PRINTERS];
    int printer_count;
    int selected_printer;
    uint32_t copies;
    int queue_count;
    int active_count;
    bool daemon_running;
    bool submit_mode;
    bool source_ready;
    char note[160];
    char error[160];
    int mouse_x;
    int mouse_y;
};

struct AppState {
    WsWindow win;
    dlg::Request request;
    dlg::Result result;
    bool running;
    bool is_print;
    FileDialogState file;
    PrintDialogState print;
};

AppState g_app = {};

struct FileDialogLayout {
    Rect back_btn;
    Rect forward_btn;
    Rect up_btn;
    Rect home_btn;
    Rect view_btn;
    Rect path_rect;
    Rect content_rect;
    Rect list_rect;
    Rect footer_rect;
    Rect name_rect;
    Rect cancel_btn;
    Rect confirm_btn;
};

struct PrintDialogLayout {
    Rect header_rect;
    Rect printers_rect;
    Rect detail_rect;
    Rect copy_minus_btn;
    Rect copy_value_rect;
    Rect copy_plus_btn;
    Rect cancel_btn;
    Rect printers_btn;
    Rect refresh_btn;
    Rect confirm_btn;
};

void safe_copy(char* dst, int dst_len, const char* src) {
    dlg::safe_copy(dst, dst_len, src);
}

int str_compare_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool str_ends_with(const char* s, const char* suffix) {
    int slen = montauk::slen(s);
    int suflen = montauk::slen(suffix);
    if (suflen > slen) return false;
    for (int i = 0; i < suflen; i++) {
        char sc = s[slen - suflen + i];
        char ec = suffix[i];
        if (sc >= 'A' && sc <= 'Z') sc += 32;
        if (ec >= 'A' && ec <= 'Z') ec += 32;
        if (sc != ec) return false;
    }
    return true;
}

void str_append(char* dst, const char* src, int max_len) {
    int len = montauk::slen(dst);
    int i = 0;
    while (src[i] && len < max_len - 1) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

const char* path_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' && *(p + 1)) last = p + 1;
    }
    return last;
}

void build_fullpath(char* out, int out_len, const char* dir, const char* name) {
    safe_copy(out, out_len, dir);
    int len = montauk::slen(out);
    if (len > 0 && out[len - 1] != '/') str_append(out, "/", out_len);
    str_append(out, name, out_len);
}

void split_parent_path(const char* path, char* out_dir, int out_dir_len, char* out_name, int out_name_len) {
    safe_copy(out_dir, out_dir_len, "");
    safe_copy(out_name, out_name_len, "");
    if (!path || !path[0]) return;

    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;

    if (last_slash < 0) {
        safe_copy(out_name, out_name_len, path);
        return;
    }

    int dir_len = last_slash + 1;
    if (dir_len >= out_dir_len) dir_len = out_dir_len - 1;
    montauk::memcpy(out_dir, path, (uint64_t)dir_len);
    out_dir[dir_len] = '\0';
    safe_copy(out_name, out_name_len, path + last_slash + 1);
}

bool is_dir_path(const char* path) {
    if (!path || !path[0]) return false;
    const char* names[1];
    return montauk::readdir(path, names, 1) >= 0;
}

bool path_exists_via_open(const char* path) {
    if (!path || !path[0]) return false;
    int fd = montauk::open(path);
    if (fd < 0) return false;
    montauk::close(fd);
    return true;
}

void format_size(char* buf, int buf_len, int size) {
    if (size < 1024) snprintf(buf, buf_len, "%d B", size);
    else if (size < 1024 * 1024) {
        int kb = size / 1024;
        int frac = ((size % 1024) * 10) / 1024;
        if (kb < 10) snprintf(buf, buf_len, "%d.%d KB", kb, frac);
        else snprintf(buf, buf_len, "%d KB", kb);
    } else {
        int mb = size / (1024 * 1024);
        int frac = ((size % (1024 * 1024)) * 10) / (1024 * 1024);
        if (mb < 10) snprintf(buf, buf_len, "%d.%d MB", mb, frac);
        else snprintf(buf, buf_len, "%d MB", mb);
    }
}

void fit_text_end(const char* src, char* out, int out_len, int max_px) {
    if (!src || !src[0]) {
        safe_copy(out, out_len, "");
        return;
    }
    if (text_width(src) <= max_px) {
        safe_copy(out, out_len, src);
        return;
    }

    int slen = montauk::slen(src);
    int keep = slen;
    while (keep > 1) {
        char candidate[256];
        candidate[0] = '.';
        candidate[1] = '.';
        int pos = 2;
        const char* tail = src + (slen - keep);
        while (*tail && pos < (int)sizeof(candidate) - 1)
            candidate[pos++] = *tail++;
        candidate[pos] = '\0';
        if (text_width(candidate) <= max_px) {
            safe_copy(out, out_len, candidate);
            return;
        }
        keep--;
    }

    safe_copy(out, out_len, src);
}

void draw_input(Canvas& c, const Rect& rect, const char* text, bool focused, int cursor_pos) {
    c.fill_rect(rect.x, rect.y, rect.w, rect.h, colors::WHITE);
    c.rect(rect.x, rect.y, rect.w, rect.h, focused ? colors::ACCENT : colors::BORDER);
    int ty = rect.y + (rect.h - system_font_height()) / 2;
    c.text(rect.x + 6, ty, text, colors::TEXT_COLOR);
    if (focused) {
        char prefix[256];
        int plen = cursor_pos;
        if (plen > 255) plen = 255;
        for (int i = 0; i < plen; i++) prefix[i] = text[i];
        prefix[plen] = '\0';
        int cx = rect.x + 6 + text_width(prefix);
        c.vline(cx, rect.y + 4, rect.h - 8, colors::ACCENT);
    }
}

void draw_action_button(Canvas& c, const Rect& rect, const char* label, bool enabled, bool hovered, bool primary) {
    Color bg = primary ? colors::ACCENT : Color::from_rgb(0xE8, 0xE8, 0xE8);
    Color fg = primary ? colors::WHITE : colors::TEXT_COLOR;
    if (!enabled) {
        bg = Color::from_rgb(0xF0, 0xF0, 0xF0);
        fg = Color::from_rgb(0x9A, 0x9A, 0x9A);
    } else if (hovered) {
        bg = primary ? Color::from_rgb(0x2B, 0x6B, 0xE0) : Color::from_rgb(0xDC, 0xDC, 0xDC);
    }
    c.fill_rounded_rect(rect.x, rect.y, rect.w, rect.h, 4, bg);
    int tx = rect.x + (rect.w - text_width(label)) / 2;
    int ty = rect.y + (rect.h - system_font_height()) / 2;
    c.text(tx, ty, label, fg);
}

void draw_scrollbar(Canvas& c, const Scrollbar& sb) {
    if (sb.content_height <= sb.view_height) return;
    c.fill_rect(sb.bounds.x, sb.bounds.y, sb.bounds.w, sb.bounds.h, sb.bg);
    int th = sb.thumb_height();
    int ty = sb.thumb_y();
    Color thumb = (sb.hovered || sb.dragging) ? sb.hover_fg : sb.fg;
    c.fill_rounded_rect(sb.bounds.x + 1, ty, sb.bounds.w - 2, th, 3, thumb);
}

FileDialogLayout file_layout(const FileDialogState* st) {
    FileDialogLayout lo = {};
    int footer_h = (g_app.request.mode == dlg::FILE_DIALOG_SAVE) ? FOOTER_H_SAVE : FOOTER_H_OPEN;
    lo.back_btn = {4, 4, 24, 24};
    lo.forward_btn = {32, 4, 24, 24};
    lo.up_btn = {60, 4, 24, 24};
    lo.home_btn = {88, 4, 24, 24};
    lo.view_btn = {120, 4, 24, 24};
    lo.path_rect = {8, TOOLBAR_H + 4, g_app.win.width - 16, PATHBAR_H - 8};
    lo.content_rect = {0, TOOLBAR_H + PATHBAR_H, g_app.win.width, g_app.win.height - TOOLBAR_H - PATHBAR_H - footer_h};
    lo.footer_rect = {0, g_app.win.height - footer_h, g_app.win.width, footer_h};
    lo.list_rect = lo.content_rect;
    int btn_y = lo.footer_rect.y + footer_h - BUTTON_H - 12;
    lo.confirm_btn = {g_app.win.width - BUTTON_W - 16, btn_y, BUTTON_W, BUTTON_H};
    lo.cancel_btn = {lo.confirm_btn.x - BUTTON_W - 8, btn_y, BUTTON_W, BUTTON_H};
    lo.name_rect = {88, lo.footer_rect.y + 12, lo.cancel_btn.x - 100, 30};
    if (lo.name_rect.w < 120) lo.name_rect.w = 120;
    (void)st;
    return lo;
}

PrintDialogLayout print_layout() {
    PrintDialogLayout lo = {};
    lo.header_rect = {0, 0, 0, 0};
    lo.printers_rect = {16, 16, 212, g_app.win.height - 94};
    lo.detail_rect = {244, 16, g_app.win.width - 260, g_app.win.height - 94};
    int copy_y = lo.detail_rect.y + 202;
    lo.copy_minus_btn = {lo.detail_rect.x + 16, copy_y, 28, 28};
    lo.copy_value_rect = {lo.copy_minus_btn.x + 36, copy_y, 104, 28};
    lo.copy_plus_btn = {lo.copy_value_rect.x + lo.copy_value_rect.w + 8, copy_y, 28, 28};
    int btn_y = g_app.win.height - BUTTON_H - 16;
    lo.confirm_btn = {g_app.win.width - BUTTON_W - 16, btn_y, BUTTON_W, BUTTON_H};
    lo.cancel_btn = {lo.confirm_btn.x - BUTTON_W - 8, btn_y, BUTTON_W, BUTTON_H};
    lo.refresh_btn = {16, btn_y, BUTTON_W, BUTTON_H};
    lo.printers_btn = {lo.refresh_btn.x + BUTTON_W + 8, btn_y, 112, BUTTON_H};
    return lo;
}

void dialog_finish(const char* status, const char* path, const char* job_id, const char* message,
                   const char* printer_uri = "", const char* printer_name = "", uint32_t copies = 0) {
    safe_copy(g_app.result.status, sizeof(g_app.result.status), status);
    safe_copy(g_app.result.path, sizeof(g_app.result.path), path);
    safe_copy(g_app.result.job_id, sizeof(g_app.result.job_id), job_id);
    safe_copy(g_app.result.printer_uri, sizeof(g_app.result.printer_uri), printer_uri);
    safe_copy(g_app.result.printer_name, sizeof(g_app.result.printer_name), printer_name);
    g_app.result.copies = copies;
    safe_copy(g_app.result.message, sizeof(g_app.result.message), message);
    dlg::write_result_file(g_app.request.result_path, &g_app.result);
    g_app.running = false;
}

void dialog_cancel(const char* message = "") {
    dialog_finish("cancel", "", "", message);
}

void file_sync_path_buffer(FileDialogState* st) {
    if (st->at_drives_root) {
        st->path_buf[0] = '\0';
        st->path_len = 0;
        st->path_cursor = 0;
        return;
    }
    safe_copy(st->path_buf, sizeof(st->path_buf), st->current_path);
    st->path_len = montauk::slen(st->path_buf);
    st->path_cursor = st->path_len;
}

void file_set_filename(FileDialogState* st, const char* name) {
    safe_copy(st->filename_buf, sizeof(st->filename_buf), name);
    st->filename_len = montauk::slen(st->filename_buf);
    st->filename_cursor = st->filename_len;
}

void file_set_error(FileDialogState* st, const char* msg) {
    safe_copy(st->error, sizeof(st->error), msg);
}

int special_folder_index(const char* name) {
    for (int i = 0; i < SPECIAL_FOLDER_COUNT; i++) {
        if (montauk::streq(name, SPECIAL_FOLDER_NAMES[i])) return i;
    }
    return -1;
}

void load_file_icons(FileDialogState* st) {
    Color def = colors::ICON_COLOR;
    st->icons.icon_go_back = svg_load("0:/icons/go-previous-symbolic.svg", 16, 16, def);
    st->icons.icon_go_forward = svg_load("0:/icons/go-next-symbolic.svg", 16, 16, def);
    st->icons.icon_go_up = svg_load("0:/icons/go-up-symbolic.svg", 16, 16, def);
    st->icons.icon_home = svg_load("0:/icons/user-home.svg", 16, 16, def);
    st->icons.icon_folder = svg_load("0:/icons/folder.svg", 16, 16, def);
    st->icons.icon_folder_lg = svg_load("0:/icons/folder.svg", 48, 48, def);
    st->icons.icon_file = svg_load("0:/icons/text-x-generic.svg", 16, 16, def);
    st->icons.icon_file_lg = svg_load("0:/icons/text-x-generic.svg", 48, 48, def);
    st->icons.icon_drive = svg_load("0:/icons/drive-harddisk.svg", 16, 16, def);
    st->icons.icon_drive_lg = svg_load("0:/icons/drive-harddisk.svg", 48, 48, def);
    st->icons.icon_home_folder = svg_load("0:/icons/folder-blue-home.svg", 16, 16, def);
    st->icons.icon_home_folder_lg = svg_load("0:/icons/folder-blue-home.svg", 48, 48, def);
    for (int i = 0; i < SPECIAL_FOLDER_COUNT; i++) {
        char path[128];
        snprintf(path, sizeof(path), "0:/icons/%s", SPECIAL_FOLDER_ICONS[i]);
        st->icons.icon_special[i] = svg_load(path, 16, 16, def);
        st->icons.icon_special_lg[i] = svg_load(path, 48, 48, def);
    }
}

void free_icon(SvgIcon* icon) {
    if (icon && icon->pixels) {
        svg_free(*icon);
        icon->pixels = nullptr;
    }
}

void free_file_icons(FileDialogState* st) {
    free_icon(&st->icons.icon_go_back);
    free_icon(&st->icons.icon_go_forward);
    free_icon(&st->icons.icon_go_up);
    free_icon(&st->icons.icon_home);
    free_icon(&st->icons.icon_folder);
    free_icon(&st->icons.icon_folder_lg);
    free_icon(&st->icons.icon_file);
    free_icon(&st->icons.icon_file_lg);
    free_icon(&st->icons.icon_drive);
    free_icon(&st->icons.icon_drive_lg);
    free_icon(&st->icons.icon_home_folder);
    free_icon(&st->icons.icon_home_folder_lg);
    for (int i = 0; i < SPECIAL_FOLDER_COUNT; i++) {
        free_icon(&st->icons.icon_special[i]);
        free_icon(&st->icons.icon_special_lg[i]);
    }
}

void file_push_history(FileDialogState* st) {
    if (st->history_count > 0 && st->history_pos >= 0) {
        if (montauk::streq(st->history[st->history_pos], st->current_path)) return;
    }
    st->history_pos++;
    if (st->history_pos >= MAX_HISTORY) st->history_pos = MAX_HISTORY - 1;
    safe_copy(st->history[st->history_pos], sizeof(st->history[st->history_pos]), st->current_path);
    st->history_count = st->history_pos + 1;
}

void file_apply_pending_select(FileDialogState* st) {
    st->selected = -1;
    if (!st->pending_select[0]) return;
    for (int i = 0; i < st->entry_count; i++) {
        if (montauk::streq(st->entry_names[i], st->pending_select)) {
            st->selected = i;
            break;
        }
    }
    st->pending_select[0] = '\0';
}

void file_read_drives(FileDialogState* st) {
    st->entry_count = 0;
    st->at_drives_root = true;
    st->current_path[0] = '\0';

    if (st->home_dir[0] != '\0') {
        int i = st->entry_count++;
        safe_copy(st->entry_names[i], sizeof(st->entry_names[i]), "Home");
        st->entry_types[i] = ENTRY_HOME;
        st->entry_sizes[i] = 0;
        st->is_dir[i] = true;
        st->drive_indices[i] = -1;
    }

    if (st->home_dir[0] != '\0') {
        for (int sf = 0; sf < SPECIAL_FOLDER_COUNT && st->entry_count < MAX_ENTRIES; sf++) {
            char probe[256];
            safe_copy(probe, sizeof(probe), st->home_dir);
            int plen = montauk::slen(probe);
            if (plen > 0 && probe[plen - 1] != '/') str_append(probe, "/", sizeof(probe));
            str_append(probe, SPECIAL_FOLDER_NAMES[sf], sizeof(probe));
            if (path_exists_via_open(probe)) {
                int i = st->entry_count++;
                safe_copy(st->entry_names[i], sizeof(st->entry_names[i]), SPECIAL_FOLDER_NAMES[sf]);
                st->entry_types[i] = ENTRY_SPECIAL;
                st->entry_sizes[i] = 0;
                st->is_dir[i] = true;
                st->drive_indices[i] = sf;
            }
        }
    }

    int drives[MAX_DRIVES];
    int drive_count = montauk::drivelist(drives, MAX_DRIVES);
    for (int di = 0; di < drive_count && st->entry_count < MAX_ENTRIES; di++) {
        int d = drives[di];
        int i = st->entry_count++;
        char label[64];
        if (d < 10) snprintf(label, sizeof(label), "Drive %d:/", d);
        else snprintf(label, sizeof(label), "Drive %d:/", d);
        safe_copy(st->entry_names[i], sizeof(st->entry_names[i]), label);
        st->entry_types[i] = ENTRY_DRIVE;
        st->entry_sizes[i] = 0;
        st->is_dir[i] = true;
        st->drive_indices[i] = d;
    }

    st->scrollbar.scroll_offset = 0;
    st->last_click_item = -1;
    st->last_click_time = 0;
    file_sync_path_buffer(st);
    file_apply_pending_select(st);
}

void file_read_dir(FileDialogState* st) {
    if (!st->current_path[0] || !is_dir_path(st->current_path)) {
        file_read_drives(st);
        return;
    }

    st->at_drives_root = false;
    const char* names[MAX_ENTRIES];
    st->entry_count = montauk::readdir(st->current_path, names, MAX_ENTRIES);
    if (st->entry_count < 0) st->entry_count = 0;

    const char* after_drive = st->current_path;
    for (int k = 0; after_drive[k]; k++) {
        if (after_drive[k] == ':' && after_drive[k + 1] == '/') {
            after_drive += k + 2;
            break;
        }
    }

    char prefix[256] = {};
    int prefix_len = 0;
    if (after_drive[0] != '\0') {
        safe_copy(prefix, sizeof(prefix), after_drive);
        prefix_len = montauk::slen(prefix);
        if (prefix_len > 0 && prefix[prefix_len - 1] != '/') {
            prefix[prefix_len++] = '/';
            prefix[prefix_len] = '\0';
        }
    }

    for (int i = 0; i < st->entry_count; i++) {
        const char* raw = names[i];
        if (prefix_len > 0) {
            bool match = true;
            for (int k = 0; k < prefix_len; k++) {
                if (raw[k] != prefix[k]) { match = false; break; }
            }
            if (match) raw += prefix_len;
        }

        safe_copy(st->entry_names[i], sizeof(st->entry_names[i]), raw);
        int len = montauk::slen(st->entry_names[i]);
        if (len > 0 && st->entry_names[i][len - 1] == '/') {
            st->is_dir[i] = true;
            st->entry_names[i][len - 1] = '\0';
        } else {
            st->is_dir[i] = false;
        }

        st->entry_types[i] = st->is_dir[i] ? ENTRY_DIR : ENTRY_FILE;
        st->entry_sizes[i] = 0;
        st->drive_indices[i] = -1;

        if (!st->is_dir[i]) {
            char full[512];
            build_fullpath(full, sizeof(full), st->current_path, st->entry_names[i]);
            int fd = montauk::open(full);
            if (fd >= 0) {
                st->entry_sizes[i] = (int)montauk::getsize(fd);
                montauk::close(fd);
            }
        }
    }

    for (int i = 1; i < st->entry_count; i++) {
        char tmp_name[64];
        uint8_t tmp_type = st->entry_types[i];
        int tmp_size = st->entry_sizes[i];
        bool tmp_isdir = st->is_dir[i];
        safe_copy(tmp_name, sizeof(tmp_name), st->entry_names[i]);

        int j = i - 1;
        while (j >= 0) {
            bool swap = false;
            if (tmp_isdir && !st->is_dir[j]) swap = true;
            else if (tmp_isdir == st->is_dir[j] && str_compare_ci(tmp_name, st->entry_names[j]) < 0) swap = true;
            if (!swap) break;

            safe_copy(st->entry_names[j + 1], sizeof(st->entry_names[j + 1]), st->entry_names[j]);
            st->entry_types[j + 1] = st->entry_types[j];
            st->entry_sizes[j + 1] = st->entry_sizes[j];
            st->is_dir[j + 1] = st->is_dir[j];
            st->drive_indices[j + 1] = st->drive_indices[j];
            j--;
        }

        safe_copy(st->entry_names[j + 1], sizeof(st->entry_names[j + 1]), tmp_name);
        st->entry_types[j + 1] = tmp_type;
        st->entry_sizes[j + 1] = tmp_size;
        st->is_dir[j + 1] = tmp_isdir;
        st->drive_indices[j + 1] = -1;
    }

    st->scrollbar.scroll_offset = 0;
    st->last_click_item = -1;
    st->last_click_time = 0;
    file_sync_path_buffer(st);
    file_apply_pending_select(st);
}

void file_ensure_selected_visible(FileDialogState* st) {
    if (st->selected < 0 || st->selected >= st->entry_count) return;
    FileDialogLayout lo = file_layout(st);
    if (st->grid_view) {
        int cols = (lo.content_rect.w - SCROLLBAR_W) / GRID_CELL_W;
        if (cols < 1) cols = 1;
        int row = st->selected / cols;
        int top = row * GRID_CELL_H;
        int bottom = top + GRID_CELL_H;
        if (top < st->scrollbar.scroll_offset) st->scrollbar.scroll_offset = top;
        else if (bottom > st->scrollbar.scroll_offset + lo.content_rect.h)
            st->scrollbar.scroll_offset = bottom - lo.content_rect.h;
    } else {
        int view_h = lo.content_rect.h - HEADER_H;
        int top = st->selected * ITEM_H;
        int bottom = top + ITEM_H;
        if (top < st->scrollbar.scroll_offset) st->scrollbar.scroll_offset = top;
        else if (bottom > st->scrollbar.scroll_offset + view_h)
            st->scrollbar.scroll_offset = bottom - view_h;
    }
    int ms = st->scrollbar.max_scroll();
    if (st->scrollbar.scroll_offset < 0) st->scrollbar.scroll_offset = 0;
    if (st->scrollbar.scroll_offset > ms) st->scrollbar.scroll_offset = ms;
}

void file_navigate_into(FileDialogState* st, int idx) {
    if (idx < 0 || idx >= st->entry_count) return;

    if (st->at_drives_root && st->entry_types[idx] == ENTRY_HOME) {
        safe_copy(st->current_path, sizeof(st->current_path), st->home_dir);
        file_push_history(st);
        file_read_dir(st);
        return;
    }

    if (st->at_drives_root && st->entry_types[idx] == ENTRY_SPECIAL) {
        safe_copy(st->current_path, sizeof(st->current_path), st->home_dir);
        int len = montauk::slen(st->current_path);
        if (len > 0 && st->current_path[len - 1] != '/') str_append(st->current_path, "/", sizeof(st->current_path));
        str_append(st->current_path, st->entry_names[idx], sizeof(st->current_path));
        file_push_history(st);
        file_read_dir(st);
        return;
    }

    if (st->at_drives_root && st->entry_types[idx] == ENTRY_DRIVE) {
        snprintf(st->current_path, sizeof(st->current_path), "%d:/", st->drive_indices[idx]);
        file_push_history(st);
        file_read_dir(st);
        return;
    }

    if (st->is_dir[idx]) {
        char next[256];
        build_fullpath(next, sizeof(next), st->current_path, st->entry_names[idx]);
        safe_copy(st->current_path, sizeof(st->current_path), next);
        file_push_history(st);
        file_read_dir(st);
    }
}

bool file_accept_open(FileDialogState* st) {
    if (st->selected < 0 || st->selected >= st->entry_count) {
        file_set_error(st, "Select a file to open");
        return false;
    }
    if (st->is_dir[st->selected]) {
        file_navigate_into(st, st->selected);
        return false;
    }

    char full[512];
    build_fullpath(full, sizeof(full), st->current_path, st->entry_names[st->selected]);
    dialog_finish("ok", full, "", full);
    return true;
}

bool file_accept_save(FileDialogState* st) {
    if (st->at_drives_root) {
        file_set_error(st, "Choose a folder before saving");
        return false;
    }
    if (!st->filename_buf[0]) {
        file_set_error(st, "Enter a file name");
        return false;
    }

    char full[512];
    build_fullpath(full, sizeof(full), st->current_path, st->filename_buf);
    dialog_finish("ok", full, "", full);
    return true;
}

void file_activate_selected(FileDialogState* st, bool from_double_click) {
    if (st->selected < 0 || st->selected >= st->entry_count) return;
    if (st->is_dir[st->selected]) {
        file_navigate_into(st, st->selected);
        return;
    }

    if (g_app.request.mode == dlg::FILE_DIALOG_SAVE) {
        file_set_filename(st, st->entry_names[st->selected]);
        if (from_double_click) file_accept_save(st);
    } else {
        file_accept_open(st);
    }
}

void file_go_home(FileDialogState* st) {
    file_read_drives(st);
    file_push_history(st);
}

void file_go_back(FileDialogState* st) {
    if (st->history_pos <= 0) return;
    st->history_pos--;
    safe_copy(st->current_path, sizeof(st->current_path), st->history[st->history_pos]);
    if (st->current_path[0]) file_read_dir(st);
    else file_read_drives(st);
}

void file_go_forward(FileDialogState* st) {
    if (st->history_pos >= st->history_count - 1) return;
    st->history_pos++;
    safe_copy(st->current_path, sizeof(st->current_path), st->history[st->history_pos]);
    if (st->current_path[0]) file_read_dir(st);
    else file_read_drives(st);
}

void file_go_up(FileDialogState* st) {
    if (st->at_drives_root) return;

    int len = montauk::slen(st->current_path);
    if (len >= 3 && st->current_path[len - 1] == '/') {
        int colon = -1;
        for (int i = 0; i < len; i++) {
            if (st->current_path[i] == ':') { colon = i; break; }
        }
        if (colon >= 0 && colon + 2 == len) {
            file_read_drives(st);
            file_push_history(st);
            return;
        }
    }

    if (len > 0 && st->current_path[len - 1] == '/') {
        st->current_path[len - 1] = '\0';
        len--;
    }
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (st->current_path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash >= 0) {
        if (last_slash > 0 && st->current_path[last_slash - 1] == ':') st->current_path[last_slash + 1] = '\0';
        else st->current_path[last_slash] = '\0';
    }
    file_push_history(st);
    file_read_dir(st);
}

void file_open_path(FileDialogState* st, const char* path) {
    if (!path || !path[0]) {
        if (st->home_dir[0]) {
            safe_copy(st->current_path, sizeof(st->current_path), st->home_dir);
            file_push_history(st);
            file_read_dir(st);
        } else {
            file_read_drives(st);
            file_push_history(st);
        }
        return;
    }

    if (is_dir_path(path)) {
        safe_copy(st->current_path, sizeof(st->current_path), path);
        file_push_history(st);
        file_read_dir(st);
        return;
    }

    char dir[256];
    char name[128];
    split_parent_path(path, dir, sizeof(dir), name, sizeof(name));
    if (dir[0] && is_dir_path(dir)) {
        safe_copy(st->current_path, sizeof(st->current_path), dir);
        safe_copy(st->pending_select, sizeof(st->pending_select), name);
        if (g_app.request.mode == dlg::FILE_DIALOG_SAVE && name[0]) file_set_filename(st, name);
        file_push_history(st);
        file_read_dir(st);
        if (g_app.request.mode == dlg::FILE_DIALOG_OPEN && name[0]) {
            for (int i = 0; i < st->entry_count; i++) {
                if (montauk::streq(st->entry_names[i], name)) {
                    st->selected = i;
                    break;
                }
            }
        }
        return;
    }

    file_set_error(st, "Path does not exist");
}

void init_file_dialog(const dlg::Request& req) {
    FileDialogState* st = &g_app.file;
    montauk::memset(st, 0, sizeof(*st));
    load_file_icons(st);
    st->selected = -1;
    st->last_click_item = -1;
    st->history_pos = -1;
    st->grid_view = true;
    st->focus = FOCUS_LIST;
    st->scrollbar.init(0, 0, SCROLLBAR_W, 100);
    montauk::user::get_home_dir(st->home_dir, sizeof(st->home_dir));

    char initial_dir[256] = {};
    char initial_name[128] = {};
    if (req.initial_path[0]) split_parent_path(req.initial_path, initial_dir, sizeof(initial_dir), initial_name, sizeof(initial_name));

    if (req.initial_path[0] && is_dir_path(req.initial_path)) {
        safe_copy(st->current_path, sizeof(st->current_path), req.initial_path);
        file_read_dir(st);
        file_push_history(st);
    } else if (initial_dir[0] && is_dir_path(initial_dir)) {
        safe_copy(st->current_path, sizeof(st->current_path), initial_dir);
        safe_copy(st->pending_select, sizeof(st->pending_select), initial_name);
        file_read_dir(st);
        file_push_history(st);
    } else if (st->home_dir[0] && is_dir_path(st->home_dir)) {
        safe_copy(st->current_path, sizeof(st->current_path), st->home_dir);
        file_read_dir(st);
        file_push_history(st);
    } else {
        file_read_drives(st);
        file_push_history(st);
    }

    if (req.mode == dlg::FILE_DIALOG_SAVE) {
        if (req.suggested_name[0]) file_set_filename(st, req.suggested_name);
        else if (initial_name[0]) file_set_filename(st, initial_name);
    }
}

void draw_toolbar_icon(Canvas& c, const Rect& rect, const SvgIcon& icon) {
    c.fill_rect(rect.x, rect.y, rect.w, rect.h, Color::from_rgb(0xE8, 0xE8, 0xE8));
    if (icon.pixels) {
        int ix = rect.x + (rect.w - icon.width) / 2;
        int iy = rect.y + (rect.h - icon.height) / 2;
        c.icon(ix, iy, icon);
    }
}

void draw_grid_entries(Canvas& c, FileDialogState* st, const FileDialogLayout& lo) {
    int cols = (lo.content_rect.w - SCROLLBAR_W) / GRID_CELL_W;
    if (cols < 1) cols = 1;
    int rows = (st->entry_count + cols - 1) / cols;
    int content_h = rows * GRID_CELL_H;
    st->scrollbar.bounds = {c.w - SCROLLBAR_W, lo.content_rect.y, SCROLLBAR_W, lo.content_rect.h};
    st->scrollbar.content_height = content_h;
    st->scrollbar.view_height = lo.content_rect.h;

    for (int i = 0; i < st->entry_count; i++) {
        int col = i % cols;
        int row = i / cols;
        int cell_x = col * GRID_CELL_W;
        int cell_y = lo.content_rect.y + row * GRID_CELL_H - st->scrollbar.scroll_offset;
        if (cell_y + GRID_CELL_H <= lo.content_rect.y || cell_y >= lo.content_rect.y + lo.content_rect.h) continue;

        if (i == st->selected) {
            int sy = gui_max(cell_y, lo.content_rect.y);
            int sh = gui_min(cell_y + GRID_CELL_H, lo.content_rect.y + lo.content_rect.h) - sy;
            int sw = gui_min(GRID_CELL_W, c.w - SCROLLBAR_W - cell_x);
            if (sh > 0 && sw > 0) c.fill_rect(cell_x, sy, sw, sh, colors::MENU_HOVER);
        }

        int icon_x = cell_x + (GRID_CELL_W - GRID_ICON) / 2;
        int icon_y = cell_y + GRID_PAD;
        SvgIcon* icon = &st->icons.icon_file_lg;
        int sfi = -1;
        if (st->entry_types[i] == ENTRY_SPECIAL) sfi = st->drive_indices[i];
        if (sfi >= 0 && st->icons.icon_special_lg[sfi].pixels) icon = &st->icons.icon_special_lg[sfi];
        else if (st->entry_types[i] == ENTRY_HOME && st->icons.icon_home_folder_lg.pixels) icon = &st->icons.icon_home_folder_lg;
        else if (st->entry_types[i] == ENTRY_DRIVE && st->icons.icon_drive_lg.pixels) icon = &st->icons.icon_drive_lg;
        else if (st->is_dir[i] && st->icons.icon_folder_lg.pixels) icon = &st->icons.icon_folder_lg;
        if (icon->pixels) c.icon(icon_x, icon_y, *icon);

        char label[16];
        int nlen = montauk::slen(st->entry_names[i]);
        if (nlen > 11) {
            for (int k = 0; k < 9; k++) label[k] = st->entry_names[i][k];
            label[9] = '.';
            label[10] = '.';
            label[11] = '\0';
        } else {
            safe_copy(label, sizeof(label), st->entry_names[i]);
        }
        int tx = cell_x + (GRID_CELL_W - text_width(label)) / 2;
        if (tx < cell_x) tx = cell_x;
        int ty = icon_y + GRID_ICON + 2;
        if (ty >= lo.content_rect.y && ty + system_font_height() <= lo.content_rect.y + lo.content_rect.h)
            c.text(tx, ty, label, colors::TEXT_COLOR);
    }
}

void draw_list_entries(Canvas& c, FileDialogState* st, const FileDialogLayout& lo) {
    int header_y = lo.content_rect.y;
    int rows_y = header_y + HEADER_H;
    int rows_h = lo.content_rect.h - HEADER_H;
    if (rows_h < 0) rows_h = 0;

    c.fill_rect(0, header_y, c.w, HEADER_H, Color::from_rgb(0xF8, 0xF8, 0xF8));
    c.text(8, header_y + 2, "Name", Color::from_rgb(0x88, 0x88, 0x88));
    c.text(c.w - SCROLLBAR_W - 120, header_y + 2, "Size", Color::from_rgb(0x88, 0x88, 0x88));
    c.text(c.w - SCROLLBAR_W - 60, header_y + 2, "Type", Color::from_rgb(0x88, 0x88, 0x88));
    c.hline(0, header_y + HEADER_H - 1, c.w, colors::BORDER);
    c.vline(c.w - SCROLLBAR_W - 124, header_y, lo.content_rect.h, colors::BORDER);
    c.vline(c.w - SCROLLBAR_W - 64, header_y, lo.content_rect.h, colors::BORDER);

    st->scrollbar.bounds = {c.w - SCROLLBAR_W, rows_y, SCROLLBAR_W, rows_h};
    st->scrollbar.content_height = st->entry_count * ITEM_H;
    st->scrollbar.view_height = rows_h;

    for (int i = 0; i < st->entry_count; i++) {
        int y = rows_y + i * ITEM_H - st->scrollbar.scroll_offset;
        if (y + ITEM_H <= rows_y || y >= rows_y + rows_h) continue;

        if (i == st->selected) c.fill_rect(0, y, c.w - SCROLLBAR_W, ITEM_H, colors::MENU_HOVER);

        int sfi = -1;
        if (st->entry_types[i] == ENTRY_SPECIAL) sfi = st->drive_indices[i];
        if (sfi >= 0 && st->icons.icon_special[sfi].pixels) c.icon(8, y + 4, st->icons.icon_special[sfi]);
        else if (st->entry_types[i] == ENTRY_HOME && st->icons.icon_home_folder.pixels) c.icon(8, y + 4, st->icons.icon_home_folder);
        else if (st->entry_types[i] == ENTRY_DRIVE && st->icons.icon_drive.pixels) c.icon(8, y + 4, st->icons.icon_drive);
        else if (st->is_dir[i] && st->icons.icon_folder.pixels) c.icon(8, y + 4, st->icons.icon_folder);
        else if (st->icons.icon_file.pixels) c.icon(8, y + 4, st->icons.icon_file);

        c.text(30, y + 3, st->entry_names[i], colors::TEXT_COLOR);

        if (!st->is_dir[i]) {
            char size_buf[16];
            format_size(size_buf, sizeof(size_buf), st->entry_sizes[i]);
            c.text(c.w - SCROLLBAR_W - 120, y + 3, size_buf, Color::from_rgb(0x66, 0x66, 0x66));
        }

        const char* type_label = st->is_dir[i] ? "Folder" : "File";
        if (st->entry_types[i] == ENTRY_DRIVE) type_label = "Drive";
        else if (st->entry_types[i] == ENTRY_HOME) type_label = "Home";
        c.text(c.w - SCROLLBAR_W - 60, y + 3, type_label, Color::from_rgb(0x66, 0x66, 0x66));
        c.hline(0, y + ITEM_H - 1, c.w, Color::from_rgb(0xF0, 0xF0, 0xF0));
    }
}

void draw_file_dialog() {
    FileDialogState* st = &g_app.file;
    Canvas c = g_app.win.canvas();
    c.fill(colors::WINDOW_BG);

    FileDialogLayout lo = file_layout(st);

    c.fill_rect(0, 0, c.w, TOOLBAR_H, Color::from_rgb(0xF5, 0xF5, 0xF5));
    draw_toolbar_icon(c, lo.back_btn, st->icons.icon_go_back);
    draw_toolbar_icon(c, lo.forward_btn, st->icons.icon_go_forward);
    draw_toolbar_icon(c, lo.up_btn, st->icons.icon_go_up);
    draw_toolbar_icon(c, lo.home_btn, st->icons.icon_home);

    c.fill_rect(lo.view_btn.x, lo.view_btn.y, lo.view_btn.w, lo.view_btn.h, Color::from_rgb(0xE8, 0xE8, 0xE8));
    if (st->grid_view) {
        for (int r = 0; r < 2; r++)
            for (int cc = 0; cc < 2; cc++)
                c.fill_rect(lo.view_btn.x + 5 + cc * 8, lo.view_btn.y + 5 + r * 8, 6, 6, colors::TEXT_COLOR);
    } else {
        for (int r = 0; r < 3; r++)
            c.fill_rect(lo.view_btn.x + 5, lo.view_btn.y + 5 + r * 5, 14, 2, colors::TEXT_COLOR);
    }
    c.hline(0, TOOLBAR_H - 1, c.w, colors::BORDER);

    char path_label[256];
    if (st->at_drives_root && st->focus != FOCUS_PATH) safe_copy(path_label, sizeof(path_label), "Computer");
    else safe_copy(path_label, sizeof(path_label), st->path_buf);
    draw_input(c, lo.path_rect, path_label, st->focus == FOCUS_PATH, st->path_cursor);

    if (st->grid_view) draw_grid_entries(c, st, lo);
    else draw_list_entries(c, st, lo);

    draw_scrollbar(c, st->scrollbar);

    c.fill_rect(0, lo.footer_rect.y, c.w, lo.footer_rect.h, Color::from_rgb(0xF7, 0xF7, 0xF7));
    c.hline(0, lo.footer_rect.y, c.w, colors::BORDER);

    bool confirm_enabled = false;
    const char* confirm_label = (g_app.request.mode == dlg::FILE_DIALOG_SAVE) ? "Save" : "Open";
    if (g_app.request.mode == dlg::FILE_DIALOG_SAVE) {
        c.text(12, lo.footer_rect.y + 18, "File name", Color::from_rgb(0x66, 0x66, 0x66));
        draw_input(c, lo.name_rect, st->filename_buf, st->focus == FOCUS_NAME, st->filename_cursor);
        confirm_enabled = st->filename_buf[0] != '\0' && !st->at_drives_root;
    } else {
        char selection[256] = {};
        if (st->selected >= 0 && st->selected < st->entry_count) {
            char full[512];
            if (st->at_drives_root && st->entry_types[st->selected] == ENTRY_HOME) safe_copy(full, sizeof(full), st->home_dir);
            else if (st->at_drives_root && st->entry_types[st->selected] == ENTRY_DRIVE) snprintf(full, sizeof(full), "%d:/", st->drive_indices[st->selected]);
            else if (st->at_drives_root && st->entry_types[st->selected] == ENTRY_SPECIAL) {
                safe_copy(full, sizeof(full), st->home_dir);
                int len = montauk::slen(full);
                if (len > 0 && full[len - 1] != '/') str_append(full, "/", sizeof(full));
                str_append(full, st->entry_names[st->selected], sizeof(full));
            } else {
                build_fullpath(full, sizeof(full), st->current_path, st->entry_names[st->selected]);
            }
            fit_text_end(full, selection, sizeof(selection), lo.cancel_btn.x - 16);
        } else {
            safe_copy(selection, sizeof(selection), "Choose a file");
        }
        c.text(12, lo.footer_rect.y + 14, selection, Color::from_rgb(0x66, 0x66, 0x66));
        confirm_enabled = st->selected >= 0;
    }

    if (st->error[0]) {
        char err[160];
        fit_text_end(st->error, err, sizeof(err), lo.cancel_btn.x - 16);
        c.text(12, lo.footer_rect.y + lo.footer_rect.h - system_font_height() - 12, err, Color::from_rgb(0xC0, 0x33, 0x33));
    }

    draw_action_button(c, lo.cancel_btn, "Cancel", true, lo.cancel_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, lo.confirm_btn, confirm_label, confirm_enabled, lo.confirm_btn.contains(st->mouse_x, st->mouse_y), true);
}

int count_dir_entries(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;

    int count = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    return count;
}

void print_set_error(PrintDialogState* st, const char* msg) {
    safe_copy(st->error, sizeof(st->error), msg);
}

void print_adjust_copies(PrintDialogState* st, int delta) {
    if (!st) return;
    int next = (int)st->copies + delta;
    if (next < 1) next = 1;
    if (next > 99) next = 99;
    st->copies = (uint32_t)next;
}

void print_update_note(PrintDialogState* st) {
    if (st->printer_count <= 0) {
        safe_copy(st->note, sizeof(st->note), "No printers discovered from the userspace print spooler yet.");
        return;
    }
    if (!st->daemon_running) {
        safe_copy(st->note, sizeof(st->note), "The print spooler is offline. Saved printers are still available.");
        return;
    }
    if (st->submit_mode) {
        safe_copy(st->note, sizeof(st->note), "The selected document will be handed to the userspace print spooler.");
    } else {
        safe_copy(st->note, sizeof(st->note), "This app stores printer settings only for now. Document output wiring will be added later.");
    }
}

int print_find_printer(const PrintDialogState* st, const char* uri) {
    if (!st || !uri || !uri[0]) return -1;
    for (int i = 0; i < st->printer_count; i++) {
        if (montauk::streq(st->printers[i].uri, uri)) return i;
    }
    return -1;
}

void print_add_printer(PrintDialogState* st,
                       const char* uri,
                       const char* name,
                       const char* status,
                       const char* seen_at,
                       bool ok,
                       bool is_default) {
    if (!st || !uri || !uri[0]) return;

    int idx = print_find_printer(st, uri);
    if (idx < 0) {
        if (st->printer_count >= MAX_DISCOVERED_PRINTERS) return;
        idx = st->printer_count++;
        montauk::memset(&st->printers[idx], 0, sizeof(st->printers[idx]));
        safe_copy(st->printers[idx].uri, sizeof(st->printers[idx].uri), uri);
    }

    if (name && name[0]) safe_copy(st->printers[idx].name, sizeof(st->printers[idx].name), name);
    if (status && status[0]) safe_copy(st->printers[idx].status, sizeof(st->printers[idx].status), status);
    if (seen_at && seen_at[0]) safe_copy(st->printers[idx].seen_at, sizeof(st->printers[idx].seen_at), seen_at);
    st->printers[idx].ok = ok;
    st->printers[idx].is_default = is_default;
}

Rect print_row_rect(const PrintDialogLayout& lo, int idx) {
    return {lo.printers_rect.x + 8, lo.printers_rect.y + 28 + idx * 58, lo.printers_rect.w - 16, 52};
}

bool print_can_confirm(const PrintDialogState* st) {
    if (!st || st->selected_printer < 0 || st->selected_printer >= st->printer_count) return false;
    if (!st->submit_mode) return true;
    return st->source_ready;
}

void print_refresh(PrintDialogState* st) {
    if (!st) return;

    char preferred_uri[256] = {};
    if (st->selected_printer >= 0 && st->selected_printer < st->printer_count)
        safe_copy(preferred_uri, sizeof(preferred_uri), st->printers[st->selected_printer].uri);
    else if (g_app.request.printer_uri[0])
        safe_copy(preferred_uri, sizeof(preferred_uri), g_app.request.printer_uri);

    st->printer_count = 0;
    st->selected_printer = -1;
    st->error[0] = '\0';
    st->queue_count = count_dir_entries(print::SPOOL_QUEUE_DIR);
    st->active_count = count_dir_entries(print::SPOOL_ACTIVE_DIR);

    int pid = -1;
    st->daemon_running = print::daemon_is_running(&pid);

    print::KnownPrinter known[MAX_DISCOVERED_PRINTERS];
    int known_count = print::list_known_printers(known, MAX_DISCOVERED_PRINTERS);
    for (int i = 0; i < known_count; i++) {
        const char* status = known[i].status_message[0]
            ? known[i].status_message
            : (known[i].ok ? "Ready for queued jobs" : "Saved printer");
        print_add_printer(st, known[i].uri, known[i].display_name, status, known[i].last_seen, known[i].ok, known[i].is_default);
    }

    if (st->printer_count <= 0) {
        char default_uri[256] = {};
        bool have_default = print::read_default_printer_uri(default_uri, sizeof(default_uri));
        print::ProbeState probe = {};
        bool have_probe = print::load_printer_probe_state(&probe);

        if (have_probe && probe.printer_uri[0]) {
            const char* status = probe.message[0] ? probe.message
                : (probe.caps.status_message[0] ? probe.caps.status_message
                   : (probe.ok ? "Ready for queued jobs" : "Saved printer"));
            print_add_printer(st, probe.printer_uri, probe.caps.printer_name, status, probe.probed_at,
                              probe.ok, have_default && montauk::streq(default_uri, probe.printer_uri));
        } else if (have_default) {
            print_add_printer(st, default_uri, "Configured printer", "Saved by the print spooler", "", false, true);
        }
    }

    if (preferred_uri[0]) st->selected_printer = print_find_printer(st, preferred_uri);
    if (st->selected_printer < 0) {
        for (int i = 0; i < st->printer_count; i++) {
            if (st->printers[i].is_default) {
                st->selected_printer = i;
                break;
            }
        }
    }
    if (st->selected_printer < 0 && st->printer_count > 0) st->selected_printer = 0;

    st->source_ready = !st->submit_mode || (st->source_path[0] && path_exists_via_open(st->source_path));
    print_update_note(st);
}

void init_print_dialog(const dlg::Request& req) {
    PrintDialogState* st = &g_app.print;
    montauk::memset(st, 0, sizeof(*st));
    Color def = colors::ICON_COLOR;
    st->printer_icon = svg_load("0:/icons/printer-symbolic.svg", 24, 24, def);
    safe_copy(st->source_path, sizeof(st->source_path), req.source_path);
    safe_copy(st->job_name, sizeof(st->job_name), req.job_name);
    st->submit_mode = req.mode == dlg::PRINT_DIALOG_SUBMIT;
    st->copies = req.copies > 0 ? req.copies : 1;
    print_refresh(st);
}

void draw_print_dialog() {
    PrintDialogState* st = &g_app.print;
    Canvas c = g_app.win.canvas();
    c.fill(colors::WINDOW_BG);
    PrintDialogLayout lo = print_layout();
    const auto* selected = (st->selected_printer >= 0 && st->selected_printer < st->printer_count)
        ? &st->printers[st->selected_printer]
        : nullptr;

    c.fill_rounded_rect(lo.printers_rect.x, lo.printers_rect.y, lo.printers_rect.w, lo.printers_rect.h, 6, colors::WHITE);
    c.rect(lo.printers_rect.x, lo.printers_rect.y, lo.printers_rect.w, lo.printers_rect.h, colors::BORDER);
    c.text(lo.printers_rect.x + 12, lo.printers_rect.y + 8, "Printers", Color::from_rgb(0x77, 0x77, 0x77));

    if (st->printer_count <= 0) {
        c.text(lo.printers_rect.x + 12, lo.printers_rect.y + 38, "No printers discovered", colors::TEXT_COLOR);
        c.text(lo.printers_rect.x + 12, lo.printers_rect.y + 58, "Open Printers to configure one.", Color::from_rgb(0x77, 0x77, 0x77));
    } else {
        for (int i = 0; i < st->printer_count; i++) {
            Rect row = print_row_rect(lo, i);
            bool selected = i == st->selected_printer;
            bool hovered = row.contains(st->mouse_x, st->mouse_y);
            Color bg = selected ? colors::MENU_HOVER
                                : (hovered ? Color::from_rgb(0xF6, 0xF6, 0xF6) : colors::WHITE);
            c.fill_rounded_rect(row.x, row.y, row.w, row.h, 6, bg);
            c.rect(row.x, row.y, row.w, row.h, selected ? colors::ACCENT : Color::from_rgb(0xE4, 0xE4, 0xE4));
            if (st->printer_icon.pixels) c.icon(row.x + 10, row.y + 14, st->printer_icon);

            char display[128];
            if (st->printers[i].is_default && st->printers[i].name[0])
                snprintf(display, sizeof(display), "%s (Default)", st->printers[i].name);
            else if (st->printers[i].is_default)
                snprintf(display, sizeof(display), "Default printer");
            else
                safe_copy(display, sizeof(display), st->printers[i].name[0] ? st->printers[i].name : st->printers[i].uri);

            char title[112];
            fit_text_end(display, title, sizeof(title), row.w - 68);
            c.text(row.x + 42, row.y + 9, title, colors::TEXT_COLOR);

            char subtitle[256];
            safe_copy(subtitle, sizeof(subtitle), st->printers[i].uri[0] ? st->printers[i].uri : "Saved printer");
            fit_text_end(subtitle, title, sizeof(title), row.w - 68);
            c.text(row.x + 42, row.y + 28, title, Color::from_rgb(0x77, 0x77, 0x77));
        }
    }

    c.fill_rounded_rect(lo.detail_rect.x, lo.detail_rect.y, lo.detail_rect.w, lo.detail_rect.h, 6, colors::WHITE);
    c.rect(lo.detail_rect.x, lo.detail_rect.y, lo.detail_rect.w, lo.detail_rect.h, colors::BORDER);

    int dx = lo.detail_rect.x + 16;
    int dy = lo.detail_rect.y + 18;
    c.text(dx, dy, "Document", Color::from_rgb(0x77, 0x77, 0x77));
    char line[256];
    const char* job_label = st->job_name[0] ? st->job_name
        : (st->source_path[0] ? path_basename(st->source_path) : "Untitled document");
    fit_text_end(job_label, line, sizeof(line), lo.detail_rect.w - 32);
    c.text(dx, dy + 20, line, colors::TEXT_COLOR);

    dy += 68;
    c.text(dx, dy, "Printer", Color::from_rgb(0x77, 0x77, 0x77));
    if (selected) {
        const char* title = selected->name[0] ? selected->name : selected->uri;
        fit_text_end(title, line, sizeof(line), lo.detail_rect.w - 32);
        c.text(dx, dy + 18, line, colors::TEXT_COLOR);

        const char* subtitle = selected->uri[0] ? selected->uri : selected->status;
        fit_text_end(subtitle, line, sizeof(line), lo.detail_rect.w - 32);
        c.text(dx, dy + 36, line, Color::from_rgb(0x77, 0x77, 0x77));
    } else {
        c.text(dx, dy + 18, "No printer selected", Color::from_rgb(0x77, 0x77, 0x77));
    }

    dy += 86;
    c.text(dx, dy, "Copies", Color::from_rgb(0x77, 0x77, 0x77));
    draw_action_button(c, lo.copy_minus_btn, "-", st->copies > 1,
                       lo.copy_minus_btn.contains(st->mouse_x, st->mouse_y), false);
    c.fill_rounded_rect(lo.copy_value_rect.x, lo.copy_value_rect.y, lo.copy_value_rect.w, lo.copy_value_rect.h, 4, Color::from_rgb(0xF6, 0xF6, 0xF6));
    c.rect(lo.copy_value_rect.x, lo.copy_value_rect.y, lo.copy_value_rect.w, lo.copy_value_rect.h, colors::BORDER);
    snprintf(line, sizeof(line), "%u %s", (unsigned)st->copies, st->copies == 1 ? "copy" : "copies");
    c.text(lo.copy_value_rect.x + 10, lo.copy_value_rect.y + 7, line, colors::TEXT_COLOR);
    draw_action_button(c, lo.copy_plus_btn, "+", st->copies < 99,
                       lo.copy_plus_btn.contains(st->mouse_x, st->mouse_y), false);

    if (st->error[0]) {
        char line[160];
        fit_text_end(st->error, line, sizeof(line), lo.cancel_btn.x - 24);
        c.text(16, lo.cancel_btn.y - 22, line, Color::from_rgb(0xC0, 0x33, 0x33));
    }

    draw_action_button(c, lo.refresh_btn, "Refresh", true, lo.refresh_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, lo.printers_btn, "Printers", true, lo.printers_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, lo.cancel_btn, "Cancel", true, lo.cancel_btn.contains(st->mouse_x, st->mouse_y), false);
    draw_action_button(c, lo.confirm_btn, "Print", print_can_confirm(st),
                       lo.confirm_btn.contains(st->mouse_x, st->mouse_y), true);
}

void print_finish_setup(PrintDialogState* st) {
    if (!st || st->selected_printer < 0 || st->selected_printer >= st->printer_count) {
        print_set_error(st, "Choose a printer to continue");
        return;
    }

    const auto& printer = st->printers[st->selected_printer];
    dialog_finish("ok", "", "", "Printer selected",
                  printer.uri, printer.name, st->copies);
}

void print_submit(PrintDialogState* st) {
    if (!st || st->selected_printer < 0 || st->selected_printer >= st->printer_count) {
        print_set_error(st, "Choose a printer to print");
        return;
    }
    if (!st->source_ready) {
        print_set_error(st, "Source document is unavailable");
        return;
    }

    const auto& printer = st->printers[st->selected_printer];
    char job_id[64] = {};
    char err[160] = {};
    const char* job_name = st->job_name[0] ? st->job_name : path_basename(st->source_path);
    if (!print::submit_document_job(st->source_path, printer.uri, job_name,
                                    job_id, sizeof(job_id), err, sizeof(err),
                                    (int)(st->copies > 0 ? st->copies : 1))) {
        print_set_error(st, err[0] ? err : "Failed to queue print job");
        return;
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "Queued as %s", job_id);
    dialog_finish("ok", "", job_id, msg, printer.uri, printer.name, st->copies);
}

void text_insert(char* buf, int* len, int* cursor, int max_len, char ch) {
    if (!buf || !len || !cursor || *len >= max_len - 1) return;
    for (int i = *len; i > *cursor; i--) buf[i] = buf[i - 1];
    buf[*cursor] = ch;
    (*len)++;
    (*cursor)++;
    buf[*len] = '\0';
}

void text_backspace(char* buf, int* len, int* cursor) {
    if (!buf || !len || !cursor || *cursor <= 0 || *len <= 0) return;
    for (int i = *cursor - 1; i < *len - 1; i++) buf[i] = buf[i + 1];
    (*len)--;
    (*cursor)--;
    buf[*len] = '\0';
}

void text_delete(char* buf, int* len, int* cursor) {
    if (!buf || !len || !cursor || *cursor >= *len || *len <= 0) return;
    for (int i = *cursor; i < *len - 1; i++) buf[i] = buf[i + 1];
    (*len)--;
    buf[*len] = '\0';
}

int file_entry_at(FileDialogState* st, int mx, int my) {
    FileDialogLayout lo = file_layout(st);
    if (!lo.content_rect.contains(mx, my)) return -1;

    if (st->grid_view) {
        if (mx >= lo.content_rect.w - SCROLLBAR_W) return -1;
        int cols = (lo.content_rect.w - SCROLLBAR_W) / GRID_CELL_W;
        if (cols < 1) cols = 1;
        int col = mx / GRID_CELL_W;
        int row = (my - lo.content_rect.y + st->scrollbar.scroll_offset) / GRID_CELL_H;
        int idx = row * cols + col;
        if (idx >= 0 && idx < st->entry_count && col < cols) return idx;
        return -1;
    }

    int rows_y = lo.content_rect.y + HEADER_H;
    if (my < rows_y || mx >= lo.content_rect.w - SCROLLBAR_W) return -1;
    int rel_y = my - rows_y + st->scrollbar.scroll_offset;
    int idx = rel_y / ITEM_H;
    if (idx >= 0 && idx < st->entry_count) return idx;
    return -1;
}

void handle_file_mouse(const Montauk::WinEvent& ev) {
    FileDialogState* st = &g_app.file;
    st->mouse_x = ev.mouse.x;
    st->mouse_y = ev.mouse.y;

    FileDialogLayout lo = file_layout(st);
    MouseEvent me = {ev.mouse.x, ev.mouse.y, ev.mouse.buttons, ev.mouse.prev_buttons, ev.mouse.scroll};
    st->scrollbar.handle_mouse(me);

    if (me.left_pressed()) {
        st->error[0] = '\0';

        if (lo.back_btn.contains(me.x, me.y)) { file_go_back(st); return; }
        if (lo.forward_btn.contains(me.x, me.y)) { file_go_forward(st); return; }
        if (lo.up_btn.contains(me.x, me.y)) { file_go_up(st); return; }
        if (lo.home_btn.contains(me.x, me.y)) { file_go_home(st); return; }
        if (lo.view_btn.contains(me.x, me.y)) {
            st->grid_view = !st->grid_view;
            st->scrollbar.scroll_offset = 0;
            return;
        }
        if (lo.cancel_btn.contains(me.x, me.y)) { dialog_cancel(); return; }
        if (lo.confirm_btn.contains(me.x, me.y)) {
            if (g_app.request.mode == dlg::FILE_DIALOG_SAVE) file_accept_save(st);
            else file_accept_open(st);
            return;
        }
        if (lo.path_rect.contains(me.x, me.y)) {
            st->focus = FOCUS_PATH;
            return;
        }
        if (g_app.request.mode == dlg::FILE_DIALOG_SAVE && lo.name_rect.contains(me.x, me.y)) {
            st->focus = FOCUS_NAME;
            return;
        }

        int idx = file_entry_at(st, me.x, me.y);
        if (idx >= 0) {
            uint64_t now = montauk::get_milliseconds();
            st->focus = FOCUS_LIST;
            if (st->last_click_item == idx && (now - st->last_click_time) < 400) {
                st->selected = idx;
                file_activate_selected(st, true);
                st->last_click_item = -1;
                st->last_click_time = 0;
            } else {
                st->selected = idx;
                st->last_click_item = idx;
                st->last_click_time = now;
                if (g_app.request.mode == dlg::FILE_DIALOG_SAVE && !st->is_dir[idx]) {
                    file_set_filename(st, st->entry_names[idx]);
                }
            }
            file_ensure_selected_visible(st);
            return;
        }

        st->selected = -1;
        st->focus = FOCUS_LIST;
    }

    if (me.scroll != 0 && lo.content_rect.contains(me.x, me.y)) {
        int step = st->grid_view ? GRID_CELL_H : ITEM_H;
        st->scrollbar.scroll_offset -= me.scroll * step;
        int ms = st->scrollbar.max_scroll();
        if (st->scrollbar.scroll_offset < 0) st->scrollbar.scroll_offset = 0;
        if (st->scrollbar.scroll_offset > ms) st->scrollbar.scroll_offset = ms;
    }
}

void handle_file_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return;

    FileDialogState* st = &g_app.file;
    st->error[0] = '\0';

    if (key.scancode == 0x01) {
        dialog_cancel();
        return;
    }

    if (st->focus == FOCUS_PATH) {
        if (key.ascii == '\n' || key.ascii == '\r') {
            file_open_path(st, st->path_buf);
            st->focus = FOCUS_LIST;
            return;
        }
        if (key.ascii == '\b' || key.scancode == 0x0E) {
            text_backspace(st->path_buf, &st->path_len, &st->path_cursor);
            return;
        }
        if (key.scancode == 0x53) { text_delete(st->path_buf, &st->path_len, &st->path_cursor); return; }
        if (key.scancode == 0x4B) { if (st->path_cursor > 0) st->path_cursor--; return; }
        if (key.scancode == 0x4D) { if (st->path_cursor < st->path_len) st->path_cursor++; return; }
        if (key.scancode == 0x47) { st->path_cursor = 0; return; }
        if (key.scancode == 0x4F) { st->path_cursor = st->path_len; return; }
        if (key.ascii >= 32 && key.ascii < 127) {
            text_insert(st->path_buf, &st->path_len, &st->path_cursor, (int)sizeof(st->path_buf), key.ascii);
            return;
        }
        return;
    }

    if (st->focus == FOCUS_NAME && g_app.request.mode == dlg::FILE_DIALOG_SAVE) {
        if (key.ascii == '\n' || key.ascii == '\r') { file_accept_save(st); return; }
        if (key.ascii == '\b' || key.scancode == 0x0E) {
            text_backspace(st->filename_buf, &st->filename_len, &st->filename_cursor);
            return;
        }
        if (key.scancode == 0x53) { text_delete(st->filename_buf, &st->filename_len, &st->filename_cursor); return; }
        if (key.scancode == 0x4B) { if (st->filename_cursor > 0) st->filename_cursor--; return; }
        if (key.scancode == 0x4D) { if (st->filename_cursor < st->filename_len) st->filename_cursor++; return; }
        if (key.scancode == 0x47) { st->filename_cursor = 0; return; }
        if (key.scancode == 0x4F) { st->filename_cursor = st->filename_len; return; }
        if (key.ascii >= 32 && key.ascii < 127 && key.ascii != '/' && key.ascii != '\\') {
            text_insert(st->filename_buf, &st->filename_len, &st->filename_cursor, (int)sizeof(st->filename_buf), key.ascii);
            return;
        }
        return;
    }

    if (key.ascii == '\n' || key.ascii == '\r') {
        if (g_app.request.mode == dlg::FILE_DIALOG_SAVE) file_accept_save(st);
        else file_accept_open(st);
        return;
    }

    if (key.scancode == 0x4B || key.scancode == 0x48) {
        if (st->selected > 0) st->selected--;
        else if (st->entry_count > 0) st->selected = 0;
        file_ensure_selected_visible(st);
        return;
    }

    if (key.scancode == 0x4D || key.scancode == 0x50) {
        if (st->selected < st->entry_count - 1) st->selected++;
        else if (st->entry_count > 0) st->selected = st->entry_count - 1;
        file_ensure_selected_visible(st);
        return;
    }

    if (key.ctrl && (key.ascii == 'l' || key.ascii == 'L')) {
        st->focus = FOCUS_PATH;
        return;
    }

    if (key.ctrl && g_app.request.mode == dlg::FILE_DIALOG_SAVE && (key.ascii == 'n' || key.ascii == 'N')) {
        st->focus = FOCUS_NAME;
        return;
    }
}

void handle_print_mouse(const Montauk::WinEvent& ev) {
    PrintDialogState* st = &g_app.print;
    st->mouse_x = ev.mouse.x;
    st->mouse_y = ev.mouse.y;

    MouseEvent me = {ev.mouse.x, ev.mouse.y, ev.mouse.buttons, ev.mouse.prev_buttons, ev.mouse.scroll};
    if (!me.left_pressed()) return;

    PrintDialogLayout lo = print_layout();
    if (lo.cancel_btn.contains(me.x, me.y)) { dialog_cancel(); return; }
    if (lo.refresh_btn.contains(me.x, me.y)) { print_refresh(st); return; }
    if (lo.printers_btn.contains(me.x, me.y)) { montauk::spawn("0:/apps/printers/printers.elf"); return; }
    if (lo.copy_minus_btn.contains(me.x, me.y) && st->copies > 1) {
        print_adjust_copies(st, -1);
        st->error[0] = '\0';
        return;
    }
    if (lo.copy_plus_btn.contains(me.x, me.y) && st->copies < 99) {
        print_adjust_copies(st, +1);
        st->error[0] = '\0';
        return;
    }
    for (int i = 0; i < st->printer_count; i++) {
        Rect row = print_row_rect(lo, i);
        if (row.contains(me.x, me.y)) {
            st->selected_printer = i;
            st->error[0] = '\0';
            return;
        }
    }
    if (lo.confirm_btn.contains(me.x, me.y) && print_can_confirm(st)) {
        if (st->submit_mode) print_submit(st);
        else print_finish_setup(st);
    }
}

void handle_print_key(const Montauk::KeyEvent& key) {
    if (!key.pressed) return;
    if (key.scancode == 0x01) {
        dialog_cancel();
        return;
    }
    if ((key.ascii == '\n' || key.ascii == '\r') && print_can_confirm(&g_app.print)) {
        if (g_app.print.submit_mode) print_submit(&g_app.print);
        else print_finish_setup(&g_app.print);
    }
    if (key.ctrl && (key.ascii == 'r' || key.ascii == 'R')) {
        print_refresh(&g_app.print);
        return;
    }
    if ((key.ascii == '+' || key.ascii == '=') && g_app.print.copies < 99) {
        print_adjust_copies(&g_app.print, +1);
        g_app.print.error[0] = '\0';
        return;
    }
    if ((key.ascii == '-' || key.ascii == '_') && g_app.print.copies > 1) {
        print_adjust_copies(&g_app.print, -1);
        g_app.print.error[0] = '\0';
        return;
    }
    if ((key.scancode == 0x48 || key.scancode == 0x4B) && g_app.print.printer_count > 0) {
        if (g_app.print.selected_printer > 0) g_app.print.selected_printer--;
        else g_app.print.selected_printer = 0;
        g_app.print.error[0] = '\0';
        return;
    }
    if ((key.scancode == 0x50 || key.scancode == 0x4D) && g_app.print.printer_count > 0) {
        if (g_app.print.selected_printer < g_app.print.printer_count - 1) g_app.print.selected_printer++;
        else g_app.print.selected_printer = g_app.print.printer_count - 1;
        g_app.print.error[0] = '\0';
    }
}

void cleanup() {
    if (g_app.is_print) {
        free_icon(&g_app.print.printer_icon);
    } else {
        free_file_icons(&g_app.file);
    }
    g_app.win.destroy();
}

} // namespace

extern "C" void _start() {
    if (!fonts::init()) montauk::exit(1);

    char argbuf[256] = {};
    if (montauk::getargs(argbuf, sizeof(argbuf)) <= 0 || !argbuf[0]) montauk::exit(1);
    if (!dlg::read_request_file(argbuf, &g_app.request)) montauk::exit(1);

    g_app.running = true;
    dlg::reset_result(&g_app.result);

    g_app.is_print = g_app.request.kind == dlg::REQUEST_KIND_PRINT;
    const int init_w = g_app.is_print ? PRINT_INIT_W : FILE_INIT_W;
    const int init_h = g_app.is_print ? PRINT_INIT_H : FILE_INIT_H;
    const char* title = g_app.request.title[0] ? g_app.request.title : (g_app.is_print ? "Print" : "Open");

    if (g_app.is_print) init_print_dialog(g_app.request);
    else init_file_dialog(g_app.request);

    if (!g_app.win.create(title, init_w, init_h)) montauk::exit(1);

    if (g_app.is_print) draw_print_dialog();
    else draw_file_dialog();
    g_app.win.present();

    while (g_app.running) {
        if (g_app.is_print) draw_print_dialog();
        else draw_file_dialog();
        g_app.win.present();

        Montauk::WinEvent ev;
        int rc = g_app.win.poll(&ev);
        if (rc < 0) {
            dialog_cancel();
            break;
        }
        if (rc == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) {
            dialog_cancel();
            break;
        }

        if (ev.type == 2 || ev.type == 4) {
            continue;
        }

        if (ev.type == 1) {
            if (g_app.is_print) handle_print_mouse(ev);
            else handle_file_mouse(ev);
            continue;
        }

        if (ev.type == 0) {
            if (g_app.is_print) handle_print_key(ev.key);
            else handle_file_key(ev.key);
        }
    }

    cleanup();
    montauk::exit(0);
}
