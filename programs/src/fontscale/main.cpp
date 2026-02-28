/*
    * main.cpp
    * fontscale - Change terminal font scale
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>

static int atoi(const char* s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static void print_int(int n) {
    char buf[16];
    int i = 0;
    if (n == 0) {
        montauk::putchar('0');
        return;
    }
    while (n > 0 && i < 15) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) montauk::putchar(buf[--i]);
}

extern "C" void _start() {
    char args[128];
    int len = montauk::getargs(args, sizeof(args));

    if (len <= 0 || args[0] == '\0') {
        // No args: show current scale
        int sx, sy;
        montauk::get_termscale(&sx, &sy);
        int cols, rows;
        montauk::termsize(&cols, &rows);

        montauk::print("Font scale: ");
        print_int(sx);
        montauk::print("x");
        print_int(sy);
        montauk::print("  Terminal: ");
        print_int(cols);
        montauk::print("x");
        print_int(rows);
        montauk::putchar('\n');
        montauk::exit(0);
    }

    // Parse arguments
    const char* p = montauk::skip_spaces(args);
    int scale_x = atoi(p);

    // Skip past first number to find optional second
    while (*p >= '0' && *p <= '9') p++;
    p = montauk::skip_spaces(p);
    int scale_y = (*p >= '1' && *p <= '8') ? atoi(p) : scale_x;

    if (scale_x < 1 || scale_x > 8 || scale_y < 1 || scale_y > 8) {
        montauk::print("fontscale: scale must be 1-8\n");
        montauk::exit(1);
    }

    montauk::termscale(scale_x, scale_y);

    // Clear and show result
    montauk::print("\033[2J\033[H");

    int cols, rows;
    montauk::termsize(&cols, &rows);
    montauk::print("Font scale set to ");
    print_int(scale_x);
    montauk::print("x");
    print_int(scale_y);
    montauk::print("  (");
    print_int(cols);
    montauk::print("x");
    print_int(rows);
    montauk::print(")\n");

    montauk::exit(0);
}
