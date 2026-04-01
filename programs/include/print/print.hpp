/*
 * print.hpp
 * Shared helpers for the MontaukOS userspace print stack
 * Copyright (c) 2026 Daniel Hammer
 */

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <http/http.hpp>
#include <tls/tls.hpp>

extern "C" {
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
}

namespace print {

static constexpr const char* CONFIG_DIR              = "0:/config/printing";
static constexpr const char* DEFAULT_PRINTER_PATH    = "0:/config/printing/default-printer-uri";
static constexpr const char* SPOOL_ROOT              = "0:/spool/print";
static constexpr const char* SPOOL_QUEUE_DIR         = "0:/spool/print/queue";
static constexpr const char* SPOOL_ACTIVE_DIR        = "0:/spool/print/active";
static constexpr const char* SPOOL_DONE_DIR          = "0:/spool/print/done";
static constexpr const char* SPOOL_FAILED_DIR        = "0:/spool/print/failed";
static constexpr const char* SPOOL_DOCS_DIR          = "0:/spool/print/docs";
static constexpr const char* SPOOL_TMP_DIR           = "0:/spool/print/tmp";
static constexpr const char* SPOOL_DAEMON_PID_PATH   = "0:/spool/print/daemon.pid";
static constexpr const char* PRINTD_BINARY           = "0:/os/printd.elf";
static constexpr int MAX_PATH_LEN                    = 256;
static constexpr int MAX_JOB_NAME_LEN                = 128;
static constexpr int MAX_STATUS_LEN                  = 192;
static constexpr int MAX_DEBUG_LEN                   = 320;
static constexpr int MAX_DOC_BYTES                   = 4 * 1024 * 1024;
static constexpr int HTTP_RESPONSE_MAX               = 64 * 1024;
static constexpr uint64_t JOB_WAIT_SLICE_MS          = 250;
static constexpr uint64_t DEFAULT_WAIT_TIMEOUT_MS    = 120000;

struct JobMeta {
    char id[48];
    char state[16];
    char source_kind[16];
    char job_name[MAX_JOB_NAME_LEN];
    char user_name[64];
    char printer_uri[MAX_PATH_LEN];
    char doc_path[MAX_PATH_LEN];
    char doc_format[64];
    char source_name[128];
    char created_at[32];
    char updated_at[32];
    char status_message[MAX_STATUS_LEN];
    char debug_info[MAX_DEBUG_LEN];
    int remote_job_id;
    int size_bytes;
};

struct IppUri {
    bool use_tls;
    char host[128];
    uint16_t port;
    char path[128];
    char normalized[MAX_PATH_LEN];
    uint32_t ip;
};

struct IppCapabilities {
    bool ok;
    bool supports_pdf;
    bool supports_text;
    bool supports_jpeg;
    bool use_tls;
    int http_status;
    uint16_t ipp_status;
    int request_id;
    uint32_t resolved_ip;
    uint16_t port;
    char host[128];
    char path[128];
    char printer_name[128];
    char status_message[128];
    char supported_formats[192];
    char default_format[64];
    char preferred_format[64];
};

struct IppPrintResult {
    bool ok;
    bool use_tls;
    int http_status;
    uint16_t ipp_status;
    int request_id;
    int job_id;
    uint32_t resolved_ip;
    uint16_t port;
    char host[128];
    char path[128];
    char document_format[64];
    char job_uri[MAX_PATH_LEN];
    char status_message[128];
};

struct ArgList {
    int count;
    char* argv[24];
    char storage[512];
};

struct ByteBuilder {
    uint8_t* data;
    int len;
    int cap;
};

inline void zero_job(JobMeta* job) {
    if (job) memset(job, 0, sizeof(*job));
}

inline void safe_copy(char* dst, int dst_len, const char* src) {
    if (dst == nullptr || dst_len <= 0) return;
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, (size_t)dst_len, "%s", src);
}

inline void format_ipv4(char* out, int out_len, uint32_t ip) {
    if (out == nullptr || out_len <= 0) return;
    snprintf(out, (size_t)out_len, "%u.%u.%u.%u",
             (unsigned)(ip & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 24) & 0xFFu));
}

inline void trim_line(char* s) {
    if (s == nullptr) return;
    int len = (int)strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            s[--len] = '\0';
            continue;
        }
        break;
    }
}

inline void sanitize_field(char* s) {
    if (s == nullptr) return;
    for (int i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\r' || s[i] == '\n') s[i] = ' ';
    }
}

inline void now_string(char* out, int out_len) {
    if (out == nullptr || out_len <= 0) return;
    Montauk::DateTime dt = {};
    montauk::gettime(&dt);
    snprintf(out, (size_t)out_len, "%04d-%02d-%02d %02d:%02d:%02d",
             (int)dt.Year, (int)dt.Month, (int)dt.Day,
             (int)dt.Hour, (int)dt.Minute, (int)dt.Second);
}

inline bool file_exists(const char* path) {
    struct stat st = {};
    return path != nullptr && stat(path, &st) == 0;
}

inline bool dir_exists(const char* path) {
    struct stat st = {};
    return path != nullptr && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool ensure_dir(const char* path) {
    if (path == nullptr || *path == '\0') return false;
    if (mkdir(path, 0755) == 0) return true;
    return dir_exists(path);
}

inline bool ensure_spool_dirs() {
    return ensure_dir("0:/config")
        && ensure_dir(CONFIG_DIR)
        && ensure_dir("0:/spool")
        && ensure_dir(SPOOL_ROOT)
        && ensure_dir(SPOOL_QUEUE_DIR)
        && ensure_dir(SPOOL_ACTIVE_DIR)
        && ensure_dir(SPOOL_DONE_DIR)
        && ensure_dir(SPOOL_FAILED_DIR)
        && ensure_dir(SPOOL_DOCS_DIR)
        && ensure_dir(SPOOL_TMP_DIR);
}

inline bool path_join(char* out, int out_len, const char* a, const char* b) {
    if (out == nullptr || out_len <= 0 || a == nullptr || b == nullptr) return false;
    if (*a == '\0') {
        safe_copy(out, out_len, b);
        return true;
    }
    size_t alen = strlen(a);
    if (alen > 0 && a[alen - 1] == '/')
        snprintf(out, (size_t)out_len, "%s%s", a, b);
    else
        snprintf(out, (size_t)out_len, "%s/%s", a, b);
    return true;
}

inline const char* basename_ptr(const char* path) {
    if (path == nullptr) return "";
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

inline void make_job_file_path(const char* dir, const char* id, char* out, int out_len) {
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "%s.job", id);
    path_join(out, out_len, dir, leaf);
}

inline void make_doc_file_path(const char* id, char* out, int out_len) {
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "%s.bin", id);
    path_join(out, out_len, SPOOL_DOCS_DIR, leaf);
}

inline bool read_text_file_line(const char* path, char* out, int out_len) {
    if (path == nullptr || out == nullptr || out_len <= 0) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    out[0] = '\0';
    bool ok = fgets(out, out_len, f) != nullptr;
    fclose(f);
    if (ok) trim_line(out);
    return ok;
}

