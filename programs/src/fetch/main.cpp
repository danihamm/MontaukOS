/*
    * main.cpp
    * HTTP/1.0 client for ZenithOS
    * Usage: run fetch.elf <server_ip> <port> <path>
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::skip_spaces;
using zenith::memcpy;

// ---- Minimal snprintf (no libc available) ----

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState {
    char*  buf;
    int    pos;
    int    max;
};

static void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

static void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24];
    int i = 0;
    const char* digits = "0123456789abcdef";
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    int total = (neg ? 1 : 0) + i;
    if (neg && pad == '0') pf_putc(st, '-');
    for (int w = total; w < width; w++) pf_putc(st, pad);
    if (neg && pad != '0') pf_putc(st, '-');
    while (i > 0) pf_putc(st, tmp[--i]);
}

static int vsnprintf(char* buf, int size, const char* fmt, va_list ap) {
    PfState st;
    st.buf = buf;
    st.pos = 0;
    st.max = size > 0 ? size - 1 : 0;
    while (*fmt) {
        if (*fmt != '%') { pf_putc(&st, *fmt++); continue; }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            long val = va_arg(ap, int);
            int neg = 0; unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); }
            else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg);
            break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            for (int w = slen; w < width; w++) pf_putc(&st, ' ');
            for (int j = 0; j < slen; j++) pf_putc(&st, s[j]);
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); pf_putc(&st, c); break; }
        case '%': pf_putc(&st, '%'); break;
        default: pf_putc(&st, '%'); pf_putc(&st, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (size > 0) {
        if (st.pos < size) st.buf[st.pos] = '\0';
        else st.buf[size - 1] = '\0';
    }
    return st.pos;
}

static int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

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

// ---- HTTP response parser ----

// Find "\r\n\r\n" boundary between headers and body
static int find_header_end(const char* buf, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    }
    return -1;
}

// Extract HTTP status code from first line: "HTTP/1.x NNN ..."
static int parse_status_code(const char* buf, int len) {
    // Find first space
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    if (i >= len) return -1;
    i++; // skip space
    // Parse 3-digit status
    if (i + 2 >= len) return -1;
    if (buf[i] < '0' || buf[i] > '9') return -1;
    int code = (buf[i] - '0') * 100 + (buf[i+1] - '0') * 10 + (buf[i+2] - '0');
    return code;
}

// Extract status text from first line: "HTTP/1.x NNN Status Text\r\n"
static void parse_status_text(const char* buf, int len, char* out, int outMax) {
    // Skip "HTTP/1.x NNN "
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    i++; // skip first space (after HTTP/1.x)
    while (i < len && buf[i] != ' ') i++;
    i++; // skip second space (after status code)
    // Copy until \r or end
    int j = 0;
    while (i < len && buf[i] != '\r' && buf[i] != '\n' && j < outMax - 1) {
        out[j++] = buf[i++];
    }
    out[j] = '\0';
}

// ---- Main ----

extern "C" void _start() {
    // Parse arguments: <server_ip> <port> <path>
    char argbuf[512];
    zenith::getargs(argbuf, sizeof(argbuf));
    const char* arg = skip_spaces(argbuf);

    if (*arg == '\0') {
        zenith::print("Usage: fetch.elf <server_ip> <port> <path>\n");
        zenith::print("Example: run fetch.elf 10.0.68.1 80 /\n");
        zenith::print("         run fetch.elf 93.184.216.34 80 /index.html\n");
        zenith::exit(0);
    }

    // Parse IP
    char ipStr[32];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 31) { ipStr[i] = arg[i]; i++; }
    ipStr[i] = '\0';
    arg = skip_spaces(arg + i);

    uint32_t serverIp;
    if (!parse_ip(ipStr, &serverIp)) {
        zenith::print("Invalid IP address: ");
        zenith::print(ipStr);
        zenith::putchar('\n');
        zenith::exit(1);
    }

    // Parse port
    char portStr[16];
    i = 0;
    while (arg[i] && arg[i] != ' ' && i < 15) { portStr[i] = arg[i]; i++; }
    portStr[i] = '\0';
    arg = skip_spaces(arg + i);

    uint16_t serverPort;
    if (!parse_uint16(portStr, &serverPort)) {
        zenith::print("Invalid port: ");
        zenith::print(portStr);
        zenith::putchar('\n');
        zenith::exit(1);
    }

    // Parse path (rest of args, default to "/" if empty)
    char path[256];
    if (*arg) {
        i = 0;
        while (arg[i] && i < 255) { path[i] = arg[i]; i++; }
        path[i] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    // Print connection info
    char msg[256];
    snprintf(msg, sizeof(msg), "Connecting to %s:%d...\n", ipStr, (int)serverPort);
    zenith::print(msg);

    // Create socket
    int fd = zenith::socket(Zenith::SOCK_TCP);
    if (fd < 0) {
        zenith::print("Error: failed to create socket\n");
        zenith::exit(1);
    }

    // Connect
    if (zenith::connect(fd, serverIp, serverPort) < 0) {
        zenith::print("Error: connection failed\n");
        zenith::closesocket(fd);
        zenith::exit(1);
    }

    // Build and send HTTP request
    char request[512];
    int reqLen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: ZenithOS/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, ipStr);

    snprintf(msg, sizeof(msg), "GET %s\n", path);
    zenith::print(msg);

    if (zenith::send(fd, request, reqLen) < 0) {
        zenith::print("Error: failed to send request\n");
        zenith::closesocket(fd);
        zenith::exit(1);
    }

    // Receive response
    // We accumulate the full response to parse headers, then print the body.
    // Use a large static buffer (32 KB) since we can't save to files anyway.
    static char respBuf[32768];
    int respLen = 0;
    bool aborted = false;
    int idleCount = 0;

    while (respLen < (int)sizeof(respBuf) - 1) {
        // Check for Ctrl+Q to abort
        if (zenith::is_key_available()) {
            Zenith::KeyEvent ev;
            zenith::getkey(&ev);
            if (ev.pressed && ev.ctrl && ev.ascii == 'q') {
                aborted = true;
                break;
            }
        }

        int r = zenith::recv(fd, respBuf + respLen, sizeof(respBuf) - 1 - respLen);
        if (r > 0) {
            respLen += r;
            idleCount = 0;
        } else if (r == 0) {
            // Connection closed by server — done
            break;
        } else {
            // No data available or error
            idleCount++;
            if (idleCount > 2000) {
                // Assume connection closed after extended idle
                break;
            }
            zenith::yield();
        }
    }

    respBuf[respLen] = '\0';
    zenith::closesocket(fd);

    if (aborted) {
        zenith::print("\nAborted.\n");
        zenith::exit(0);
    }

    if (respLen == 0) {
        zenith::print("Error: empty response\n");
        zenith::exit(1);
    }

    // Parse response headers
    int headerEnd = find_header_end(respBuf, respLen);
    if (headerEnd < 0) {
        // No proper header/body separator — just print everything
        zenith::print("Warning: malformed response (no header boundary)\n\n");
        zenith::print(respBuf);
        zenith::putchar('\n');
        zenith::exit(0);
    }

    int statusCode = parse_status_code(respBuf, headerEnd);
    char statusText[64];
    parse_status_text(respBuf, headerEnd, statusText, sizeof(statusText));

    int bodyLen = respLen - headerEnd;

    // Print summary
    snprintf(msg, sizeof(msg), "HTTP/1.0 %d %s (%d bytes)\n\n", statusCode, statusText, bodyLen);
    zenith::print(msg);

    // Print body — it may contain null bytes in binary content, but for text we
    // can just print as a string. For binary, we print what we can.
    if (bodyLen > 0) {
        // Print body in chunks (print expects null-terminated)
        const char* body = respBuf + headerEnd;
        int printed = 0;
        char chunk[512];
        while (printed < bodyLen) {
            int n = bodyLen - printed;
            if (n > (int)sizeof(chunk) - 1) n = (int)sizeof(chunk) - 1;
            memcpy(chunk, body + printed, n);
            chunk[n] = '\0';
            zenith::print(chunk);
            printed += n;
        }
        zenith::putchar('\n');
    }

    zenith::exit(0);
}
