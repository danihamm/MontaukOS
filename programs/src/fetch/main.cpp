/*
    * main.cpp
    * HTTP/HTTPS client for ZenithOS (TLS 1.2 via BearSSL)
    * Usage: fetch [-v] <url>
    *        fetch [-v] <host> <port> [path]    (legacy mode, plain HTTP)
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>
#include <tls/tls.hpp>

extern "C" {
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
}

using zenith::skip_spaces;

// ---- IP/port parsing ----

static bool parse_ip(const char* s, uint32_t* out) {
    uint32_t octets[4];
    int idx = 0;
    uint32_t val = 0;
    bool hasDigit = false;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 255) return false;
            hasDigit = true;
        } else if (c == '.' || c == '\0') {
            if (!hasDigit || idx >= 4) return false;
            octets[idx++] = val;
            val = 0; hasDigit = false;
            if (c == '\0') break;
        } else return false;
    }
    if (idx != 4) return false;
    *out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

static bool parse_uint16(const char* s, uint16_t* out) {
    uint32_t val = 0;
    if (*s == '\0') return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        val = val * 10 + (*s - '0');
        if (val > 65535) return false;
        s++;
    }
    *out = (uint16_t)val;
    return true;
}

static void format_ip(char* buf, uint32_t ip) {
    snprintf(buf, 32, "%u.%u.%u.%u",
        ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

// ---- URL parser ----

struct ParsedUrl {
    char host[256];
    char path[512];
    uint16_t port;
    bool https;
    bool valid;
};

static ParsedUrl parse_url(const char* url) {
    ParsedUrl u;
    memset(&u, 0, sizeof(u));
    u.path[0] = '/'; u.path[1] = '\0';

    // Check scheme
    if (strncmp(url, "https://", 8) == 0) {
        u.https = true;
        u.port = 443;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        u.https = false;
        u.port = 80;
        url += 7;
    } else {
        u.valid = false;
        return u;
    }

    // Parse host (until '/', ':', or end)
    int i = 0;
    while (url[i] && url[i] != '/' && url[i] != ':' && i < 255) {
        u.host[i] = url[i];
        i++;
    }
    u.host[i] = '\0';
    url += i;

    // Optional port
    if (*url == ':') {
        url++;
        uint32_t p = 0;
        while (*url >= '0' && *url <= '9') {
            p = p * 10 + (*url - '0');
            url++;
        }
        if (p > 0 && p <= 65535) u.port = (uint16_t)p;
    }

    // Path
    if (*url == '/') {
        int j = 0;
        while (url[j] && j < 511) {
            u.path[j] = url[j];
            j++;
        }
        u.path[j] = '\0';
    }

    u.valid = (u.host[0] != '\0');
    return u;
}

// ---- HTTP response parser ----

static int find_header_end(const char* buf, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    }
    return -1;
}

static int parse_status_code(const char* buf, int len) {
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    if (i >= len) return -1;
    i++;
    if (i + 2 >= len) return -1;
    if (buf[i] < '0' || buf[i] > '9') return -1;
    return (buf[i] - '0') * 100 + (buf[i+1] - '0') * 10 + (buf[i+2] - '0');
}

static void parse_status_text(const char* buf, int len, char* out, int outMax) {
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    i++;
    while (i < len && buf[i] != ' ') i++;
    i++;
    int j = 0;
    while (i < len && buf[i] != '\r' && buf[i] != '\n' && j < outMax - 1)
        out[j++] = buf[i++];
    out[j] = '\0';
}

// ---- Keyboard abort check for TLS ----

static bool check_keyboard_abort() {
    if (zenith::is_key_available()) {
        Zenith::KeyEvent ev;
        zenith::getkey(&ev);
        if (ev.pressed && ev.ctrl && ev.ascii == 'q') return true;
    }
    return false;
}

// ---- Plain HTTP exchange (no TLS) ----

static int plain_http_exchange(int fd, const char* request, int reqLen,
                               char* respBuf, int respMax) {
    // Send request
    int sent = 0;
    uint64_t deadline = zenith::get_milliseconds() + 15000;
    while (sent < reqLen) {
        int r = zenith::send(fd, request + sent, reqLen - sent);
        if (r > 0) { sent += r; deadline = zenith::get_milliseconds() + 15000; }
        else if (r < 0) return -1;
        else {
            if (zenith::get_milliseconds() >= deadline) return -1;
            zenith::sleep_ms(1);
        }
    }

    // Receive response
    int respLen = 0;
    deadline = zenith::get_milliseconds() + 15000;
    while (respLen < respMax - 1) {
        if (zenith::is_key_available()) {
            Zenith::KeyEvent ev;
            zenith::getkey(&ev);
            if (ev.pressed && ev.ctrl && ev.ascii == 'q') return -2; // aborted
        }

        int r = zenith::recv(fd, respBuf + respLen, respMax - 1 - respLen);
        if (r > 0) { respLen += r; deadline = zenith::get_milliseconds() + 15000; }
        else if (r < 0) break;
        else {
            if (zenith::get_milliseconds() >= deadline) break;
            zenith::sleep_ms(1);
        }
    }
    return respLen;
}

// ---- Print response body ----

static void print_response(const char* respBuf, int respLen, bool verbose) {
    if (respLen <= 0) {
        zenith::print("Error: empty response\n");
        return;
    }

    int headerEnd = find_header_end(respBuf, respLen);
    if (headerEnd < 0) {
        zenith::print("Warning: malformed response (no header boundary)\n\n");
        // Print raw
        char chunk[512];
        int printed = 0;
        while (printed < respLen) {
            int n = respLen - printed;
            if (n > 511) n = 511;
            memcpy(chunk, respBuf + printed, n);
            chunk[n] = '\0';
            zenith::print(chunk);
            printed += n;
        }
        zenith::putchar('\n');
        return;
    }

    int statusCode = parse_status_code(respBuf, headerEnd);
    char statusText[64];
    parse_status_text(respBuf, headerEnd, statusText, sizeof(statusText));
    int bodyLen = respLen - headerEnd;

    if (verbose) {
        char msg[256];
        snprintf(msg, sizeof(msg), "HTTP %d %s (%d bytes)\n\n", statusCode, statusText, bodyLen);
        zenith::print(msg);
    }

    if (bodyLen > 0) {
        const char* body = respBuf + headerEnd;
        char chunk[512];
        int printed = 0;
        while (printed < bodyLen) {
            int n = bodyLen - printed;
            if (n > 511) n = 511;
            memcpy(chunk, body + printed, n);
            chunk[n] = '\0';
            zenith::print(chunk);
            printed += n;
        }
        zenith::putchar('\n');
    }
}

// ---- Main ----

extern "C" void _start() {
    char argbuf[1024];
    zenith::getargs(argbuf, sizeof(argbuf));
    const char* arg = skip_spaces(argbuf);

    if (*arg == '\0') {
        zenith::print("Usage: fetch [-v] <url>\n");
        zenith::print("       fetch [-v] <host> <port> [path]\n");
        zenith::print("\n");
        zenith::print("  -v  Verbose output (show connection info and headers)\n");
        zenith::print("\n");
        zenith::print("Examples:\n");
        zenith::print("  fetch https://icanhazip.com\n");
        zenith::print("  fetch http://example.com/index.html\n");
        zenith::print("  fetch -v https://example.com\n");
        zenith::print("  fetch 10.0.68.1 80 /\n");
        zenith::exit(0);
    }

    // Check for -v flag
    bool verbose = false;
    if (arg[0] == '-' && arg[1] == 'v' && (arg[2] == ' ' || arg[2] == '\0')) {
        verbose = true;
        arg = skip_spaces(arg + 2);
    }

    // Determine mode: URL mode (starts with http:// or https://) vs legacy mode
    bool urlMode = (strncmp(arg, "http://", 7) == 0 || strncmp(arg, "https://", 8) == 0);

    char hostStr[256];
    char path[512];
    uint16_t port;
    bool useHttps = false;

    if (urlMode) {
        ParsedUrl url = parse_url(arg);
        if (!url.valid) {
            zenith::print("Error: invalid URL\n");
            zenith::exit(1);
        }
        strcpy(hostStr, url.host);
        strcpy(path, url.path);
        port = url.port;
        useHttps = url.https;
    } else {
        // Legacy mode: <host> <port> [path]
        int i = 0;
        while (arg[i] && arg[i] != ' ' && i < 255) { hostStr[i] = arg[i]; i++; }
        hostStr[i] = '\0';
        arg = skip_spaces(arg + i);

        char portStr[16];
        i = 0;
        while (arg[i] && arg[i] != ' ' && i < 15) { portStr[i] = arg[i]; i++; }
        portStr[i] = '\0';
        arg = skip_spaces(arg + i);

        if (!parse_uint16(portStr, &port)) {
            zenith::print("Invalid port: ");
            zenith::print(portStr);
            zenith::putchar('\n');
            zenith::exit(1);
        }

        if (*arg) {
            i = 0;
            while (arg[i] && i < 511) { path[i] = arg[i]; i++; }
            path[i] = '\0';
        } else {
            path[0] = '/'; path[1] = '\0';
        }
    }

    // Resolve host to IP
    uint32_t serverIp;
    if (!parse_ip(hostStr, &serverIp)) {
        serverIp = zenith::resolve(hostStr);
        if (serverIp == 0) {
            zenith::print("Error: could not resolve ");
            zenith::print(hostStr);
            zenith::putchar('\n');
            zenith::exit(1);
        }
    }

    char ipStr[32];
    format_ip(ipStr, serverIp);

    if (verbose) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Connecting to %s:%d (%s)...\n",
            hostStr, (int)port, useHttps ? "HTTPS" : "HTTP");
        zenith::print(msg);
    }

    // Build HTTP request
    char request[1024];
    int reqLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: ZenithOS/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, hostStr);

    if (verbose) {
        char msg[128];
        snprintf(msg, sizeof(msg), "GET %s\n", path);
        zenith::print(msg);
    }

    // Allocate response buffer on heap (stack is only 16 KB)
    static constexpr int RESP_MAX = 65536;
    char* respBuf = (char*)malloc(RESP_MAX);
    if (!respBuf) {
        zenith::print("Error: out of memory\n");
        zenith::exit(1);
    }

    int respLen;

    if (useHttps) {
        // ---- TLS handshake and exchange ----
        tls::TrustAnchors tas = tls::load_trust_anchors();
        if (verbose) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Loaded %u trust anchors\n", (unsigned)tas.count);
            zenith::print(msg);
        }
        if (tas.count == 0) {
            zenith::print("Error: no trust anchors loaded\n");
            free(respBuf);
            zenith::exit(1);
        }

        if (verbose) {
            uint32_t days, secs;
            tls::get_bearssl_time(&days, &secs);
            Zenith::DateTime dt;
            zenith::gettime(&dt);
            char tmsg[128];
            snprintf(tmsg, sizeof(tmsg), "System time: %u-%02u-%02u %02u:%02u:%02u (days=%u secs=%u)\n",
                (unsigned)dt.Year, (unsigned)dt.Month, (unsigned)dt.Day,
                (unsigned)dt.Hour, (unsigned)dt.Minute, (unsigned)dt.Second,
                (unsigned)days, (unsigned)secs);
            zenith::print(tmsg);
            zenith::print("TLS handshake...\n");
        }

        respLen = tls::https_fetch(hostStr, serverIp, port,
                                   request, reqLen, tas,
                                   respBuf, RESP_MAX, check_keyboard_abort);

        if (verbose && respLen > 0) {
            zenith::print("TLS connection established\n");
        }
    } else {
        // ---- Plain HTTP ----
        int fd = zenith::socket(Zenith::SOCK_TCP);
        if (fd < 0) {
            zenith::print("Error: failed to create socket\n");
            zenith::exit(1);
        }

        if (zenith::connect(fd, serverIp, port) < 0) {
            zenith::print("Error: connection failed\n");
            zenith::closesocket(fd);
            zenith::exit(1);
        }

        respLen = plain_http_exchange(fd, request, reqLen, respBuf, RESP_MAX);
        zenith::closesocket(fd);

        if (respLen == -2) {
            zenith::print("\nAborted.\n");
            zenith::exit(0);
        }
    }

    if (respLen <= 0) {
        zenith::print("Error: no response received\n");
        zenith::exit(1);
    }

    respBuf[respLen] = '\0';
    print_response(respBuf, respLen, verbose);

    zenith::exit(0);
}
