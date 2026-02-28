/*
    * main.cpp
    * tcpconnect - Interactive TCP client
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>

using montauk::skip_spaces;

static void print_int(uint64_t n) {
    if (n == 0) {
        montauk::putchar('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        montauk::putchar(buf[j]);
    }
}

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
            val = 0;
            hasDigit = false;
            if (c == '\0') break;
        } else {
            return false;
        }
    }

    if (idx != 4) return false;
    *out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

static void print_ip(uint32_t ip) {
    print_int(ip & 0xFF);
    montauk::putchar('.');
    print_int((ip >> 8) & 0xFF);
    montauk::putchar('.');
    print_int((ip >> 16) & 0xFF);
    montauk::putchar('.');
    print_int((ip >> 24) & 0xFF);
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

extern "C" void _start() {
    char args[256];
    int len = montauk::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        montauk::print("Usage: tcpconnect <host> <port>\n");
        montauk::exit(1);
    }

    // Parse host (IP or hostname)
    char hostStr[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 127) {
        hostStr[i] = args[i];
        i++;
    }
    hostStr[i] = '\0';

    uint32_t ip;
    if (!parse_ip(hostStr, &ip)) {
        ip = montauk::resolve(hostStr);
        if (ip == 0) {
            montauk::print("Could not resolve: ");
            montauk::print(hostStr);
            montauk::putchar('\n');
            montauk::exit(1);
        }
    }

    // Parse port
    const char* portStr = skip_spaces(args + i);
    if (*portStr == '\0') {
        montauk::print("Usage: tcpconnect <ip> <port>\n");
        montauk::exit(1);
    }
    uint16_t port;
    if (!parse_uint16(portStr, &port)) {
        montauk::print("Invalid port: ");
        montauk::print(portStr);
        montauk::putchar('\n');
        montauk::exit(1);
    }

    // Create socket
    int fd = montauk::socket(Montauk::SOCK_TCP);
    if (fd < 0) {
        montauk::print("Error: failed to create socket\n");
        montauk::exit(1);
    }

    montauk::print("Connecting to ");
    print_ip(ip);
    montauk::putchar(':');
    print_int(port);
    montauk::print("...\n");

    if (montauk::connect(fd, ip, port) < 0) {
        montauk::print("Error: connection failed\n");
        montauk::closesocket(fd);
        montauk::exit(1);
    }

    montauk::print("Connected! Type to send, Ctrl+Q to disconnect.\n");

    // Interactive send/receive loop
    char sendBuf[256];
    int sendPos = 0;
    uint8_t recvBuf[512];

    while (true) {
        // Poll for received data (non-blocking)
        int r = montauk::recv(fd, recvBuf, sizeof(recvBuf) - 1);
        if (r < 0) {
            montauk::print("\nConnection closed by remote.\n");
            break;
        }
        if (r > 0) {
            recvBuf[r] = '\0';
            montauk::print((const char*)recvBuf);
        }

        // Poll keyboard
        if (montauk::is_key_available()) {
            Montauk::KeyEvent ev;
            montauk::getkey(&ev);

            if (!ev.pressed) continue;

            // Ctrl+Q to quit
            if (ev.ctrl && (ev.ascii == 'q' || ev.ascii == 'Q')) {
                montauk::print("\nDisconnecting...\n");
                break;
            }

            if (ev.ascii == '\n') {
                sendBuf[sendPos++] = '\n';
                montauk::putchar('\n');
                montauk::send(fd, sendBuf, sendPos);
                sendPos = 0;
            } else if (ev.ascii == '\b') {
                if (sendPos > 0) {
                    sendPos--;
                    montauk::putchar('\b');
                    montauk::putchar(' ');
                    montauk::putchar('\b');
                }
            } else if (ev.ascii >= ' ' && sendPos < 254) {
                sendBuf[sendPos++] = ev.ascii;
                montauk::putchar(ev.ascii);
            }
        } else {
            // No key and no data -- yield to avoid busy-spinning
            montauk::yield();
        }
    }

    montauk::closesocket(fd);
    montauk::exit(0);
}