inline bool write_text_file_atomic(const char* path, const char* text) {
    if (path == nullptr || text == nullptr) return false;
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    size_t want = strlen(text);
    bool ok = fwrite(text, 1, want, f) == want;
    fclose(f);
    if (!ok) {
        remove(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

inline bool read_file_bytes(const char* path, uint8_t** out_data, int* out_len, char* err, int err_len) {
    if (out_data) *out_data = nullptr;
    if (out_len) *out_len = 0;
    if (path == nullptr) {
        safe_copy(err, err_len, "missing path");
        return false;
    }

    struct stat st = {};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        safe_copy(err, err_len, "file not found");
        return false;
    }
    if ((int)st.st_size < 0 || st.st_size > MAX_DOC_BYTES) {
        safe_copy(err, err_len, "document is too large");
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        safe_copy(err, err_len, "failed to open file");
        return false;
    }

    int size = (int)st.st_size;
    uint8_t* data = (uint8_t*)malloc((size_t)(size > 0 ? size : 1));
    if (!data) {
        fclose(f);
        safe_copy(err, err_len, "out of memory");
        return false;
    }

    bool ok = true;
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        ok = false;
    }
    fclose(f);

    if (!ok) {
        free(data);
        safe_copy(err, err_len, "failed to read file");
        return false;
    }

    if (out_data) *out_data = data;
    if (out_len) *out_len = size;
    return true;
}

inline bool write_file_atomic(const char* path, const uint8_t* data, int len, char* err, int err_len) {
    if (path == nullptr || len < 0) {
        safe_copy(err, err_len, "invalid write parameters");
        return false;
    }

    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        safe_copy(err, err_len, "failed to create temporary file");
        return false;
    }

    bool ok = true;
    if (len > 0 && fwrite(data, 1, (size_t)len, f) != (size_t)len) ok = false;
    fclose(f);

    if (!ok) {
        remove(tmp);
        safe_copy(err, err_len, "failed to write file");
        return false;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        safe_copy(err, err_len, "failed to rename file");
        return false;
    }
    return true;
}

inline bool copy_file_atomic(const char* src, const char* dst, int* out_size, char* err, int err_len) {
    uint8_t* data = nullptr;
    int len = 0;
    if (!read_file_bytes(src, &data, &len, err, err_len)) return false;
    bool ok = write_file_atomic(dst, data, len, err, err_len);
    free(data);
    if (ok && out_size) *out_size = len;
    return ok;
}

inline bool read_default_printer_uri(char* out, int out_len) {
    return read_text_file_line(DEFAULT_PRINTER_PATH, out, out_len);
}

inline bool write_default_printer_uri(const char* uri) {
    ensure_spool_dirs();
    return write_text_file_atomic(DEFAULT_PRINTER_PATH, uri);
}

inline bool parse_args(ArgList* out) {
    if (out == nullptr) return false;
    memset(out, 0, sizeof(*out));
    int len = montauk::getargs(out->storage, sizeof(out->storage));
    if (len < 0) len = 0;
    if (len >= (int)sizeof(out->storage)) len = (int)sizeof(out->storage) - 1;
    out->storage[len] = '\0';

    char* p = out->storage;
    while (*p && out->count < (int)(sizeof(out->argv) / sizeof(out->argv[0]))) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p;
            p++;
        }

        out->argv[out->count++] = p;
        if (quote) {
            while (*p && *p != quote) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p == '\0') break;
        *p++ = '\0';
    }
    return true;
}

inline bool parse_u16(const char* s, uint16_t* out) {
    if (s == nullptr || *s == '\0' || out == nullptr) return false;
    unsigned long val = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        val = val * 10 + (unsigned long)(*p - '0');
        if (val > 65535UL) return false;
    }
    *out = (uint16_t)val;
    return true;
}

inline bool parse_ipv4_literal(const char* s, uint32_t* out) {
    if (s == nullptr || out == nullptr) return false;
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    const char* p = s;
    while (*p) {
        if (part >= 4) return false;
        if (*p < '0' || *p > '9') return false;
        uint32_t value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10u + (uint32_t)(*p - '0');
            if (value > 255u) return false;
            p++;
        }
        parts[part++] = value;
        if (*p == '.') {
            p++;
            continue;
        }
        if (*p != '\0') return false;
    }
    if (part != 4) return false;
    *out = (parts[0] & 0xFFu)
         | ((parts[1] & 0xFFu) << 8)
         | ((parts[2] & 0xFFu) << 16)
         | ((parts[3] & 0xFFu) << 24);
    return true;
}

inline bool resolve_host(const char* host, uint32_t* out_ip) {
    if (host == nullptr || out_ip == nullptr) return false;
    if (parse_ipv4_literal(host, out_ip)) return true;
    *out_ip = montauk::resolve(host);
    return *out_ip != 0;
}

