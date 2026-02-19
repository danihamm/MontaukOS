/*
    * main.cpp
    * tcpconnect - Interactive TCP client
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::skip_spaces;

static void print_int(uint64_t n) {
    if (n == 0) {
        zenith::putchar('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        zenith::putchar(buf[j]);
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
    zenith::putchar('.');
    print_int((ip >> 8) & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 16) & 0xFF);
    zenith::putchar('.');
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
    int len = zenith::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        zenith::print("Usage: tcpconnect <ip> <port>\n");
        zenith::exit(1);
    }

    // Parse IP address (up to first space)
    char ipStr[32];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) {
        ipStr[i] = args[i];
        i++;
    }
    ipStr[i] = '\0';

    uint32_t ip;
    if (!parse_ip(ipStr, &ip)) {
        zenith::print("Invalid IP address: ");
        zenith::print(ipStr);
        zenith::putchar('\n');
        zenith::exit(1);
    }

    // Parse port
    const char* portStr = skip_spaces(args + i);
    if (*portStr == '\0') {
        zenith::print("Usage: tcpconnect <ip> <port>\n");
        zenith::exit(1);
    }
    uint16_t port;
    if (!parse_uint16(portStr, &port)) {
        zenith::print("Invalid port: ");
        zenith::print(portStr);
        zenith::putchar('\n');
        zenith::exit(1);
    }

    // Create socket
    int fd = zenith::socket(Zenith::SOCK_TCP);
    if (fd < 0) {
        zenith::print("Error: failed to create socket\n");
        zenith::exit(1);
    }

    zenith::print("Connecting to ");
    print_ip(ip);
    zenith::putchar(':');
    print_int(port);
    zenith::print("...\n");

    if (zenith::connect(fd, ip, port) < 0) {
        zenith::print("Error: connection failed\n");
        zenith::closesocket(fd);
        zenith::exit(1);
    }

    zenith::print("Connected! Type to send, Ctrl+Q to disconnect.\n");

    // Interactive send/receive loop
    char sendBuf[256];
    int sendPos = 0;
    uint8_t recvBuf[512];

    while (true) {
        // Poll for received data (non-blocking)
        int r = zenith::recv(fd, recvBuf, sizeof(recvBuf) - 1);
        if (r < 0) {
            zenith::print("\nConnection closed by remote.\n");
            break;
        }
        if (r > 0) {
            recvBuf[r] = '\0';
            zenith::print((const char*)recvBuf);
        }

        // Poll keyboard
        if (zenith::is_key_available()) {
            Zenith::KeyEvent ev;
            zenith::getkey(&ev);

            if (!ev.pressed) continue;

            // Ctrl+Q to quit
            if (ev.ctrl && (ev.ascii == 'q' || ev.ascii == 'Q')) {
                zenith::print("\nDisconnecting...\n");
                break;
            }

            if (ev.ascii == '\n') {
                sendBuf[sendPos++] = '\n';
                zenith::putchar('\n');
                zenith::send(fd, sendBuf, sendPos);
                sendPos = 0;
            } else if (ev.ascii == '\b') {
                if (sendPos > 0) {
                    sendPos--;
                    zenith::putchar('\b');
                    zenith::putchar(' ');
                    zenith::putchar('\b');
                }
            } else if (ev.ascii >= ' ' && sendPos < 254) {
                sendBuf[sendPos++] = ev.ascii;
                zenith::putchar(ev.ascii);
            }
        } else {
            // No key and no data -- yield to avoid busy-spinning
            zenith::yield();
        }
    }

    zenith::closesocket(fd);
    zenith::exit(0);
}
