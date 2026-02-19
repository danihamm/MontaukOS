/*
    * main.cpp
    * ifconfig - Show or set network configuration
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::starts_with;
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

static void print_ip(uint32_t ip) {
    print_int(ip & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 8) & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 16) & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 24) & 0xFF);
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

extern "C" void _start() {
    char args[256];
    int len = zenith::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        // Show current network configuration
        Zenith::NetCfg cfg;
        zenith::get_netcfg(&cfg);
        zenith::print("  IP Address:   ");
        print_ip(cfg.ipAddress);
        zenith::putchar('\n');
        zenith::print("  Subnet Mask:  ");
        print_ip(cfg.subnetMask);
        zenith::putchar('\n');
        zenith::print("  Gateway:      ");
        print_ip(cfg.gateway);
        zenith::putchar('\n');
        zenith::exit(0);
    }

    if (!starts_with(args, "set ")) {
        zenith::print("Usage: ifconfig              Show network config\n");
        zenith::print("       ifconfig set <ip> <mask> <gateway>\n");
        zenith::exit(1);
    }

    // Parse: set <ip> <mask> <gateway>
    const char* p = skip_spaces(args + 4);

    // Parse IP
    char tok[32];
    int i = 0;
    while (p[i] && p[i] != ' ' && i < 31) { tok[i] = p[i]; i++; }
    tok[i] = '\0';
    uint32_t ip;
    if (!parse_ip(tok, &ip)) {
        zenith::print("Invalid IP address: ");
        zenith::print(tok);
        zenith::putchar('\n');
        zenith::exit(1);
    }
    p = skip_spaces(p + i);

    // Parse subnet mask
    i = 0;
    while (p[i] && p[i] != ' ' && i < 31) { tok[i] = p[i]; i++; }
    tok[i] = '\0';
    uint32_t mask;
    if (!parse_ip(tok, &mask)) {
        zenith::print("Invalid subnet mask: ");
        zenith::print(tok);
        zenith::putchar('\n');
        zenith::exit(1);
    }
    p = skip_spaces(p + i);

    // Parse gateway
    i = 0;
    while (p[i] && p[i] != ' ' && i < 31) { tok[i] = p[i]; i++; }
    tok[i] = '\0';
    uint32_t gw;
    if (!parse_ip(tok, &gw)) {
        zenith::print("Invalid gateway: ");
        zenith::print(tok);
        zenith::putchar('\n');
        zenith::exit(1);
    }

    Zenith::NetCfg cfg;
    cfg.ipAddress  = ip;
    cfg.subnetMask = mask;
    cfg.gateway    = gw;
    if (zenith::set_netcfg(&cfg) < 0) {
        zenith::print("Error: failed to set network config\n");
        zenith::exit(1);
    }

    zenith::print("Network config updated:\n");
    zenith::print("  IP Address:   "); print_ip(ip); zenith::putchar('\n');
    zenith::print("  Subnet Mask:  "); print_ip(mask); zenith::putchar('\n');
    zenith::print("  Gateway:      "); print_ip(gw); zenith::putchar('\n');
    zenith::exit(0);
}
