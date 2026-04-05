/*
    * main.cpp
    * Shared modal dialogs for MontaukOS desktop apps
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <gui/gui.hpp>
#include <gui/standalone.hpp>
#include <gui/font.hpp>
#include <gui/dialogs.hpp>

#include "main.hpp"
#include "filedialog.hpp"
#include "printdialog.hpp"

extern "C" {
#include <stdio.h>
}

using namespace gui;
namespace dlg = gui::dialogs;

AppState g_app = {};

void dialog_finish(const char* status, const char* path, const char* job_id, const char* message,
                   const char* printer_uri, const char* printer_name, uint32_t copies) {
    gui::dialogs::safe_copy(g_app.result.status, sizeof(g_app.result.status), status);
    gui::dialogs::safe_copy(g_app.result.path, sizeof(g_app.result.path), path);
    gui::dialogs::safe_copy(g_app.result.job_id, sizeof(g_app.result.job_id), job_id);
    gui::dialogs::safe_copy(g_app.result.printer_uri, sizeof(g_app.result.printer_uri), printer_uri);
    gui::dialogs::safe_copy(g_app.result.printer_name, sizeof(g_app.result.printer_name), printer_name);
    g_app.result.copies = copies;
    gui::dialogs::safe_copy(g_app.result.message, sizeof(g_app.result.message), message);
    gui::dialogs::write_result_file(g_app.request.result_path, &g_app.result);
    g_app.running = false;
}

void dialog_cancel(const char* message) {
    dialog_finish("cancel", "", "", message, "", "", 0);
}

void cleanup() {
    if (g_app.is_print) {
        printdialog::cleanup();
    } else {
        filedialog::cleanup();
    }
    g_app.win.destroy();
}

extern "C" void _start() {
    if (!fonts::init()) montauk::exit(1);

    char argbuf[256] = {};
    if (montauk::getargs(argbuf, sizeof(argbuf)) <= 0 || !argbuf[0]) montauk::exit(1);
    if (!gui::dialogs::read_request_file(argbuf, &g_app.request)) montauk::exit(1);

    g_app.running = true;
    gui::dialogs::reset_result(&g_app.result);

    g_app.is_print = g_app.request.kind == gui::dialogs::REQUEST_KIND_PRINT;
    const int init_w = g_app.is_print ? printdialog::PRINT_INIT_W : filedialog::FILE_INIT_W;
    const int init_h = g_app.is_print ? printdialog::PRINT_INIT_H : filedialog::FILE_INIT_H;
    const char* title = g_app.request.title[0] ? g_app.request.title : (g_app.is_print ? "Print" : "Open");

    if (g_app.is_print) printdialog::init(g_app.request);
    else filedialog::init(g_app.request);

    if (!g_app.win.create(title, init_w, init_h)) montauk::exit(1);

    if (g_app.is_print) printdialog::draw();
    else filedialog::draw();
    g_app.win.present();

    while (g_app.running) {
        if (g_app.is_print) printdialog::draw();
        else filedialog::draw();
        g_app.win.present();

        Montauk::WinEvent ev;
        int rc = g_app.win.poll(&ev);
        if (rc < 0) {
            dialog_cancel("");
            break;
        }
        if (rc == 0) {
            montauk::sleep_ms(16);
            continue;
        }

        if (ev.type == 3) {
            dialog_cancel("");
            break;
        }

        if (ev.type == 2 || ev.type == 4) {
            continue;
        }

        if (ev.type == 1) {
            if (g_app.is_print) printdialog::handle_mouse(ev);
            else filedialog::handle_mouse(ev);
            continue;
        }

        if (ev.type == 0) {
            if (g_app.is_print) printdialog::handle_key(ev.key);
            else filedialog::handle_key(ev.key);
        }
    }

    cleanup();
    montauk::exit(0);
}