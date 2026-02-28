/*
    * main.cpp
    * date - Show current date and time
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

static void print_int_padded(uint64_t n) {
    if (n < 10) montauk::putchar('0');
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
    Montauk::DateTime dt;
    montauk::gettime(&dt);

    print_int(dt.Day);
    montauk::putchar(' ');
    montauk::print(month_name(dt.Month));
    montauk::putchar(' ');
    print_int(dt.Year);
    montauk::print(", ");
    print_int(dt.Hour);
    montauk::putchar(':');
    print_int_padded(dt.Minute);
    montauk::putchar(':');
    print_int_padded(dt.Second);
    montauk::print(" UTC\n");
    montauk::exit(0);
}
