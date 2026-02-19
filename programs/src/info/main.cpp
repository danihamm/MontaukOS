/*
    * main.cpp
    * info - Show system information
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

extern "C" void _start() {
    Zenith::SysInfo info;
    zenith::get_info(&info);
    zenith::print(info.osName);
    zenith::print(" v");
    zenith::print(info.osVersion);
    zenith::putchar('\n');
    zenith::print("Syscall API version: ");
    print_int(info.apiVersion);
    zenith::putchar('\n');
    zenith::exit(0);
}