inline bool starts_with_ci(const char* s, const char* prefix) {
    if (s == nullptr || prefix == nullptr) return false;
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline bool str_ieq(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

inline bool ends_with_ci(const char* s, const char* suffix) {
    if (s == nullptr || suffix == nullptr) return false;
    int slen = (int)strlen(s);
    int tlen = (int)strlen(suffix);
    if (tlen > slen) return false;
    return str_ieq(s + slen - tlen, suffix);
}

inline bool host_looks_like_mdns(const char* host) {
    return host != nullptr && ends_with_ci(host, ".local");
}

inline bool parse_ipp_uri_impl(const char* uri, IppUri* out, char* err, int err_len, bool require_resolve) {
    IppUri local = {};
    if (out == nullptr) out = &local;
    memset(out, 0, sizeof(*out));
    if (uri == nullptr || *uri == '\0') {
        safe_copy(err, err_len, "missing printer URI");
        return false;
    }

    const char* sep = strstr(uri, "://");
    if (!sep) {
        safe_copy(err, err_len, "printer URI must include a scheme");
        return false;
    }

    char scheme[8] = {};
    size_t scheme_len = (size_t)(sep - uri);
    if (scheme_len == 0 || scheme_len >= sizeof(scheme)) {
        safe_copy(err, err_len, "invalid printer URI scheme");
        return false;
    }
    memcpy(scheme, uri, scheme_len);

    bool use_tls = false;
    uint16_t port = 0;
    if (str_ieq(scheme, "ipp")) {
        use_tls = false;
        port = 631;
    } else if (str_ieq(scheme, "ipps")) {
        use_tls = true;
        port = 631;
    } else if (str_ieq(scheme, "http")) {
        use_tls = false;
        port = 80;
    } else if (str_ieq(scheme, "https")) {
        use_tls = true;
        port = 443;
    } else {
        safe_copy(err, err_len, "unsupported printer URI scheme");
        return false;
    }

    const char* host = sep + 3;
    if (*host == '\0') {
        safe_copy(err, err_len, "printer URI is missing a host");
        return false;
    }

    const char* host_end = host;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t host_len = (size_t)(host_end - host);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        safe_copy(err, err_len, "printer host is invalid");
        return false;
    }

    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';
    out->use_tls = use_tls;

    const char* path = host_end;
    if (*host_end == ':') {
        char port_buf[8] = {};
        const char* port_start = host_end + 1;
        const char* port_end = port_start;
        while (*port_end && *port_end != '/') port_end++;
        size_t port_len = (size_t)(port_end - port_start);
        if (port_len == 0 || port_len >= sizeof(port_buf)) {
            safe_copy(err, err_len, "printer port is invalid");
            return false;
        }
        memcpy(port_buf, port_start, port_len);
        if (!parse_u16(port_buf, &port)) {
            safe_copy(err, err_len, "printer port is invalid");
            return false;
        }
        path = port_end;
    }

    out->port = port;
    if (*path == '\0') safe_copy(out->path, sizeof(out->path), "/ipp/print");
    else safe_copy(out->path, sizeof(out->path), path);
    if (out->path[0] != '/') {
        char tmp[sizeof(out->path)];
        snprintf(tmp, sizeof(tmp), "/%s", out->path);
        safe_copy(out->path, sizeof(out->path), tmp);
    }

    if (require_resolve && !resolve_host(out->host, &out->ip)) {
        char msg[160];
        if (host_looks_like_mdns(out->host))
            snprintf(msg, sizeof(msg),
                     "could not resolve \"%s\" (.local/mDNS not supported yet)",
                     out->host);
        else
            snprintf(msg, sizeof(msg), "could not resolve printer host \"%s\"", out->host);
        safe_copy(err, err_len, msg);
        return false;
    }

    snprintf(out->normalized, sizeof(out->normalized), "%s://%s:%u%s",
             out->use_tls ? "ipps" : "ipp", out->host, (unsigned)out->port, out->path);
    return true;
}

inline bool parse_ipp_uri(const char* uri, IppUri* out, char* err, int err_len) {
    return parse_ipp_uri_impl(uri, out, err, err_len, true);
}

inline bool normalize_ipp_uri(const char* uri, IppUri* out, char* err, int err_len) {
    return parse_ipp_uri_impl(uri, out, err, err_len, false);
}

inline bool infer_doc_format(const char* path, char* out, int out_len) {
    if (path == nullptr || out == nullptr || out_len <= 0) return false;
    const char* base = basename_ptr(path);
    const char* dot = strrchr(base, '.');
    if (!dot) {
        safe_copy(out, out_len, "application/octet-stream");
        return true;
    }
    if (str_ieq(dot, ".pdf")) safe_copy(out, out_len, "application/pdf");
    else if (str_ieq(dot, ".jpg") || str_ieq(dot, ".jpeg")) safe_copy(out, out_len, "image/jpeg");
    else if (str_ieq(dot, ".urf")) safe_copy(out, out_len, "image/urf");
    else if (str_ieq(dot, ".pwg")) safe_copy(out, out_len, "image/pwg-raster");
    else if (str_ieq(dot, ".txt") || str_ieq(dot, ".log") || str_ieq(dot, ".md") || str_ieq(dot, ".csv"))
        safe_copy(out, out_len, "text/plain");
    else if (str_ieq(dot, ".ps")) safe_copy(out, out_len, "application/postscript");
    else safe_copy(out, out_len, "application/octet-stream");
    return true;
}

inline void generate_job_id(char* out, int out_len) {
    uint64_t now = montauk::get_milliseconds();
    uint32_t rnd = 0;
    if (montauk::getrandom(&rnd, sizeof(rnd)) < 0) {
        rnd = (uint32_t)(now ^ (uint64_t)montauk::getpid());
    }
    snprintf(out, (size_t)out_len, "%llu-%04x",
             (unsigned long long)now,
             (unsigned)(rnd & 0xFFFFu));
}

inline bool save_job_to_path_atomic(const char* path, const JobMeta* job) {
    if (path == nullptr || job == nullptr) return false;
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "wb");
    if (!f) return false;

    JobMeta copy = *job;
    sanitize_field(copy.job_name);
    sanitize_field(copy.user_name);
    sanitize_field(copy.printer_uri);
    sanitize_field(copy.doc_path);
    sanitize_field(copy.doc_format);
    sanitize_field(copy.source_name);
    sanitize_field(copy.created_at);
    sanitize_field(copy.updated_at);
    sanitize_field(copy.status_message);
    sanitize_field(copy.debug_info);

    fprintf(f, "id=%s\n", copy.id);
    fprintf(f, "state=%s\n", copy.state);
    fprintf(f, "source_kind=%s\n", copy.source_kind);
    fprintf(f, "job_name=%s\n", copy.job_name);
    fprintf(f, "user_name=%s\n", copy.user_name);
    fprintf(f, "printer_uri=%s\n", copy.printer_uri);
    fprintf(f, "doc_path=%s\n", copy.doc_path);
    fprintf(f, "doc_format=%s\n", copy.doc_format);
    fprintf(f, "source_name=%s\n", copy.source_name);
    fprintf(f, "created_at=%s\n", copy.created_at);
    fprintf(f, "updated_at=%s\n", copy.updated_at);
    fprintf(f, "status_message=%s\n", copy.status_message);
    fprintf(f, "debug_info=%s\n", copy.debug_info);
    fprintf(f, "remote_job_id=%d\n", copy.remote_job_id);
    fprintf(f, "size_bytes=%d\n", copy.size_bytes);
    fclose(f);

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

inline bool load_job_from_path(const char* path, JobMeta* job) {
    if (path == nullptr || job == nullptr) return false;
    zero_job(job);

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char line[384];
    while (fgets(line, sizeof(line), f) != nullptr) {
        trim_line(line);
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';

        if (strcmp(line, "id") == 0) safe_copy(job->id, sizeof(job->id), eq);
        else if (strcmp(line, "state") == 0) safe_copy(job->state, sizeof(job->state), eq);
        else if (strcmp(line, "source_kind") == 0) safe_copy(job->source_kind, sizeof(job->source_kind), eq);
        else if (strcmp(line, "job_name") == 0) safe_copy(job->job_name, sizeof(job->job_name), eq);
        else if (strcmp(line, "user_name") == 0) safe_copy(job->user_name, sizeof(job->user_name), eq);
        else if (strcmp(line, "printer_uri") == 0) safe_copy(job->printer_uri, sizeof(job->printer_uri), eq);
        else if (strcmp(line, "doc_path") == 0) safe_copy(job->doc_path, sizeof(job->doc_path), eq);
        else if (strcmp(line, "doc_format") == 0) safe_copy(job->doc_format, sizeof(job->doc_format), eq);
        else if (strcmp(line, "source_name") == 0) safe_copy(job->source_name, sizeof(job->source_name), eq);
        else if (strcmp(line, "created_at") == 0) safe_copy(job->created_at, sizeof(job->created_at), eq);
        else if (strcmp(line, "updated_at") == 0) safe_copy(job->updated_at, sizeof(job->updated_at), eq);
        else if (strcmp(line, "status_message") == 0) safe_copy(job->status_message, sizeof(job->status_message), eq);
        else if (strcmp(line, "debug_info") == 0) safe_copy(job->debug_info, sizeof(job->debug_info), eq);
        else if (strcmp(line, "remote_job_id") == 0) job->remote_job_id = atoi(eq);
        else if (strcmp(line, "size_bytes") == 0) job->size_bytes = atoi(eq);
    }
    fclose(f);
    return job->id[0] != '\0';
}

inline bool find_job(const char* id, char* out_path, int out_path_len, char* out_state, int out_state_len) {
    if (id == nullptr || *id == '\0') return false;
    const struct {
        const char* dir;
        const char* state;
    } dirs[] = {
        {SPOOL_QUEUE_DIR, "queued"},
        {SPOOL_ACTIVE_DIR, "printing"},
        {SPOOL_DONE_DIR, "completed"},
        {SPOOL_FAILED_DIR, "failed"},
    };

    char probe[MAX_PATH_LEN];
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        make_job_file_path(dirs[i].dir, id, probe, sizeof(probe));
        if (file_exists(probe)) {
            safe_copy(out_path, out_path_len, probe);
            safe_copy(out_state, out_state_len, dirs[i].state);
            return true;
        }
    }
    return false;
}

inline bool daemon_is_running(int* out_pid) {
    if (out_pid) *out_pid = -1;
    char buf[32];
    if (!read_text_file_line(SPOOL_DAEMON_PID_PATH, buf, sizeof(buf))) return false;
    int pid = atoi(buf);
    if (pid <= 0) return false;

    int handle = montauk::proc_open(pid);
    if (handle < 0) return false;
    uint32_t sig = montauk::wait_handle(handle, Montauk::IPC_SIGNAL_EXITED, 0);
    montauk::close(handle);
    if (sig & Montauk::IPC_SIGNAL_EXITED) return false;
    if (out_pid) *out_pid = pid;
    return true;
}

