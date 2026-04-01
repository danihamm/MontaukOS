/*
    * filemanager_internal.hpp
    * Shared declarations for the embedded desktop file manager
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include "../apps_common.hpp"

namespace filemanager {

inline constexpr int FM_MAX_DRIVES = 16;

struct FileManagerState {
    char current_path[256];
    char history[16][256];
    int history_pos;
    int history_count;
    char entry_names[64][64];
    int entry_types[64];   // 0=file, 1=dir, 2=exec, 3=drive, 4=home, 5=apps, 6=app, 7=special_dir
    int entry_sizes[64];
    int entry_count;
    int selected;
    int scroll_offset;
    bool is_dir[64];
    int last_click_item;
    uint64_t last_click_time;
    Scrollbar scrollbar;
    DesktopState* desktop;
    bool grid_view;
    bool at_drives_root;
    int drive_indices[FM_MAX_DRIVES]; // which drive number each entry maps to

    // Clipboard
    char clipboard_path[256];
    bool clipboard_has_data;
    bool clipboard_is_cut;

    // Context menu
    bool ctx_open;
    int ctx_x, ctx_y;
    int ctx_target_idx; // -1 = background
    int ctx_items[8];
    int ctx_item_count;
    int ctx_hover;

    // Rename
    bool rename_active;
    int rename_idx;
    char rename_buf[64];
    int rename_cursor;
    int rename_len;

    // Path bar editing
    bool pathbar_editing;
    char pathbar_buf[256];
    int pathbar_cursor;
    int pathbar_len;

    // Apps view
    bool at_apps_view;
    int app_map[16];           // entry index -> external_apps[] index
    SvgIcon app_icons_lg[16];  // 48x48 icons for grid view
    SvgIcon app_icons_sm[16];  // 16x16 icons for list view
    int app_icon_count;
};

inline constexpr int FM_TOOLBAR_H = 32;
inline constexpr int FM_PATHBAR_H = 24;
inline constexpr int FM_HEADER_H = 20;
inline constexpr int FM_ITEM_H = 24;
inline constexpr int FM_SCROLLBAR_W = 12;
inline constexpr int FM_GRID_CELL_W = 80;
inline constexpr int FM_GRID_CELL_H = 80;
inline constexpr int FM_GRID_ICON = 48;
inline constexpr int FM_GRID_PAD = 4;

// Special user folders: name, icon filename, index into DesktopState arrays
inline constexpr int SF_COUNT = DesktopState::SPECIAL_FOLDER_COUNT;
inline constexpr const char* sf_names[SF_COUNT] = {
    "Documents", "Desktop", "Music", "Videos", "Pictures", "Downloads"
};
inline constexpr const char* sf_icons[SF_COUNT] = {
    "folder-blue-documents.svg", "folder-blue-desktop.svg",
    "folder-blue-music.svg", "folder-blue-videos.svg",
    "folder-blue-pictures.svg", "folder-blue-downloads.svg"
};

// Context menu action codes
inline constexpr int CTX_OPEN = 0;
inline constexpr int CTX_COPY = 1;
inline constexpr int CTX_CUT = 2;
inline constexpr int CTX_PASTE = 3;
inline constexpr int CTX_RENAME = 4;
inline constexpr int CTX_DELETE = 5;
inline constexpr int CTX_NEW_FOLDER = 6;

inline constexpr int CTX_MENU_W = 140;
inline constexpr int CTX_ITEM_H = 24;

int special_folder_index(const char* name);
const char* ctx_label(int action);

bool str_ends_with(const char* s, const char* suffix);
bool is_image_file(const char* name);
bool is_font_file(const char* name);
bool is_pdf_file(const char* name);
bool is_spreadsheet_file(const char* name);
bool is_wordprocessor_file(const char* name);
bool is_audio_file(const char* name);
bool is_video_file(const char* name);
int detect_file_type(const char* name, bool is_dir);

void filemanager_build_fullpath(char* out, int out_max, const char* dir, const char* name);
const char* path_basename(const char* path);

void filemanager_read_drives(FileManagerState* fm);
void filemanager_read_dir(FileManagerState* fm);
void filemanager_free_app_icons(FileManagerState* fm);
void filemanager_read_apps(FileManagerState* fm);

bool filemanager_delete_recursive(const char* path);
bool filemanager_copy_file(const char* src, const char* dst);
bool filemanager_copy_dir_recursive(const char* src, const char* dst);

void filemanager_do_copy(FileManagerState* fm);
void filemanager_do_cut(FileManagerState* fm);
void filemanager_do_paste(FileManagerState* fm);

void filemanager_start_rename(FileManagerState* fm);
void filemanager_finish_rename(FileManagerState* fm);
void filemanager_cancel_rename(FileManagerState* fm);
void filemanager_new_folder(FileManagerState* fm);
void filemanager_delete_selected(FileManagerState* fm);

void filemanager_open_ctx_menu(FileManagerState* fm, int local_x, int local_y, int target_idx);
void filemanager_close_ctx_menu(FileManagerState* fm);

void filemanager_push_history(FileManagerState* fm);
void filemanager_navigate(FileManagerState* fm, const char* name);
void filemanager_go_up(FileManagerState* fm);
void filemanager_navigate_to_history(FileManagerState* fm);
void filemanager_go_back(FileManagerState* fm);
void filemanager_go_forward(FileManagerState* fm);
void filemanager_go_home(FileManagerState* fm);

void filemanager_start_pathbar(FileManagerState* fm);
void filemanager_cancel_pathbar(FileManagerState* fm);
void filemanager_commit_pathbar(FileManagerState* fm);

void filemanager_open_entry(FileManagerState* fm, int idx);

void filemanager_draw_header(Canvas& c, FileManagerState* fm, Color toolbar_color, Color btn_bg);
void filemanager_on_draw(Window* win, Framebuffer& fb);
void filemanager_on_mouse(Window* win, MouseEvent& ev);
void filemanager_on_key(Window* win, const Montauk::KeyEvent& key);
void filemanager_on_close(Window* win);

} // namespace filemanager
