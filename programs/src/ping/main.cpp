/*
    * main.cpp
    * ping - Send ICMP echo requests
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>

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

extern "C" void _start() {
    char args[256];
    int len = montauk::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        montauk::print("Usage: ping <host>\n");
        montauk::exit(1);
    }

    uint32_t ip;
    if (!parse_ip(args, &ip)) {
        ip = montauk::resolve(args);
        if (ip == 0) {
            montauk::print("Could not resolve: ");
            montauk::print(args);
            montauk::putchar('\n');
            montauk::exit(1);
        }
    }

    montauk::print("PING ");
    montauk::print(args);
    montauk::print(" (");
    print_ip(ip);
    montauk::print(")\n");

    for (int i = 0; i < 4; i++) {
        int32_t rtt = montauk::ping(ip, 3000);
        if (rtt < 0) {
            montauk::print("  Request timed out\n");
        } else {
            montauk::print("  Reply from ");
            print_ip(ip);
            montauk::print(": time=");
            print_int((uint64_t)rtt);
            montauk::print("ms\n");
        }
        if (i < 3) {
            montauk::sleep_ms(1000);
        }
    }

    montauk::exit(0);
}