inline bool ensure_daemon_running(char* err, int err_len) {
    ensure_spool_dirs();
    int pid = -1;
    if (daemon_is_running(&pid)) return true;

    int spawned = montauk::spawn(PRINTD_BINARY);
    if (spawned < 0) {
        safe_copy(err, err_len, "failed to start print daemon");
        return false;
    }

    for (int i = 0; i < 20; i++) {
        montauk::sleep_ms(100);
        if (daemon_is_running(&pid)) return true;
    }

    safe_copy(err, err_len, "print daemon did not start");
    return false;
}

inline int pdf_escape_line(const char* src, char* out, int out_len) {
    int pos = 0;
    for (int i = 0; src[i] != '\0' && pos < out_len - 2; i++) {
        if (src[i] == '(' || src[i] == ')' || src[i] == '\\') out[pos++] = '\\';
        out[pos++] = src[i];
    }
    out[pos] = '\0';
    return pos;
}

inline bool generate_test_page_text(uint8_t** out_data, int* out_len) {
    if (out_data) *out_data = nullptr;
    if (out_len) *out_len = 0;

    char timestamp[32];
    now_string(timestamp, sizeof(timestamp));

    char body[1024];
    int n = snprintf(body, sizeof(body),
                     "MontaukOS Test Page\r\n"
                     "====================\r\n"
                     "\r\n"
                     "This page was generated by the MontaukOS userspace print spooler.\r\n"
                     "Protocol: Internet Printing Protocol (IPP)\r\n"
                     "Generated: %s\r\n"
                     "PID: %d\r\n"
                     "\r\n"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n"
                     "abcdefghijklmnopqrstuvwxyz\r\n"
                     "0123456789 !@#$%%^&*()[]{}<>?/\\\\|+-=_~\r\n",
                     timestamp, montauk::getpid());
    if (n < 0) return false;

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) return false;
    memcpy(buf, body, (size_t)n);

    if (out_data) *out_data = buf;
    if (out_len) *out_len = n;
    return true;
}

