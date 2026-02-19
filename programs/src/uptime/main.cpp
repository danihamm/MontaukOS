/*
    * main.cpp
    * uptime - Show system uptime
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
    uint64_t ms = zenith::get_milliseconds();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    secs %= 60;
    ms %= 1000;

    zenith::print("Uptime: ");
    print_int(mins);
    zenith::print("m ");
    print_int(secs);
    zenith::print("s ");
    print_int(ms);
    zenith::print("ms\n");
    zenith::exit(0);
}
