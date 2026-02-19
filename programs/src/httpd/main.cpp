/*
    * main.cpp
    * HTTP/1.0 server for ZenithOS
    * Usage: run httpd.elf [port]  (default: 80)
    * Serves a built-in index page and files from the VFS
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::slen;
using zenith::streq;
using zenith::starts_with;
using zenith::skip_spaces;

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

// ---- Content type detection ----

static bool ends_with(const char* str, const char* suffix) {
    int sl = slen(str);
    int xl = slen(suffix);
    if (xl > sl) return false;
    for (int i = 0; i < xl; i++) {
        char a = str[sl - xl + i];
        char b = suffix[i];
        // Case-insensitive for file extensions
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static const char* content_type_for(const char* path) {
    if (ends_with(path, ".html") || ends_with(path, ".htm"))
        return "text/html";
    if (ends_with(path, ".txt"))
        return "text/plain";
    if (ends_with(path, ".css"))
        return "text/css";
    if (ends_with(path, ".js"))
        return "application/javascript";
    return "application/octet-stream";
}

// ---- HTTP response helpers ----

// Send a complete HTTP response with headers and body
static void send_response(int clientFd, int statusCode, const char* statusText,
                          const char* contentType, const char* body, int bodyLen) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Server: ZenithOS/1.0\r\n"
        "\r\n",
        statusCode, statusText, contentType, bodyLen);

    zenith::send(clientFd, header, hlen);
    if (bodyLen > 0) {
        zenith::send(clientFd, body, bodyLen);
    }
}

// Send a file from the VFS
static int send_file_response(int clientFd, const char* vfsPath, const char* urlPath) {
    int handle = zenith::open(vfsPath);
    if (handle < 0) return -1;

    uint64_t size = zenith::getsize(handle);
    const char* ctype = content_type_for(urlPath);

    // Send header
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Server: ZenithOS/1.0\r\n"
        "\r\n",
        ctype, (unsigned)size);
    zenith::send(clientFd, header, hlen);

    // Send file body in chunks
    uint8_t buf[512];
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t chunk = size - offset;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        int bytesRead = zenith::read(handle, buf, offset, chunk);
        if (bytesRead <= 0) break;
        zenith::send(clientFd, buf, bytesRead);
        offset += bytesRead;
    }

    zenith::close(handle);
    return (int)size;
}

// ---- Request parsing ----

// Extract the request path from "GET /path HTTP/1.x\r\n..."
// Returns length of path written, or -1 on parse error
static int parse_request_path(const char* req, int reqLen, char* pathOut, int pathMax) {
    // Find "GET "
    if (reqLen < 4) return -1;
    if (req[0] != 'G' || req[1] != 'E' || req[2] != 'T' || req[3] != ' ')
        return -1;

    int i = 4;
    int j = 0;
    while (i < reqLen && req[i] != ' ' && req[i] != '\r' && req[i] != '\n') {
        if (j < pathMax - 1) pathOut[j++] = req[i];
        i++;
    }
    pathOut[j] = '\0';
    return j;
}

// ---- Logging ----

static void log_request(const char* method, const char* path, int status, int bodyLen) {
    // Get timestamp
    Zenith::DateTime dt;
    zenith::gettime(&dt);

    char msg[256];
    snprintf(msg, sizeof(msg), "[%02d:%02d:%02d] %s %s -> %d (%d bytes)\n",
        (int)dt.Hour, (int)dt.Minute, (int)dt.Second,
        method, path, status, bodyLen);
    zenith::print(msg);
}

// ---- Page generators ----

static int generate_index_page(char* buf, int bufSize) {
    Zenith::SysInfo info;
    zenith::get_info(&info);

    uint64_t ms = zenith::get_milliseconds();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    uint64_t hours = mins / 60;
    secs %= 60;
    mins %= 60;

    return snprintf(buf, bufSize,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>ZenithOS Web Server</title></head>\n"
        "<body>\n"
        "<h1>ZenithOS Web Server</h1>\n"
        "<p>Welcome! This page is being served by <b>httpd</b> running on ZenithOS.</p>\n"
        "<h2>System Information</h2>\n"
        "<table>\n"
        "<tr><td><b>OS:</b></td><td>%s</td></tr>\n"
        "<tr><td><b>Version:</b></td><td>%s</td></tr>\n"
        "<tr><td><b>Uptime:</b></td><td>%uh %um %us</td></tr>\n"
        "</table>\n"
        "<h2>Browse Files</h2>\n"
        "<p><a href=\"/files/\">Browse VFS files</a></p>\n"
        "</body>\n"
        "</html>\n",
        info.osName, info.osVersion,
        (unsigned)hours, (unsigned)mins, (unsigned)secs);
}

static int generate_404_page(char* buf, int bufSize, const char* path) {
    return snprintf(buf, bufSize,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>404 Not Found</title></head>\n"
        "<body>\n"
        "<h1>404 Not Found</h1>\n"
        "<p>The requested path <code>%s</code> was not found on this server.</p>\n"
        "<p><a href=\"/\">Back to home</a></p>\n"
        "</body>\n"
        "</html>\n",
        path);
}

static int generate_dir_listing(char* buf, int bufSize, const char* urlPath, const char* vfsDir) {
    int pos = 0;

    pos += snprintf(buf + pos, bufSize - pos,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>Index of %s</title></head>\n"
        "<body>\n"
        "<h1>Index of %s</h1>\n"
        "<hr>\n"
        "<ul>\n",
        urlPath, urlPath);

    // Add parent directory link if not at /files/
    if (!streq(urlPath, "/files/")) {
        pos += snprintf(buf + pos, bufSize - pos,
            "<li><a href=\"..\">..</a></li>\n");
    }

    // List directory entries
    const char* entries[64];
    int count = zenith::readdir(vfsDir, entries, 64);

    // Find the prefix to strip from entry names
    // vfsDir is like "0:/" or "0:/subdir"
    // entries come back as "subdir/file" for "0:/subdir"
    // or "file" for "0:/"
    // The prefix in entry names is the part after "0:/"
    const char* dirRel = vfsDir + 3; // skip "0:/"
    int dirRelLen = slen(dirRel);

    for (int i = 0; i < count && pos < bufSize - 128; i++) {
        // Extract just the filename portion
        const char* name = entries[i];
        // If directory is not root, entries have "dir/name" format — strip the dir prefix
        if (dirRelLen > 0 && starts_with(name, dirRel)) {
            name = name + dirRelLen;
            if (*name == '/') name++;
        }
        if (*name == '\0') continue;

        // Build the URL for this entry
        pos += snprintf(buf + pos, bufSize - pos,
            "<li><a href=\"%s%s\">%s</a></li>\n",
            urlPath, name, name);
    }

    pos += snprintf(buf + pos, bufSize - pos,
        "</ul>\n"
        "<hr>\n"
        "<p><i>ZenithOS httpd</i></p>\n"
        "</body>\n"
        "</html>\n");

    return pos;
}

// ---- Request handler ----

static void handle_client(int clientFd) {
    // Read request (HTTP requests are small, 4 KB is plenty)
    char reqBuf[4096];
    int reqLen = 0;
    int idleCount = 0;

    // Read until we get the full header (ends with \r\n\r\n)
    while (reqLen < (int)sizeof(reqBuf) - 1) {
        int r = zenith::recv(clientFd, reqBuf + reqLen, sizeof(reqBuf) - 1 - reqLen);
        if (r > 0) {
            reqLen += r;
            idleCount = 0;
            // Check if we have the full header
            bool done = false;
            for (int i = 0; i + 3 < reqLen; i++) {
                if (reqBuf[i] == '\r' && reqBuf[i+1] == '\n' &&
                    reqBuf[i+2] == '\r' && reqBuf[i+3] == '\n') {
                    done = true;
                    break;
                }
            }
            if (done) break;
        } else if (r == 0) {
            break; // Connection closed
        } else {
            idleCount++;
            if (idleCount > 500) break; // Timeout
            zenith::yield();
        }
    }
    reqBuf[reqLen] = '\0';

    if (reqLen == 0) {
        zenith::closesocket(clientFd);
        return;
    }

    // Parse request path
    char path[256];
    if (parse_request_path(reqBuf, reqLen, path, sizeof(path)) < 0) {
        // Bad request
        static char body[] = "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>";
        send_response(clientFd, 400, "Bad Request", "text/html", body, slen(body));
        log_request("???", "???", 400, slen(body));
        zenith::closesocket(clientFd);
        return;
    }

    // Route the request
    static char pageBuf[16384];

    if (streq(path, "/")) {
        // Try to serve 0:/www/index.html from disk first
        int handle = zenith::open("0:/www/index.html");
        if (handle >= 0) {
            zenith::close(handle);
            int bodyLen = send_file_response(clientFd, "0:/www/index.html", "/index.html");
            log_request("GET", path, 200, bodyLen);
        } else {
            // Fall back to built-in index page
            int bodyLen = generate_index_page(pageBuf, sizeof(pageBuf));
            send_response(clientFd, 200, "OK", "text/html", pageBuf, bodyLen);
            log_request("GET", path, 200, bodyLen);
        }

    } else if (streq(path, "/files") || streq(path, "/files/")) {
        // Root directory listing
        int bodyLen = generate_dir_listing(pageBuf, sizeof(pageBuf), "/files/", "0:/");
        send_response(clientFd, 200, "OK", "text/html", pageBuf, bodyLen);
        log_request("GET", path, 200, bodyLen);

    } else if (starts_with(path, "/files/")) {
        // Serve file or directory from VFS
        const char* relPath = path + 7; // skip "/files/"

        // Build VFS path: "0:/<relPath>"
        char vfsPath[256];
        int pi = 0;
        vfsPath[pi++] = '0'; vfsPath[pi++] = ':'; vfsPath[pi++] = '/';
        int ri = 0;
        while (relPath[ri] && pi < (int)sizeof(vfsPath) - 1) {
            vfsPath[pi++] = relPath[ri++];
        }
        // Strip trailing slash for VFS lookup
        if (pi > 3 && vfsPath[pi-1] == '/') pi--;
        vfsPath[pi] = '\0';

        // Try to open as file first
        int handle = zenith::open(vfsPath);
        if (handle >= 0) {
            // It's a file — serve it
            zenith::close(handle);
            int bodyLen = send_file_response(clientFd, vfsPath, path);
            if (bodyLen >= 0) {
                log_request("GET", path, 200, bodyLen);
            } else {
                static char body[] = "<!DOCTYPE html><html><body><h1>500 Internal Server Error</h1></body></html>";
                send_response(clientFd, 500, "Internal Server Error", "text/html", body, slen(body));
                log_request("GET", path, 500, slen(body));
            }
        } else {
            // Try as directory
            const char* entries[64];
            int count = zenith::readdir(vfsPath, entries, 64);
            if (count >= 0) {
                // Make sure urlPath ends with /
                char urlPath[256];
                int up = 0;
                int pp = 0;
                while (path[pp] && up < (int)sizeof(urlPath) - 2) urlPath[up++] = path[pp++];
                if (up > 0 && urlPath[up-1] != '/') urlPath[up++] = '/';
                urlPath[up] = '\0';

                int bodyLen = generate_dir_listing(pageBuf, sizeof(pageBuf), urlPath, vfsPath);
                send_response(clientFd, 200, "OK", "text/html", pageBuf, bodyLen);
                log_request("GET", path, 200, bodyLen);
            } else {
                // Not found
                int bodyLen = generate_404_page(pageBuf, sizeof(pageBuf), path);
                send_response(clientFd, 404, "Not Found", "text/html", pageBuf, bodyLen);
                log_request("GET", path, 404, bodyLen);
            }
        }

    } else {
        // 404 for anything else
        int bodyLen = generate_404_page(pageBuf, sizeof(pageBuf), path);
        send_response(clientFd, 404, "Not Found", "text/html", pageBuf, bodyLen);
        log_request("GET", path, 404, bodyLen);
    }

    zenith::closesocket(clientFd);
}

// ---- Entry point ----

extern "C" void _start() {
    // Parse arguments: [port]
    char argbuf[64];
    zenith::getargs(argbuf, sizeof(argbuf));
    const char* arg = skip_spaces(argbuf);

    uint16_t port = 80;
    if (*arg) {
        if (!parse_uint16(arg, &port)) {
            zenith::print("Invalid port: ");
            zenith::print(arg);
            zenith::putchar('\n');
            zenith::exit(1);
        }
    }

    // Create server socket
    int listenFd = zenith::socket(Zenith::SOCK_TCP);
    if (listenFd < 0) {
        zenith::print("Error: failed to create socket\n");
        zenith::exit(1);
    }

    // Bind
    if (zenith::bind(listenFd, port) < 0) {
        zenith::print("Error: failed to bind to port ");
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d", (int)port);
        zenith::print(tmp);
        zenith::putchar('\n');
        zenith::closesocket(listenFd);
        zenith::exit(1);
    }

    // Listen
    if (zenith::listen(listenFd) < 0) {
        zenith::print("Error: failed to listen\n");
        zenith::closesocket(listenFd);
        zenith::exit(1);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "ZenithOS httpd listening on port %d\n", (int)port);
    zenith::print(msg);
    zenith::print("Press Ctrl+Q between requests to stop.\n\n");

    bool running = true;
    while (running) {
        // Check for Ctrl+Q before blocking on accept
        while (zenith::is_key_available()) {
            Zenith::KeyEvent ev;
            zenith::getkey(&ev);
            if (ev.pressed && ev.ctrl && ev.ascii == 'q') {
                running = false;
                break;
            }
        }
        if (!running) break;

        // Accept next client (blocks until a connection arrives)
        int clientFd = zenith::accept(listenFd);
        if (clientFd < 0) {
            zenith::print("Warning: accept failed\n");
            zenith::yield();
            continue;
        }

        handle_client(clientFd);

        // After serving, check for Ctrl+Q
        while (zenith::is_key_available()) {
            Zenith::KeyEvent ev;
            zenith::getkey(&ev);
            if (ev.pressed && ev.ctrl && ev.ascii == 'q') {
                running = false;
                break;
            }
        }
    }

    zenith::print("\nShutting down httpd...\n");
    zenith::closesocket(listenFd);
    zenith::exit(0);
}