inline bool generate_test_page_pdf(uint8_t** out_data, int* out_len) {
    if (out_data) *out_data = nullptr;
    if (out_len) *out_len = 0;

    char timestamp[32];
    now_string(timestamp, sizeof(timestamp));

    const char* lines[] = {
        "MontaukOS Test Page",
        "Printed through the MontaukOS userspace IPP spooler.",
        "Protocol: Internet Printing Protocol (IPP)",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "abcdefghijklmnopqrstuvwxyz",
        "0123456789 !@#$%^&*()[]{}<>?/\\|+-=_~",
        nullptr,
    };

    char escaped[8][192];
    for (int i = 0; i < 6; i++) pdf_escape_line(lines[i], escaped[i], sizeof(escaped[i]));
    pdf_escape_line(timestamp, escaped[6], sizeof(escaped[6]));

    char content[2048];
    int content_len = snprintf(content, sizeof(content),
        "BT\n"
        "/F1 24 Tf\n"
        "72 744 Td\n"
        "(%s) Tj\n"
        "0 -32 Td\n"
        "/F1 12 Tf\n"
        "(%s) Tj\n"
        "0 -18 Td\n"
        "(%s) Tj\n"
        "0 -18 Td\n"
        "(Generated: %s) Tj\n"
        "0 -28 Td\n"
        "(%s) Tj\n"
        "0 -18 Td\n"
        "(%s) Tj\n"
        "0 -18 Td\n"
        "(%s) Tj\n"
        "ET\n",
        escaped[0], escaped[1], escaped[2], escaped[6],
        escaped[3], escaped[4], escaped[5]);
    if (content_len < 0 || content_len >= (int)sizeof(content)) return false;

    int pdf_cap = 4096 + content_len;
    char* pdf = (char*)malloc((size_t)pdf_cap);
    if (!pdf) return false;

    int xref[6] = {0, 0, 0, 0, 0, 0};
    int pos = 0;

    auto appendf = [&](const char* fmt, auto... args) -> bool {
        int wrote = snprintf(pdf + pos, (size_t)(pdf_cap - pos), fmt, args...);
        if (wrote < 0 || pos + wrote >= pdf_cap) return false;
        pos += wrote;
        return true;
    };

    if (!appendf("%%PDF-1.4\n%%MontaukOS\n")) { free(pdf); return false; }

    xref[1] = pos;
    if (!appendf("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) { free(pdf); return false; }
    xref[2] = pos;
    if (!appendf("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) { free(pdf); return false; }
    xref[3] = pos;
    if (!appendf("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n")) { free(pdf); return false; }
    xref[4] = pos;
    if (!appendf("4 0 obj\n<< /Length %d >>\nstream\n", content_len)) { free(pdf); return false; }
    if (pos + content_len + 32 >= pdf_cap) { free(pdf); return false; }
    memcpy(pdf + pos, content, (size_t)content_len);
    pos += content_len;
    if (!appendf("endstream\nendobj\n")) { free(pdf); return false; }
    xref[5] = pos;
    if (!appendf("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) { free(pdf); return false; }

    int xref_pos = pos;
    if (!appendf("xref\n0 6\n")) { free(pdf); return false; }
    if (!appendf("%010d 65535 f \n", 0)) { free(pdf); return false; }
    for (int i = 1; i <= 5; i++) {
        if (!appendf("%010d 00000 n \n", xref[i])) { free(pdf); return false; }
    }
    if (!appendf("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", xref_pos)) { free(pdf); return false; }

    *out_data = (uint8_t*)pdf;
    *out_len = pos;
    return true;
}

inline bool bb_init(ByteBuilder* b, int initial_cap) {
    if (b == nullptr) return false;
    b->len = 0;
    b->cap = initial_cap > 0 ? initial_cap : 512;
    b->data = (uint8_t*)malloc((size_t)b->cap);
    return b->data != nullptr;
}

inline void bb_free(ByteBuilder* b) {
    if (b && b->data) free(b->data);
    if (b) {
        b->data = nullptr;
        b->len = 0;
        b->cap = 0;
    }
}

inline bool bb_ensure(ByteBuilder* b, int extra) {
    if (b == nullptr || extra < 0) return false;
    if (b->len + extra <= b->cap) return true;
    int next = b->cap;
    while (next < b->len + extra) next *= 2;
    uint8_t* data = (uint8_t*)realloc(b->data, (size_t)next);
    if (!data) return false;
    b->data = data;
    b->cap = next;
    return true;
}

inline bool bb_put8(ByteBuilder* b, uint8_t v) {
    if (!bb_ensure(b, 1)) return false;
    b->data[b->len++] = v;
    return true;
}

inline bool bb_put16(ByteBuilder* b, uint16_t v) {
    if (!bb_ensure(b, 2)) return false;
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFFu);
    b->data[b->len++] = (uint8_t)(v & 0xFFu);
    return true;
}

inline bool bb_put32(ByteBuilder* b, uint32_t v) {
    if (!bb_ensure(b, 4)) return false;
    b->data[b->len++] = (uint8_t)((v >> 24) & 0xFFu);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFFu);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFFu);
    b->data[b->len++] = (uint8_t)(v & 0xFFu);
    return true;
}

inline bool bb_put_bytes(ByteBuilder* b, const void* data, int len) {
    if (len < 0 || !bb_ensure(b, len)) return false;
    if (len > 0) memcpy(b->data + b->len, data, (size_t)len);
    b->len += len;
    return true;
}

inline bool bb_put_attr_str(ByteBuilder* b, uint8_t tag, const char* name, const char* value) {
    uint16_t name_len = (uint16_t)strlen(name);
    uint16_t value_len = (uint16_t)strlen(value);
    return bb_put8(b, tag)
        && bb_put16(b, name_len)
        && bb_put_bytes(b, name, name_len)
        && bb_put16(b, value_len)
        && bb_put_bytes(b, value, value_len);
}

inline bool bb_put_attr_int(ByteBuilder* b, uint8_t tag, const char* name, int value) {
    uint16_t name_len = (uint16_t)strlen(name);
    return bb_put8(b, tag)
        && bb_put16(b, name_len)
        && bb_put_bytes(b, name, name_len)
        && bb_put16(b, 4)
        && bb_put32(b, (uint32_t)value);
}

inline uint16_t be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

inline uint32_t next_request_id() {
    static uint32_t request_id = 1;
    return request_id++;
}

inline int send_all_plain(int fd, const uint8_t* data, int len) {
    static constexpr int MAX_SEND_CHUNK = 32768;
    uint64_t deadline = montauk::get_milliseconds() + 15000;
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > MAX_SEND_CHUNK) chunk = MAX_SEND_CHUNK;

        int n = montauk::send(fd, data + off, (uint32_t)chunk);
        if (n > 0) {
            off += n;
            deadline = montauk::get_milliseconds() + 15000;
            continue;
        }
        if (n < 0) return -1;

        uint32_t sig = montauk::wait_handle(fd,
            Montauk::IPC_SIGNAL_WRITABLE | Montauk::IPC_SIGNAL_PEER_CLOSED,
            1000);
        if (sig & Montauk::IPC_SIGNAL_PEER_CLOSED) return -1;
        if (montauk::get_milliseconds() >= deadline) return -1;
        montauk::sleep_ms(1);
    }
    return off;
}

inline bool response_has_no_body(int status) {
    return (status >= 100 && status < 200) || status == 204 || status == 304;
}

inline int parse_content_length_value(const http::Response* resp) {
    char value[32] = {};
    if (!http::get_header(resp, "Content-Length", value, sizeof(value))) return -1;
    char* end = nullptr;
    long n = strtol(value, &end, 10);
    if (end == value || n < 0) return -1;
    return (int)n;
}

inline bool chunked_body_complete(const char* src, int src_len) {
    if (src == nullptr || src_len <= 0) return false;

    int pos = 0;
    while (pos < src_len) {
        int line_start = pos;
        while (pos < src_len && src[pos] != '\n') pos++;
        if (pos >= src_len) return false;

        int line_end = pos;
        pos++;
        while (line_end > line_start && (src[line_end - 1] == '\r' || src[line_end - 1] == '\n'))
            line_end--;

        char hex[16] = {};
        int hex_pos = 0;
        for (int i = line_start; i < line_end && hex_pos < (int)sizeof(hex) - 1; i++) {
            if (src[i] == ';') break;
            hex[hex_pos++] = src[i];
        }
        if (hex_pos == 0) return false;

        char* end = nullptr;
        unsigned long chunk = strtoul(hex, &end, 16);
        if (end == hex) return false;

        if (chunk == 0) {
            if (pos >= src_len) return false;
            if (src[pos] == '\n') return true;
            if (src[pos] == '\r' && pos + 1 < src_len && src[pos + 1] == '\n') return true;
            return http::find_header_block_end(src + pos, src + src_len) != nullptr;
        }

        if (pos + (int)chunk > src_len) return false;
        pos += (int)chunk;
        if (pos < src_len && src[pos] == '\r') pos++;
        if (pos < src_len && src[pos] == '\n') pos++;
    }

    return false;
}

inline bool response_is_chunked(const http::Response* resp);

inline bool http_response_complete(char* buf, int len, bool peer_closed) {
    if (buf == nullptr || len <= 0) return false;

    http::Response resp = {};
    if (http::parse_response(buf, len, &resp) < 0) return false;
    if (resp.body == nullptr) return false;
    if (response_has_no_body(resp.status)) return true;
    if (response_is_chunked(&resp)) return chunked_body_complete(resp.body, resp.body_len);

    int content_length = parse_content_length_value(&resp);
    if (content_length >= 0) return resp.body_len >= content_length;
    return peer_closed;
}

inline int recv_http_plain(int fd, char* buf, int cap) {
    int total = 0;
    bool peer_closed = false;
    uint64_t deadline = montauk::get_milliseconds() + 90000;

    while (total < cap - 1) {
        if (http_response_complete(buf, total, peer_closed)) break;
        if (montauk::get_milliseconds() >= deadline) break;

        uint32_t sig = montauk::wait_handle(fd,
            Montauk::IPC_SIGNAL_READABLE | Montauk::IPC_SIGNAL_PEER_CLOSED,
            1000);
        if (sig == 0) continue;

        if (sig & Montauk::IPC_SIGNAL_PEER_CLOSED)
            peer_closed = true;
        if (!(sig & Montauk::IPC_SIGNAL_READABLE)) {
            if (peer_closed) break;
            continue;
        }

        int n = montauk::recv(fd, buf + total, (uint32_t)(cap - 1 - total));
        if (n < 0) break;
        if (n == 0) {
            if (peer_closed) break;
            continue;
        }
        total += n;
        deadline = montauk::get_milliseconds() + 90000;
    }
    buf[total] = '\0';
    return total;
}

inline bool response_is_chunked(const http::Response* resp) {
    char value[64] = {};
    if (!http::get_header(resp, "Transfer-Encoding", value, sizeof(value))) return false;
    for (int i = 0; value[i]; i++) {
        if (value[i] >= 'A' && value[i] <= 'Z') value[i] = (char)(value[i] - 'A' + 'a');
    }
    return strstr(value, "chunked") != nullptr;
}

inline bool extract_http_body(const http::Response* resp, uint8_t** out_body, int* out_len) {
    if (out_body) *out_body = nullptr;
    if (out_len) *out_len = 0;
    if (resp == nullptr || resp->body == nullptr || resp->body_len < 0) return false;

    if (!response_is_chunked(resp)) {
        uint8_t* body = (uint8_t*)malloc((size_t)(resp->body_len > 0 ? resp->body_len : 1));
        if (!body) return false;
        if (resp->body_len > 0) memcpy(body, resp->body, (size_t)resp->body_len);
        if (out_body) *out_body = body;
        if (out_len) *out_len = resp->body_len;
        return true;
    }

    const char* src = resp->body;
    int src_len = resp->body_len;
    int pos = 0;
    int out_pos = 0;
    uint8_t* body = (uint8_t*)malloc((size_t)src_len);
    if (!body) return false;

    while (pos < src_len) {
        int line_start = pos;
        while (pos < src_len && src[pos] != '\n') pos++;
        int line_end = pos;
        if (pos < src_len && src[pos] == '\n') pos++;
        while (line_end > line_start && (src[line_end - 1] == '\r' || src[line_end - 1] == '\n'))
            line_end--;

        char hex[16] = {};
        int hex_pos = 0;
        for (int i = line_start; i < line_end && hex_pos < (int)sizeof(hex) - 1; i++) {
            if (src[i] == ';') break;
            hex[hex_pos++] = src[i];
        }
        unsigned long chunk = strtoul(hex, nullptr, 16);
        if (chunk == 0) {
            if (out_body) *out_body = body;
            if (out_len) *out_len = out_pos;
            return true;
        }
        if (pos + (int)chunk > src_len) break;
        memcpy(body + out_pos, src + pos, chunk);
        out_pos += (int)chunk;
        pos += (int)chunk;
        if (pos < src_len && src[pos] == '\r') pos++;
        if (pos < src_len && src[pos] == '\n') pos++;
    }

    free(body);
    return false;
}

inline bool ipp_http_post(const IppUri* uri,
                          const uint8_t* body, int body_len,
                          uint8_t** out_body, int* out_body_len,
                          int* out_http_status,
                          char* err, int err_len) {
    if (out_body) *out_body = nullptr;
    if (out_body_len) *out_body_len = 0;
    if (out_http_status) *out_http_status = -1;
    if (uri == nullptr || body == nullptr || body_len < 0) {
        safe_copy(err, err_len, "invalid IPP request");
        return false;
    }

    char ip_text[20];
    format_ipv4(ip_text, sizeof(ip_text), uri->ip);

    char host_header[160];
    if (uri->port == 631 || uri->port == 80 || uri->port == 443)
        safe_copy(host_header, sizeof(host_header), uri->host);
    else
        snprintf(host_header, sizeof(host_header), "%s:%u", uri->host, (unsigned)uri->port);

    int req_cap = body_len + 1024;
    char* req = (char*)malloc((size_t)req_cap);
    if (!req) {
        safe_copy(err, err_len, "out of memory");
        return false;
    }

    int req_len = snprintf(req, (size_t)req_cap,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: MontaukOS Print/1.0\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        uri->path, host_header, body_len);
    if (req_len < 0 || req_len + body_len >= req_cap) {
        free(req);
        safe_copy(err, err_len, "IPP request is too large");
        return false;
    }
    memcpy(req + req_len, body, (size_t)body_len);
    req_len += body_len;

    char* raw = (char*)malloc(HTTP_RESPONSE_MAX);
    if (!raw) {
        free(req);
        safe_copy(err, err_len, "out of memory");
        return false;
    }

    int raw_len = -1;
    if (uri->use_tls) {
        tls::TrustAnchors tas = tls::load_trust_anchors();
        raw_len = tls::https_fetch(uri->host, uri->ip, uri->port, req, req_len,
                                   tas, raw, HTTP_RESPONSE_MAX - 1);
        if (tas.anchors) free(tas.anchors);
    } else {
        int fd = montauk::socket(Montauk::SOCK_TCP);
        if (fd < 0) {
            free(raw);
            free(req);
            snprintf(err, (size_t)err_len, "failed to create socket for %s:%u", uri->host, (unsigned)uri->port);
            return false;
        }
        if (montauk::connect(fd, uri->ip, uri->port) < 0) {
            montauk::closesocket(fd);
            free(raw);
            free(req);
            snprintf(err, (size_t)err_len, "failed to connect to %s (%s):%u",
                     uri->host, ip_text, (unsigned)uri->port);
            return false;
        }
        if (send_all_plain(fd, (const uint8_t*)req, req_len) < 0) {
            montauk::closesocket(fd);
            free(raw);
            free(req);
            snprintf(err, (size_t)err_len, "failed to send print request to %s (%s):%u",
                     uri->host, ip_text, (unsigned)uri->port);
            return false;
        }
        raw_len = recv_http_plain(fd, raw, HTTP_RESPONSE_MAX);
        montauk::closesocket(fd);
    }

    free(req);

    if (raw_len <= 0) {
        free(raw);
        snprintf(err, (size_t)err_len, "printer returned no response from %s (%s):%u",
                 uri->host, ip_text, (unsigned)uri->port);
        return false;
    }

    raw[raw_len] = '\0';
    http::Response resp = {};
    if (http::parse_response(raw, raw_len, &resp) < 0) {
        free(raw);
        snprintf(err, (size_t)err_len, "printer returned no final HTTP response from %s (%s):%u",
                 uri->host, ip_text, (unsigned)uri->port);
        return false;
    }
    if (out_http_status) *out_http_status = resp.status;
    if (resp.status < 200 || resp.status >= 300) {
        free(raw);
        snprintf(err, (size_t)err_len, "HTTP %d from %s (%s):%u%s",
                 resp.status, uri->host, ip_text, (unsigned)uri->port, uri->path);
        return false;
    }

    bool ok = extract_http_body(&resp, out_body, out_body_len);
    free(raw);
    if (!ok) {
        snprintf(err, (size_t)err_len, "failed to parse printer response from %s (%s):%u",
                 uri->host, ip_text, (unsigned)uri->port);
        return false;
    }
    return true;
}

inline void summarize_ipp_capabilities(const IppCapabilities* caps, char* out, int out_len) {
    if (out == nullptr || out_len <= 0) return;
    out[0] = '\0';
    if (caps == nullptr) return;
    char ip_text[20];
    if (caps->resolved_ip) format_ipv4(ip_text, sizeof(ip_text), caps->resolved_ip);
    else safe_copy(ip_text, sizeof(ip_text), "unresolved");
    snprintf(out, (size_t)out_len,
             "fmts=%s%s%s host=%s ip=%s port=%u path=%s %s http=%d ipp=0x%04x req=%d pdf=%s jpeg=%s text=%s",
             caps->supported_formats[0] ? caps->supported_formats : "?",
             caps->preferred_format[0] ? " pref=" : "",
             caps->preferred_format[0] ? caps->preferred_format : "",
             caps->host[0] ? caps->host : "?",
             ip_text,
             (unsigned)caps->port,
             caps->path[0] ? caps->path : "/",
             caps->use_tls ? "tls" : "plain",
             caps->http_status,
             (unsigned)caps->ipp_status,
             caps->request_id,
             caps->supports_pdf ? "yes" : "no",
             caps->supports_jpeg ? "yes" : "no",
             caps->supports_text ? "yes" : "no");
}

inline void summarize_ipp_print_result(const IppPrintResult* result, char* out, int out_len) {
    if (out == nullptr || out_len <= 0) return;
    out[0] = '\0';
    if (result == nullptr) return;
    char ip_text[20];
    if (result->resolved_ip) format_ipv4(ip_text, sizeof(ip_text), result->resolved_ip);
    else safe_copy(ip_text, sizeof(ip_text), "unresolved");
    snprintf(out, (size_t)out_len,
             "host=%s ip=%s port=%u path=%s %s format=%s http=%d ipp=0x%04x req=%d job=%d",
             result->host[0] ? result->host : "?",
             ip_text,
             (unsigned)result->port,
             result->path[0] ? result->path : "/",
             result->use_tls ? "tls" : "plain",
             result->document_format[0] ? result->document_format : "?",
             result->http_status,
             (unsigned)result->ipp_status,
             result->request_id,
             result->job_id);
}

inline void ipp_copy_value(char* out, int out_len, const uint8_t* data, int len) {
    if (out == nullptr || out_len <= 0) return;
    int n = len;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, data, (size_t)n);
    out[n] = '\0';
}

