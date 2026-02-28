/*
    * main.cpp
    * info - Show system information
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

extern "C" void _start() {
    Montauk::SysInfo info;
    montauk::get_info(&info);
    montauk::print(info.osName);
    montauk::print(" v");
    montauk::print(info.osVersion);
    montauk::putchar('\n');
    montauk::print("Syscall API version: ");
    print_int(info.apiVersion);
    montauk::putchar('\n');
    montauk::exit(0);
}
