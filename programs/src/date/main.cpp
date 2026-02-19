/*
    * main.cpp
    * date - Show current date and time
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

static void print_int_padded(uint64_t n) {
    if (n < 10) zenith::putchar('0');
    print_int(n);
}

static const char* month_name(int m) {
    static const char* months[] = {
        "", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    if (m >= 1 && m <= 12) return months[m];
    return "?";
}

extern "C" void _start() {
    Zenith::DateTime dt;
    zenith::gettime(&dt);

    print_int(dt.Day);
    zenith::putchar(' ');
    zenith::print(month_name(dt.Month));
    zenith::putchar(' ');
    print_int(dt.Year);
    zenith::print(", ");
    print_int(dt.Hour);
    zenith::putchar(':');
    print_int_padded(dt.Minute);
    zenith::putchar(':');
    print_int_padded(dt.Second);
    zenith::print(" UTC\n");
    zenith::exit(0);
}