inline void ipp_append_list_value(char* out, int out_len, const char* value) {
    if (out == nullptr || out_len <= 0 || value == nullptr || value[0] == '\0') return;
    if (out[0] != '\0') {
        size_t len = strlen(out);
        if ((int)len < out_len - 1) {
            out[len++] = ',';
            out[len] = '\0';
        }
    }
    safe_copy(out + strlen(out), out_len - (int)strlen(out), value);
}

inline bool ipp_parse_common(const uint8_t* body, int body_len,
                             uint16_t* out_status, int* out_request_id,
                             IppCapabilities* caps, IppPrintResult* result,
                             char* err, int err_len) {
    if (body == nullptr || body_len < 8) {
        safe_copy(err, err_len, "IPP response is malformed");
        return false;
    }

    if (out_status) *out_status = be16(body + 2);
    if (out_request_id) *out_request_id = (int)be32(body + 4);

    char current_name[128] = {};
    int pos = 8;
    while (pos < body_len) {
        uint8_t tag = body[pos++];
        if (tag == 0x03) break;
        if (tag <= 0x0F) {
            current_name[0] = '\0';
            continue;
        }
        if (pos + 4 > body_len) break;
        uint16_t name_len = be16(body + pos);
        pos += 2;
        if (pos + name_len + 2 > body_len) break;
        if (name_len > 0) {
            int copy_len = (int)name_len;
            if (copy_len >= (int)sizeof(current_name)) copy_len = (int)sizeof(current_name) - 1;
            memcpy(current_name, body + pos, (size_t)copy_len);
            current_name[copy_len] = '\0';
        }
        pos += name_len;
        uint16_t value_len = be16(body + pos);
        pos += 2;
        if (pos + value_len > body_len) break;
        const uint8_t* value = body + pos;
        pos += value_len;

        if (caps) {
            if (strcmp(current_name, "printer-name") == 0) {
                ipp_copy_value(caps->printer_name, sizeof(caps->printer_name), value, value_len);
            } else if (strcmp(current_name, "status-message") == 0) {
                ipp_copy_value(caps->status_message, sizeof(caps->status_message), value, value_len);
            } else if (strcmp(current_name, "document-format-supported") == 0) {
                char fmt[96];
                ipp_copy_value(fmt, sizeof(fmt), value, value_len);
                ipp_append_list_value(caps->supported_formats, sizeof(caps->supported_formats), fmt);
                if (str_ieq(fmt, "application/pdf")) caps->supports_pdf = true;
                if (str_ieq(fmt, "image/jpeg")) caps->supports_jpeg = true;
                if (starts_with_ci(fmt, "text/plain")) caps->supports_text = true;
            } else if (strcmp(current_name, "document-format-default") == 0) {
                ipp_copy_value(caps->default_format, sizeof(caps->default_format), value, value_len);
            } else if (strcmp(current_name, "document-format-preferred") == 0) {
                ipp_copy_value(caps->preferred_format, sizeof(caps->preferred_format), value, value_len);
            }
        }

        if (result) {
            if (strcmp(current_name, "job-id") == 0 && value_len == 4) {
                result->job_id = (int)be32(value);
            } else if (strcmp(current_name, "job-uri") == 0) {
                ipp_copy_value(result->job_uri, sizeof(result->job_uri), value, value_len);
            } else if (strcmp(current_name, "status-message") == 0) {
                ipp_copy_value(result->status_message, sizeof(result->status_message), value, value_len);
            }
        }
    }
    return true;
}

