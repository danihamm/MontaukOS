/*
 * main.cpp
 * MontaukOS Word Processor
 * Copyright (c) 2026 Daniel Hammer
 */

#include "wordprocessor.hpp"

int g_win_w = INIT_W;
int g_win_h = INIT_H;
WsWindow g_win;
WordProcessorState g_wp = {};
WPFontTable g_wp_fonts = { {{nullptr}}, false };
SvgIcon g_icon_folder = {};
SvgIcon g_icon_save = {};
SvgIcon g_icon_print = {};
SvgIcon g_icon_export_pdf = {};
SvgIcon g_icon_undo = {};
SvgIcon g_icon_redo = {};
SvgIcon g_icon_align_left = {};
SvgIcon g_icon_align_center = {};
SvgIcon g_icon_align_right = {};
SvgIcon g_icon_list_bullet = {};
SvgIcon g_icon_list_number = {};
SvgIcon g_icon_indent_less = {};
SvgIcon g_icon_indent_more = {};
TrueTypeFont* g_ui_font = nullptr;
TrueTypeFont* g_ui_bold = nullptr;

void wp_load_icons() {
    Color def_color = colors::ICON_COLOR;
    if (!g_icon_folder.pixels)
        g_icon_folder = svg_load("0:/icons/folder.svg", 16, 16, def_color);
    if (!g_icon_save.pixels)
        g_icon_save = svg_load("0:/icons/document-save-symbolic.svg", 16, 16, def_color);
    if (!g_icon_print.pixels)
        g_icon_print = svg_load("0:/icons/printer-symbolic.svg", 16, 16, def_color);
    if (!g_icon_export_pdf.pixels) {
        g_icon_export_pdf = svg_load("0:/icons/document-export-symbolic.svg", 16, 16, def_color);
        if (!g_icon_export_pdf.pixels)
            g_icon_export_pdf = svg_load("0:/icons/text-x-generic-symbolic.svg", 16, 16, def_color);
    }
    if (!g_icon_undo.pixels)
        g_icon_undo = svg_load("0:/icons/edit-undo-symbolic.svg", 16, 16, def_color);
    if (!g_icon_redo.pixels)
        g_icon_redo = svg_load("0:/icons/edit-redo-symbolic.svg", 16, 16, def_color);
    if (!g_icon_align_left.pixels)
        g_icon_align_left = svg_load("0:/icons/format-justify-left-symbolic.svg", 16, 16, def_color);
    if (!g_icon_align_center.pixels)
        g_icon_align_center = svg_load("0:/icons/format-justify-center-symbolic.svg", 16, 16, def_color);
    if (!g_icon_align_right.pixels)
        g_icon_align_right = svg_load("0:/icons/format-justify-right-symbolic.svg", 16, 16, def_color);
    if (!g_icon_list_bullet.pixels)
        g_icon_list_bullet = svg_load("0:/icons/view-list-bullet-symbolic.svg", 16, 16, def_color);
    if (!g_icon_list_number.pixels)
        g_icon_list_number = svg_load("0:/icons/view-list-ordered-symbolic.svg", 16, 16, def_color);
    if (!g_icon_indent_less.pixels)
        g_icon_indent_less = svg_load("0:/icons/format-indent-less-symbolic.svg", 16, 16, def_color);
    if (!g_icon_indent_more.pixels)
        g_icon_indent_more = svg_load("0:/icons/format-indent-more-symbolic.svg", 16, 16, def_color);
}

void wp_cleanup_state() {
    wp_free_document(&g_wp);
    if (g_icon_folder.pixels) svg_free(g_icon_folder);
    if (g_icon_save.pixels) svg_free(g_icon_save);
    if (g_icon_print.pixels) svg_free(g_icon_print);
    if (g_icon_export_pdf.pixels) svg_free(g_icon_export_pdf);
    if (g_icon_undo.pixels) svg_free(g_icon_undo);
    if (g_icon_redo.pixels) svg_free(g_icon_redo);
    if (g_icon_align_left.pixels) svg_free(g_icon_align_left);
    if (g_icon_align_center.pixels) svg_free(g_icon_align_center);
    if (g_icon_align_right.pixels) svg_free(g_icon_align_right);
    if (g_icon_list_bullet.pixels) svg_free(g_icon_list_bullet);
    if (g_icon_list_number.pixels) svg_free(g_icon_list_number);
    if (g_icon_indent_less.pixels) svg_free(g_icon_indent_less);
    if (g_icon_indent_more.pixels) svg_free(g_icon_indent_more);
}

extern "C" void _start() {
    if (!fonts::init())
        montauk::exit(1);

    wp_load_fonts();
    wp_load_icons();
    wp_init_empty_document(&g_wp);

    char args[512] = {};
    int arglen = montauk::getargs(args, sizeof(args));
    if (arglen > 0 && args[0]) {
        wp_load_file(&g_wp, args);
    }

    char title[64] = "Word Processor";
    if (g_wp.filename[0]) {
        snprintf(title, sizeof(title), "%s - Word Processor", g_wp.filename);
    }

    if (!g_win.create(title, INIT_W, INIT_H))
        montauk::exit(1);

    g_win_w = g_win.width;
    g_win_h = g_win.height;

    wp_render();
    g_win.present();

    while (true) {
        Montauk::WinEvent ev;
        int r = g_win.poll(&ev);

        if (r < 0) break;

        if (r == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) break;

        if (ev.type == 2 || ev.type == 4) {
            g_win_w = g_win.width;
            g_win_h = g_win.height;
            wp_render();
            g_win.present();
            continue;
        }

        if (ev.type == 0 && ev.key.pressed) {
            wp_handle_key(ev.key);
            wp_render();
            g_win.present();
            continue;
        }

        if (ev.type == 1) {
            wp_handle_mouse(ev);
            wp_render();
            g_win.present();
        }
    }

    wp_cleanup_state();
    g_win.destroy();
    montauk::exit(0);
}
