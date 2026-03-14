/*
 * http.hpp
 * Simple HTTP request builder and response parser for MontaukOS
 * Wraps tls::https_fetch() and raw sockets for ergonomic HTTP usage.
 */

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <tls/tls.hpp>

namespace http {

// ----------------------------------------------------------------------------
// Response
// ----------------------------------------------------------------------------

struct Response {
    int status;           // HTTP status code (200, 404, etc.) or -1 on error
    const char* headers;  // Pointer into raw buffer (header block)
    int headers_len;
    const char* body;     // Pointer into raw buffer (body)
    int body_len;
    char* raw;            // Owned buffer — caller must free with montauk::mfree()
    int raw_len;
};

// Parse raw HTTP response in-place. Sets pointers into buf (does not copy).
// Returns status code, or -1 if unparseable.
inline int parse_response(char* buf, int len, Response* out) {
    out->raw = buf;
    out->raw_len = len;
    out->status = -1;
    out->headers = nullptr;
    out->headers_len = 0;
    out->body = nullptr;
    out->body_len = 0;

    if (len < 12) return -1;  // "HTTP/1.x NNN"

    // Parse status code from "HTTP/1.x NNN"
    const char* p = buf;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return -1;
    p++;
    int code = 0;
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; i++, p++)
        code = code * 10 + (*p - '0');
    out->status = code;

    // Headers start after the status line
    const char* hdr_start = buf;
    while (hdr_start < buf + len - 1) {
        if (*hdr_start == '\r' && *(hdr_start + 1) == '\n') { hdr_start += 2; break; }
        if (*hdr_start == '\n') { hdr_start++; break; }
        hdr_start++;
    }
    out->headers = hdr_start;

    // Find \r\n\r\n boundary between headers and body
    for (const char* s = hdr_start; s < buf + len - 3; s++) {
        if (s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
            out->headers_len = (int)(s - hdr_start);
            out->body = s + 4;
            out->body_len = len - (int)(out->body - buf);
            return code;
        }
    }
    // No body separator found — entire remainder is headers
    out->headers_len = len - (int)(hdr_start - buf);
    return code;
}