inline bool ipp_get_printer_capabilities(const char* printer_uri,
                                         IppCapabilities* out,
                                         char* err, int err_len) {
    if (out) memset(out, 0, sizeof(*out));
    IppUri uri = {};
    if (!parse_ipp_uri(printer_uri, &uri, err, err_len)) return false;
    if (out) {
        out->use_tls = uri.use_tls;
        out->port = uri.port;
        out->resolved_ip = uri.ip;
        safe_copy(out->host, sizeof(out->host), uri.host);
        safe_copy(out->path, sizeof(out->path), uri.path);
    }

    ByteBuilder req = {};
    if (!bb_init(&req, 512)) {
        safe_copy(err, err_len, "out of memory");
        return false;
    }

    uint32_t request_id = next_request_id();
    if (out) out->request_id = (int)request_id;
    bool ok = bb_put8(&req, 0x01) && bb_put8(&req, 0x01)
        && bb_put16(&req, 0x000B)
        && bb_put32(&req, request_id)
        && bb_put8(&req, 0x01)
        && bb_put_attr_str(&req, 0x47, "attributes-charset", "utf-8")
        && bb_put_attr_str(&req, 0x48, "attributes-natural-language", "en")
        && bb_put_attr_str(&req, 0x45, "printer-uri", uri.normalized)
        && bb_put_attr_str(&req, 0x44, "requested-attributes", "printer-name")
        && bb_put8(&req, 0x44)
        && bb_put16(&req, 0)
        && bb_put16(&req, (uint16_t)strlen("printer-state"))
        && bb_put_bytes(&req, "printer-state", (int)strlen("printer-state"))
        && bb_put8(&req, 0x44)
        && bb_put16(&req, 0)
        && bb_put16(&req, (uint16_t)strlen("document-format-supported"))
        && bb_put_bytes(&req, "document-format-supported", (int)strlen("document-format-supported"))
        && bb_put8(&req, 0x44)
        && bb_put16(&req, 0)
        && bb_put16(&req, (uint16_t)strlen("document-format-default"))
        && bb_put_bytes(&req, "document-format-default", (int)strlen("document-format-default"))
        && bb_put8(&req, 0x44)
        && bb_put16(&req, 0)
        && bb_put16(&req, (uint16_t)strlen("document-format-preferred"))
        && bb_put_bytes(&req, "document-format-preferred", (int)strlen("document-format-preferred"))
        && bb_put8(&req, 0x03);
    if (!ok) {
        bb_free(&req);
        safe_copy(err, err_len, "failed to build IPP capability request");
        return false;
    }

    uint8_t* body = nullptr;
    int body_len = 0;
    int http_status = -1;
    ok = ipp_http_post(&uri, req.data, req.len, &body, &body_len, &http_status, err, err_len);
    bb_free(&req);
    if (!ok) {
        if (out) out->http_status = http_status;
        return false;
    }

    uint16_t ipp_status = 0;
    int response_id = 0;
    if (!ipp_parse_common(body, body_len, &ipp_status, &response_id, out, nullptr, err, err_len)) {
        free(body);
        return false;
    }
    free(body);

    if (out) {
        out->ok = (ipp_status == 0x0000);
        out->http_status = http_status;
        out->ipp_status = ipp_status;
        out->request_id = response_id;
    }
    if (ipp_status != 0x0000) {
        safe_copy(err, err_len, out ? out->status_message : "IPP capability query failed");
        return false;
    }
    return true;
}

