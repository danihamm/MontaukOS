/*
    * main.cpp
    * ping - Send ICMP echo requests
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>

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

extern "C" void _start() {
    char args[256];
    int len = zenith::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        zenith::print("Usage: ping <ip address>\n");
        zenith::exit(1);
    }

    uint32_t ip;
    if (!parse_ip(args, &ip)) {
        zenith::print("Invalid IP address: ");
        zenith::print(args);
        zenith::putchar('\n');
        zenith::exit(1);
    }

    zenith::print("PING ");
    print_ip(ip);
    zenith::putchar('\n');

    for (int i = 0; i < 4; i++) {
        int32_t rtt = zenith::ping(ip, 3000);
        if (rtt < 0) {
            zenith::print("  Request timed out\n");
        } else {
            zenith::print("  Reply from ");
            print_ip(ip);
            zenith::print(": time=");
            print_int((uint64_t)rtt);
            zenith::print("ms\n");
        }
        if (i < 3) {
            zenith::sleep_ms(1000);
        }
    }

    zenith::exit(0);
}