// Find a header value by name (case-insensitive match on the name).
// Writes value into out_val (up to max_len), returns true if found.
inline bool get_header(const Response* resp, const char* name, char* out_val, int max_len) {
    if (!resp->headers || resp->headers_len == 0) return false;
    int name_len = montauk::slen(name);
    const char* p = resp->headers;
    const char* end = resp->headers + resp->headers_len;

    while (p < end) {
        // Case-insensitive prefix match
        bool match = true;
        if (p + name_len >= end) { match = false; }
        else {
            for (int i = 0; i < name_len; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match && p[name_len] != ':') match = false;
        }

        if (match) {
            const char* v = p + name_len + 1;
            while (v < end && *v == ' ') v++;  // skip OWS
            int i = 0;
            while (v < end && *v != '\r' && *v != '\n' && i < max_len - 1)
                out_val[i++] = *v++;
            out_val[i] = 0;
            return true;
        }

        // Skip to next line
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return false;
}

// Free a response's raw buffer.
inline void free_response(Response* resp) {
    if (resp->raw) { montauk::mfree(resp->raw); resp->raw = nullptr; }
}

// ----------------------------------------------------------------------------
// Request builder (internal)
// ----------------------------------------------------------------------------

inline int build_request(char* buf, int buf_size,
                         const char* method, const char* host,
                         const char* path, const char* content_type,
                         const char* body_data, int body_len,
                         const char* extra_headers) {
    char* p = buf;
    char* end = buf + buf_size - 1;

    auto append = [&](const char* s) {
        while (*s && p < end) *p++ = *s++;
    };
    auto append_int = [&](int n) {
        char tmp[16]; int ti = 0;
        if (n == 0) { if (p < end) *p++ = '0'; return; }
        while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
        for (int j = ti - 1; j >= 0 && p < end; j--) *p++ = tmp[j];
    };

    // Request line
    append(method); append(" "); append(path); append(" HTTP/1.1\r\n");

    // Host
    append("Host: "); append(host); append("\r\n");

    // Content headers (for POST/PUT/PATCH)
    if (body_data && body_len > 0) {
        if (content_type) {
            append("Content-Type: "); append(content_type); append("\r\n");
        }
        append("Content-Length: "); append_int(body_len); append("\r\n");
    }

    // Extra headers (caller-supplied, must include \r\n terminators)
    if (extra_headers) append(extra_headers);

    append("Connection: close\r\n");
    append("\r\n");

    int header_len = (int)(p - buf);

    // Append body
    if (body_data && body_len > 0 && header_len + body_len < buf_size) {
        montauk::memcpy(p, body_data, body_len);
        p += body_len;
    }

    return (int)(p - buf);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

// GET request over HTTPS. Returns parsed response. Caller must free_response().
inline Response get(const char* host, const char* path,
                    const tls::TrustAnchors& tas,
                    int resp_buf_size = 32768,
                    const char* extra_headers = nullptr,
                    tls::AbortCheckFn abort_check = nullptr) {
    Response resp = {};
    resp.status = -1;

    uint32_t ip = montauk::resolve(host);
    if (!ip) return resp;

    char req[1024];
    int reqLen = build_request(req, sizeof(req), "GET", host, path,
                               nullptr, nullptr, 0, extra_headers);

    char* buf = (char*)montauk::malloc(resp_buf_size);
    if (!buf) return resp;

    int n = tls::https_fetch(host, ip, 443, req, reqLen, tas,
                             buf, resp_buf_size - 1, abort_check);
    if (n <= 0) { montauk::mfree(buf); return resp; }
    buf[n] = 0;

    parse_response(buf, n, &resp);
    return resp;
}

// POST request over HTTPS. Returns parsed response. Caller must free_response().
inline Response post(const char* host, const char* path,
                     const char* content_type,
                     const char* body_data, int body_len,
                     const tls::TrustAnchors& tas,
                     int resp_buf_size = 32768,
                     const char* extra_headers = nullptr,
                     tls::AbortCheckFn abort_check = nullptr) {
    Response resp = {};
    resp.status = -1;

    uint32_t ip = montauk::resolve(host);
    if (!ip) return resp;

    int req_size = 1024 + body_len;
    char* req = (char*)montauk::malloc(req_size);
    if (!req) return resp;

    int reqLen = build_request(req, req_size, "POST", host, path,
                               content_type, body_data, body_len,
                               extra_headers);

    char* buf = (char*)montauk::malloc(resp_buf_size);
    if (!buf) { montauk::mfree(req); return resp; }

    int n = tls::https_fetch(host, ip, 443, req, reqLen, tas,
                             buf, resp_buf_size - 1, abort_check);
    montauk::mfree(req);

    if (n <= 0) { montauk::mfree(buf); return resp; }
    buf[n] = 0;

    parse_response(buf, n, &resp);
    return resp;
}

// Generic request over HTTPS (PUT, PATCH, DELETE, etc.).
inline Response request(const char* method,
                        const char* host, const char* path,
                        const char* content_type,
                        const char* body_data, int body_len,
                        const tls::TrustAnchors& tas,
                        int resp_buf_size = 32768,
                        const char* extra_headers = nullptr,
                        tls::AbortCheckFn abort_check = nullptr) {
    Response resp = {};
    resp.status = -1;

    uint32_t ip = montauk::resolve(host);
    if (!ip) return resp;

    int req_size = 1024 + (body_len > 0 ? body_len : 0);
    char* req = (char*)montauk::malloc(req_size);
    if (!req) return resp;

    int reqLen = build_request(req, req_size, method, host, path,
                               content_type, body_data, body_len,
                               extra_headers);

    char* buf = (char*)montauk::malloc(resp_buf_size);
    if (!buf) { montauk::mfree(req); return resp; }

    int n = tls::https_fetch(host, ip, 443, req, reqLen, tas,
                             buf, resp_buf_size - 1, abort_check);
    montauk::mfree(req);

    if (n <= 0) { montauk::mfree(buf); return resp; }
    buf[n] = 0;

    parse_response(buf, n, &resp);
    return resp;
}

// Plain HTTP (no TLS) GET over port 80.
inline Response get_plain(const char* host, const char* path,
                          int resp_buf_size = 32768,
                          const char* extra_headers = nullptr) {
    Response resp = {};
    resp.status = -1;

    uint32_t ip = montauk::resolve(host);
    if (!ip) return resp;

    char req[1024];
    int reqLen = build_request(req, sizeof(req), "GET", host, path,
                               nullptr, nullptr, 0, extra_headers);

    int sock = montauk::socket(Montauk::SOCK_TCP);
    if (sock < 0) return resp;
    if (montauk::connect(sock, ip, 80) < 0) { montauk::closesocket(sock); return resp; }

    montauk::send(sock, req, reqLen);

    char* buf = (char*)montauk::malloc(resp_buf_size);
    if (!buf) { montauk::closesocket(sock); return resp; }

    int total = 0;
    while (total < resp_buf_size - 1) {
        int n = montauk::recv(sock, buf + total, resp_buf_size - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    montauk::closesocket(sock);

    if (total <= 0) { montauk::mfree(buf); return resp; }
    buf[total] = 0;

    parse_response(buf, total, &resp);
    return resp;
}

} // namespace http