inline bool ipp_print_buffer(const char* printer_uri,
                             const char* job_name,
                             const char* user_name,
                             const char* doc_format,
                             const uint8_t* data, int data_len,
                             IppPrintResult* out,
                             char* err, int err_len) {
    if (out) memset(out, 0, sizeof(*out));
    IppUri uri = {};
    if (!parse_ipp_uri(printer_uri, &uri, err, err_len)) return false;
    if (out) {
        out->use_tls = uri.use_tls;
        out->port = uri.port;
        out->resolved_ip = uri.ip;
        safe_copy(out->host, sizeof(out->host), uri.host);
        safe_copy(out->path, sizeof(out->path), uri.path);
    }
    if (data == nullptr || data_len < 0) {
        safe_copy(err, err_len, "missing print document");
        return false;
    }

    ByteBuilder req = {};
    if (!bb_init(&req, 1024 + data_len)) {
        safe_copy(err, err_len, "out of memory");
        return false;
    }

    uint32_t request_id = next_request_id();
    const char* use_job_name = (job_name && *job_name) ? job_name : "MontaukOS Print Job";
    const char* use_user = (user_name && *user_name) ? user_name : "montauk";
    const char* use_format = (doc_format && *doc_format) ? doc_format : "application/octet-stream";
    if (out) {
        out->request_id = (int)request_id;
        safe_copy(out->document_format, sizeof(out->document_format), use_format);
    }

    bool ok = bb_put8(&req, 0x01) && bb_put8(&req, 0x01)
        && bb_put16(&req, 0x0002)
        && bb_put32(&req, request_id)
        && bb_put8(&req, 0x01)
        && bb_put_attr_str(&req, 0x47, "attributes-charset", "utf-8")
        && bb_put_attr_str(&req, 0x48, "attributes-natural-language", "en")
        && bb_put_attr_str(&req, 0x45, "printer-uri", uri.normalized)
        && bb_put_attr_str(&req, 0x42, "requesting-user-name", use_user)
        && bb_put_attr_str(&req, 0x42, "job-name", use_job_name)
        && bb_put_attr_str(&req, 0x49, "document-format", use_format)
        && bb_put8(&req, 0x03)
        && bb_put_bytes(&req, data, data_len);
    if (!ok) {
        bb_free(&req);
        safe_copy(err, err_len, "failed to build IPP print request");
        return false;
    }

    uint8_t* body = nullptr;
    int body_len = 0;
    int http_status = -1;
    ok = ipp_http_post(&uri, req.data, req.len, &body, &body_len, &http_status, err, err_len);
    bb_free(&req);
    if (!ok) {
        if (out) out->http_status = http_status;
        return false;
    }

    uint16_t ipp_status = 0;
    int response_id = 0;
    if (!ipp_parse_common(body, body_len, &ipp_status, &response_id, nullptr, out, err, err_len)) {
        free(body);
        return false;
    }
    free(body);

    if (out) {
        out->http_status = http_status;
        out->ipp_status = ipp_status;
        out->request_id = response_id;
        out->ok = (ipp_status == 0x0000 || ipp_status == 0x0001);
        if (out->status_message[0] == '\0') {
            snprintf(out->status_message, sizeof(out->status_message),
                     "IPP status 0x%04x", (unsigned)ipp_status);
        }
    }

    if (ipp_status != 0x0000 && ipp_status != 0x0001) {
        if (out && out->status_message[0]) safe_copy(err, err_len, out->status_message);
        else safe_copy(err, err_len, "print job failed");
        return false;
    }
    return true;
}

inline bool submit_document_job(const char* source_path,
                                const char* printer_uri,
                                const char* job_name_override,
                                char* out_job_id, int out_job_id_len,
                                char* err, int err_len) {
    ensure_spool_dirs();

    char resolved_uri[MAX_PATH_LEN];
    if (printer_uri && *printer_uri) safe_copy(resolved_uri, sizeof(resolved_uri), printer_uri);
    else if (!read_default_printer_uri(resolved_uri, sizeof(resolved_uri))) {
        safe_copy(err, err_len, "no default printer is configured");
        return false;
    }

    IppUri uri = {};
    if (!parse_ipp_uri(resolved_uri, &uri, err, err_len)) return false;

    char job_id[48];
    generate_job_id(job_id, sizeof(job_id));

    char doc_path[MAX_PATH_LEN];
    make_doc_file_path(job_id, doc_path, sizeof(doc_path));

    int size_bytes = 0;
    if (!copy_file_atomic(source_path, doc_path, &size_bytes, err, err_len)) return false;

    JobMeta job = {};
    safe_copy(job.id, sizeof(job.id), job_id);
    safe_copy(job.state, sizeof(job.state), "queued");
    safe_copy(job.source_kind, sizeof(job.source_kind), "document");
    safe_copy(job.job_name, sizeof(job.job_name),
              (job_name_override && *job_name_override) ? job_name_override : basename_ptr(source_path));
    safe_copy(job.user_name, sizeof(job.user_name), "montauk");
    safe_copy(job.printer_uri, sizeof(job.printer_uri), uri.normalized);
    safe_copy(job.doc_path, sizeof(job.doc_path), doc_path);
    infer_doc_format(source_path, job.doc_format, sizeof(job.doc_format));
    safe_copy(job.source_name, sizeof(job.source_name), basename_ptr(source_path));
    now_string(job.created_at, sizeof(job.created_at));
    safe_copy(job.updated_at, sizeof(job.updated_at), job.created_at);
    safe_copy(job.status_message, sizeof(job.status_message), "Queued");
    snprintf(job.debug_info, sizeof(job.debug_info),
             "printer=%s format=%s source=%s",
             uri.normalized, job.doc_format, job.source_name);
    job.remote_job_id = 0;
    job.size_bytes = size_bytes;

    char job_path[MAX_PATH_LEN];
    make_job_file_path(SPOOL_QUEUE_DIR, job.id, job_path, sizeof(job_path));
    if (!save_job_to_path_atomic(job_path, &job)) {
        remove(doc_path);
        safe_copy(err, err_len, "failed to create queued job");
        return false;
    }

    if (!ensure_daemon_running(err, err_len)) return false;

    safe_copy(out_job_id, out_job_id_len, job.id);
    return true;
}

inline bool submit_test_page_job(const char* printer_uri,
                                 char* out_job_id, int out_job_id_len,
                                 char* err, int err_len) {
    ensure_spool_dirs();

    char resolved_uri[MAX_PATH_LEN];
    if (printer_uri && *printer_uri) safe_copy(resolved_uri, sizeof(resolved_uri), printer_uri);
    else if (!read_default_printer_uri(resolved_uri, sizeof(resolved_uri))) {
        safe_copy(err, err_len, "no default printer is configured");
        return false;
    }

    IppUri uri = {};
    if (!parse_ipp_uri(resolved_uri, &uri, err, err_len)) return false;

    JobMeta job = {};
    generate_job_id(job.id, sizeof(job.id));
    safe_copy(job.state, sizeof(job.state), "queued");
    safe_copy(job.source_kind, sizeof(job.source_kind), "test-page");
    safe_copy(job.job_name, sizeof(job.job_name), "MontaukOS Test Page");
    safe_copy(job.user_name, sizeof(job.user_name), "montauk");
    safe_copy(job.printer_uri, sizeof(job.printer_uri), uri.normalized);
    safe_copy(job.doc_path, sizeof(job.doc_path), "");
    safe_copy(job.doc_format, sizeof(job.doc_format), "auto");
    safe_copy(job.source_name, sizeof(job.source_name), "test-page");
    now_string(job.created_at, sizeof(job.created_at));
    safe_copy(job.updated_at, sizeof(job.updated_at), job.created_at);
    safe_copy(job.status_message, sizeof(job.status_message), "Queued");
    snprintf(job.debug_info, sizeof(job.debug_info),
             "printer=%s format=auto source=%s",
             uri.normalized, job.source_name);

    char job_path[MAX_PATH_LEN];
    make_job_file_path(SPOOL_QUEUE_DIR, job.id, job_path, sizeof(job_path));
    if (!save_job_to_path_atomic(job_path, &job)) {
        safe_copy(err, err_len, "failed to create test page job");
        return false;
    }

    if (!ensure_daemon_running(err, err_len)) return false;

    safe_copy(out_job_id, out_job_id_len, job.id);
    return true;
}

inline bool wait_for_job(const char* job_id, JobMeta* out_job, uint64_t timeout_ms,
                         char* err, int err_len) {
    uint64_t start = montauk::get_milliseconds();
    char path[MAX_PATH_LEN];
    char state[16];
    while (true) {
        if (find_job(job_id, path, sizeof(path), state, sizeof(state))) {
            JobMeta job = {};
            if (load_job_from_path(path, &job)) {
                if (strcmp(state, "completed") == 0 || strcmp(state, "failed") == 0) {
                    if (out_job) *out_job = job;
                    return strcmp(state, "completed") == 0;
                }
            }
        }
        if (montauk::get_milliseconds() - start >= timeout_ms) {
            safe_copy(err, err_len, "timed out waiting for print job");
            return false;
        }
        montauk::sleep_ms(JOB_WAIT_SLICE_MS);
    }
}

} // namespace print
