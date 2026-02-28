/*
    * main.cpp
    * DNS lookup utility for MontaukOS
    * Usage: nslookup <hostname>
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>

using montauk::skip_spaces;

static void print_ip(uint32_t ip) {
    auto print_int = [](uint32_t n) {
        if (n == 0) { montauk::putchar('0'); return; }
        char buf[4];
        int i = 0;
        while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
        for (int j = i - 1; j >= 0; j--) montauk::putchar(buf[j]);
    };

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

    const char* hostname = skip_spaces(args);
    if (len <= 0 || hostname[0] == '\0') {
        montauk::print("Usage: nslookup <hostname>\n");
        montauk::print("Example: nslookup example.com\n");
        montauk::exit(0);
    }

    // Show DNS server
    Montauk::NetCfg cfg;
    montauk::get_netcfg(&cfg);

    montauk::print("Server:  ");
    print_ip(cfg.dnsServer);
    montauk::putchar('\n');

    montauk::print("Querying ");
    montauk::print(hostname);
    montauk::print("...\n");

    uint64_t start = montauk::get_milliseconds();
    uint32_t ip = montauk::resolve(hostname);
    uint64_t elapsed = montauk::get_milliseconds() - start;

    if (ip == 0) {
        montauk::print("Error: could not resolve ");
        montauk::print(hostname);
        montauk::putchar('\n');
        montauk::exit(1);
    }

    montauk::print("Name:    ");
    montauk::print(hostname);
    montauk::putchar('\n');
    montauk::print("Address: ");
    print_ip(ip);
    montauk::putchar('\n');

    // Print timing
    montauk::print("Time:    ");
    char buf[20];
    int i = 0;
    uint64_t ms = elapsed;
    if (ms == 0) { buf[i++] = '0'; }
    else { while (ms > 0) { buf[i++] = '0' + (ms % 10); ms /= 10; } }
    for (int j = i - 1; j >= 0; j--) montauk::putchar(buf[j]);
    montauk::print(" ms\n");

    montauk::exit(0);
}
