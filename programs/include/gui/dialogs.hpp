/*
    * dialogs.hpp
    * Shared modal dialog protocol and client helpers
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <cstdint>
#include <stdio.h>
#include <montauk/syscall.h>
#include <montauk/string.h>
#include <Api/Syscall.hpp>

namespace gui::dialogs {

inline constexpr const char* APP_BINARY = "0:/apps/dialogs/dialogs.elf";
inline constexpr const char* STORAGE_DIR = "0:/tmp/dialogs";

enum RequestKind : uint8_t {
    REQUEST_KIND_FILE = 1,
    REQUEST_KIND_PRINT = 2,
};

enum FileDialogMode : uint8_t {
    FILE_DIALOG_OPEN = 1,
    FILE_DIALOG_SAVE = 2,
};

enum PrintDialogMode : uint8_t {
    PRINT_DIALOG_SETUP = 1,
    PRINT_DIALOG_SUBMIT = 2,
};

struct Request {
    uint8_t kind;
    uint8_t mode;
    char title[96];
    char initial_path[256];
    char suggested_name[128];
    char source_path[256];
    char job_name[128];
    char printer_uri[256];
    uint32_t copies;
    char result_path[256];
};

struct Result {
    char status[16];
    char path[256];
    char job_id[64];
    char printer_uri[256];
    char printer_name[128];
    uint32_t copies;
    char message[160];
};

inline void safe_copy(char* dst, int dst_len, const char* src) {
    if (!dst || dst_len <= 0) return;
    if (!src) src = "";
    montauk::strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

inline void reset_request(Request* req) {
    if (!req) return;
    montauk::memset(req, 0, sizeof(Request));
}

inline void reset_result(Result* res) {
    if (!res) return;
    montauk::memset(res, 0, sizeof(Result));
}

inline void ensure_storage_dirs() {
    montauk::fmkdir("0:/tmp");
    montauk::fmkdir(STORAGE_DIR);
}

inline bool write_text_file(const char* path, const char* text) {
    if (!path || !path[0] || !text) return false;
    int fd = montauk::fcreate(path);
    if (fd < 0) return false;
    int len = montauk::slen(text);
    bool ok = montauk::fwrite(fd, (const uint8_t*)text, 0, (uint64_t)len) >= 0;
    montauk::close(fd);
    return ok;
}

inline bool read_text_file(const char* path, char* out, int out_len) {
    if (!out || out_len <= 0) return false;
    out[0] = '\0';
    if (!path || !path[0]) return false;
    int fd = montauk::open(path);
    if (fd < 0) return false;
    uint64_t size = montauk::getsize(fd);
    if ((int)size >= out_len) size = (uint64_t)(out_len - 1);
    int got = montauk::read(fd, (uint8_t*)out, 0, size);
    montauk::close(fd);
    if (got < 0) return false;
    out[got] = '\0';
    return true;
}

inline bool line_value(const char* text, const char* key, char* out, int out_len) {
    if (!text || !key || !out || out_len <= 0) return false;

    int key_len = montauk::slen(key);
    const char* p = text;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line);
        if (*p == '\n') p++;

        if (line_len <= key_len || line[key_len] != '=') continue;

        bool match = true;
        for (int i = 0; i < key_len; i++) {
            if (line[i] != key[i]) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        const char* value = line + key_len + 1;
        int value_len = line_len - key_len - 1;
        while (value_len > 0 && (value[value_len - 1] == '\r' || value[value_len - 1] == '\n'))
            value_len--;
        if (value_len >= out_len) value_len = out_len - 1;
        montauk::memcpy(out, value, (uint64_t)value_len);
        out[value_len] = '\0';
        return true;
    }

    return false;
}

inline uint32_t parse_u32(const char* text) {
    if (!text) return 0;
    uint32_t value = 0;
    for (int i = 0; text[i] >= '0' && text[i] <= '9'; i++) {
        value = value * 10u + (uint32_t)(text[i] - '0');
    }
    return value;
}

inline bool write_request_file(const char* path, const Request* req) {
    if (!path || !req) return false;
    char text[1536];
    int n = snprintf(text, sizeof(text),
                     "kind=%u\n"
                     "mode=%u\n"
                     "title=%s\n"
                     "initial_path=%s\n"
                     "suggested_name=%s\n"
                     "source_path=%s\n"
                     "job_name=%s\n"
                     "printer_uri=%s\n"
                     "copies=%u\n"
                     "result_path=%s\n",
                     (unsigned)req->kind,
                     (unsigned)req->mode,
                     req->title,
                     req->initial_path,
                     req->suggested_name,
                     req->source_path,
                     req->job_name,
                     req->printer_uri,
                     (unsigned)req->copies,
                     req->result_path);
    if (n <= 0 || n >= (int)sizeof(text)) return false;
    return write_text_file(path, text);
}

inline bool read_request_file(const char* path, Request* req) {
    if (!path || !req) return false;
    char text[1536];
    if (!read_text_file(path, text, sizeof(text))) return false;

    reset_request(req);
    char value[256];
    if (line_value(text, "kind", value, sizeof(value))) req->kind = (uint8_t)parse_u32(value);
    if (line_value(text, "mode", value, sizeof(value))) req->mode = (uint8_t)parse_u32(value);
    line_value(text, "title", req->title, sizeof(req->title));
    line_value(text, "initial_path", req->initial_path, sizeof(req->initial_path));
    line_value(text, "suggested_name", req->suggested_name, sizeof(req->suggested_name));
    line_value(text, "source_path", req->source_path, sizeof(req->source_path));
    line_value(text, "job_name", req->job_name, sizeof(req->job_name));
    line_value(text, "printer_uri", req->printer_uri, sizeof(req->printer_uri));
    if (line_value(text, "copies", value, sizeof(value))) req->copies = parse_u32(value);
    line_value(text, "result_path", req->result_path, sizeof(req->result_path));
    return req->kind != 0 && req->result_path[0] != '\0';
}

inline bool write_result_file(const char* path, const Result* res) {
    if (!path || !res) return false;
    char text[1024];
    int n = snprintf(text, sizeof(text),
                     "status=%s\n"
                     "path=%s\n"
                     "job_id=%s\n"
                     "printer_uri=%s\n"
                     "printer_name=%s\n"
                     "copies=%u\n"
                     "message=%s\n",
                     res->status,
                     res->path,
                     res->job_id,
                     res->printer_uri,
                     res->printer_name,
                     (unsigned)res->copies,
                     res->message);
    if (n <= 0 || n >= (int)sizeof(text)) return false;
    return write_text_file(path, text);
}

inline bool read_result_file(const char* path, Result* res) {
    if (!path || !res) return false;
    char text[1024];
    if (!read_text_file(path, text, sizeof(text))) return false;

    reset_result(res);
    char value[256];
    line_value(text, "status", res->status, sizeof(res->status));
    line_value(text, "path", res->path, sizeof(res->path));
    line_value(text, "job_id", res->job_id, sizeof(res->job_id));
    line_value(text, "printer_uri", res->printer_uri, sizeof(res->printer_uri));
    line_value(text, "printer_name", res->printer_name, sizeof(res->printer_name));
    if (line_value(text, "copies", value, sizeof(value))) res->copies = parse_u32(value);
    line_value(text, "message", res->message, sizeof(res->message));
    return res->status[0] != '\0';
}

inline void make_unique_paths(char* request_path, int request_path_len,
                              char* result_path, int result_path_len) {
    static uint32_t seq = 0;
    ensure_storage_dirs();
    uint64_t now = montauk::get_milliseconds();
    int pid = montauk::getpid();
    uint32_t cur = ++seq;
    snprintf(request_path, request_path_len, "%s/%d-%llu-%u.req",
             STORAGE_DIR, pid, (unsigned long long)now, (unsigned)cur);
    snprintf(result_path, result_path_len, "%s/%d-%llu-%u.res",
             STORAGE_DIR, pid, (unsigned long long)now, (unsigned)cur);
}

inline bool run_request(const Request* req, Result* out_res, char* out_message, int out_message_len) {
    if (out_message && out_message_len > 0) out_message[0] = '\0';
    if (!req) {
        if (out_message && out_message_len > 0) safe_copy(out_message, out_message_len, "invalid dialog request");
        return false;
    }

    char request_path[256];
    char result_path[256];
    make_unique_paths(request_path, sizeof(request_path), result_path, sizeof(result_path));

    Request req_copy = *req;
    safe_copy(req_copy.result_path, sizeof(req_copy.result_path), result_path);

    if (!write_request_file(request_path, &req_copy)) {
        if (out_message && out_message_len > 0) safe_copy(out_message, out_message_len, "failed to write dialog request");
        return false;
    }

    int pid = montauk::spawn(APP_BINARY, request_path);
    if (pid < 0) {
        montauk::fdelete(request_path);
        if (out_message && out_message_len > 0) safe_copy(out_message, out_message_len, "failed to start dialog");
        return false;
    }

    int proc = montauk::proc_open(pid);
    if (proc >= 0) {
        montauk::wait_handle(proc, Montauk::IPC_SIGNAL_EXITED);
        montauk::close(proc);
    } else {
        montauk::waitpid(pid);
    }

    Result local_res = {};
    bool ok = read_result_file(result_path, &local_res);

    montauk::fdelete(request_path);
    montauk::fdelete(result_path);

    if (!ok) {
        if (out_message && out_message_len > 0) safe_copy(out_message, out_message_len, "dialog did not return a result");
        return false;
    }

    if (out_res) *out_res = local_res;
    if (out_message && out_message_len > 0) safe_copy(out_message, out_message_len, local_res.message);
    return montauk::streq(local_res.status, "ok");
}

inline bool open_file(const char* title,
                      const char* initial_path,
                      char* out_path, int out_path_len,
                      char* out_message = nullptr, int out_message_len = 0) {
    Request req = {};
    req.kind = REQUEST_KIND_FILE;
    req.mode = FILE_DIALOG_OPEN;
    safe_copy(req.title, sizeof(req.title), title ? title : "Open");
    safe_copy(req.initial_path, sizeof(req.initial_path), initial_path);

    Result res = {};
    bool ok = run_request(&req, &res, out_message, out_message_len);
    if (ok && out_path && out_path_len > 0) safe_copy(out_path, out_path_len, res.path);
    return ok;
}

inline bool save_file(const char* title,
                      const char* initial_path,
                      const char* suggested_name,
                      char* out_path, int out_path_len,
                      char* out_message = nullptr, int out_message_len = 0) {
    Request req = {};
    req.kind = REQUEST_KIND_FILE;
    req.mode = FILE_DIALOG_SAVE;
    safe_copy(req.title, sizeof(req.title), title ? title : "Save");
    safe_copy(req.initial_path, sizeof(req.initial_path), initial_path);
    safe_copy(req.suggested_name, sizeof(req.suggested_name), suggested_name);

    Result res = {};
    bool ok = run_request(&req, &res, out_message, out_message_len);
    if (ok && out_path && out_path_len > 0) safe_copy(out_path, out_path_len, res.path);
    return ok;
}

inline bool print_file(const char* title,
                       const char* source_path,
                       const char* job_name,
                       char* out_job_id, int out_job_id_len,
                       char* out_message = nullptr, int out_message_len = 0) {
    Request req = {};
    req.kind = REQUEST_KIND_PRINT;
    req.mode = PRINT_DIALOG_SUBMIT;
    req.copies = 1;
    safe_copy(req.title, sizeof(req.title), title ? title : "Print");
    safe_copy(req.source_path, sizeof(req.source_path), source_path);
    safe_copy(req.job_name, sizeof(req.job_name), job_name);

    Result res = {};
    bool ok = run_request(&req, &res, out_message, out_message_len);
    if (ok && out_job_id && out_job_id_len > 0) safe_copy(out_job_id, out_job_id_len, res.job_id);
    return ok;
}

inline bool configure_print(const char* title,
                            const char* initial_printer_uri,
                            const char* job_name,
                            char* out_printer_uri, int out_printer_uri_len,
                            char* out_printer_name, int out_printer_name_len,
                            uint32_t* out_copies = nullptr,
                            char* out_message = nullptr, int out_message_len = 0) {
    Request req = {};
    req.kind = REQUEST_KIND_PRINT;
    req.mode = PRINT_DIALOG_SETUP;
    req.copies = out_copies && *out_copies > 0 ? *out_copies : 1;
    safe_copy(req.title, sizeof(req.title), title ? title : "Print");
    safe_copy(req.job_name, sizeof(req.job_name), job_name ? job_name : "");
    safe_copy(req.printer_uri, sizeof(req.printer_uri), initial_printer_uri);

    Result res = {};
    bool ok = run_request(&req, &res, out_message, out_message_len);
    if (ok && out_printer_uri && out_printer_uri_len > 0) safe_copy(out_printer_uri, out_printer_uri_len, res.printer_uri);
    if (ok && out_printer_name && out_printer_name_len > 0) safe_copy(out_printer_name, out_printer_name_len, res.printer_name);
    if (ok && out_copies) *out_copies = res.copies > 0 ? res.copies : 1;
    return ok;
}

} // namespace gui::dialogs
