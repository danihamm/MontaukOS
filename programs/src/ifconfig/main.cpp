/*
    * main.cpp
    * ifconfig - Show or set network configuration
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>

using montauk::starts_with;
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

static void print_ip(uint32_t ip) {
    print_int(ip & 0xFF);
    montauk::putchar('.');
    print_int((ip >> 8) & 0xFF);
    montauk::putchar('.');
    print_int((ip >> 16) & 0xFF);
    montauk::putchar('.');
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
    int len = montauk::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        // Show current network configuration
        Montauk::NetCfg cfg;
        montauk::get_netcfg(&cfg);
        montauk::print("  IP Address:   ");
        print_ip(cfg.ipAddress);
        montauk::putchar('\n');
        montauk::print("  Subnet Mask:  ");
        print_ip(cfg.subnetMask);
        montauk::putchar('\n');
        montauk::print("  Gateway:      ");
        print_ip(cfg.gateway);
        montauk::putchar('\n');
        montauk::print("  DNS Server:   ");
        print_ip(cfg.dnsServer);
        montauk::putchar('\n');
        montauk::exit(0);
    }

    if (!starts_with(args, "set ")) {
        montauk::print("Usage: ifconfig              Show network config\n");
        montauk::print("       ifconfig set <ip> <mask> <gateway>\n");
        montauk::exit(1);
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
        montauk::print("Invalid IP address: ");
        montauk::print(tok);
        montauk::putchar('\n');
        montauk::exit(1);
    }
    p = skip_spaces(p + i);

    // Parse subnet mask
    i = 0;
    while (p[i] && p[i] != ' ' && i < 31) { tok[i] = p[i]; i++; }
    tok[i] = '\0';
    uint32_t mask;
    if (!parse_ip(tok, &mask)) {
        montauk::print("Invalid subnet mask: ");
        montauk::print(tok);
        montauk::putchar('\n');
        montauk::exit(1);
    }
    p = skip_spaces(p + i);

    // Parse gateway
    i = 0;
    while (p[i] && p[i] != ' ' && i < 31) { tok[i] = p[i]; i++; }
    tok[i] = '\0';
    uint32_t gw;
    if (!parse_ip(tok, &gw)) {
        montauk::print("Invalid gateway: ");
        montauk::print(tok);
        montauk::putchar('\n');
        montauk::exit(1);
    }

    Montauk::NetCfg cfg;
    cfg.ipAddress  = ip;
    cfg.subnetMask = mask;
    cfg.gateway    = gw;
    if (montauk::set_netcfg(&cfg) < 0) {
        montauk::print("Error: failed to set network config\n");
        montauk::exit(1);
    }

    montauk::print("Network config updated:\n");
    montauk::print("  IP Address:   "); print_ip(ip); montauk::putchar('\n');
    montauk::print("  Subnet Mask:  "); print_ip(mask); montauk::putchar('\n');
    montauk::print("  Gateway:      "); print_ip(gw); montauk::putchar('\n');
    montauk::exit(0);
}
