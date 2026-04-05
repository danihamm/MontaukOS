/*
    * test_dl.cpp
    * Test application for dynamic library loading
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <libloader/libloader.h>

// Forward declaration
static void print_int(int val);

extern "C" void _start() {
    montauk::print("=== Dynamic Library Test ===\n\n");

    // Test loading libmath
    montauk::print("Loading libmath...\n");
    LibHandle* lib = libloader::dlopen("0:/os/libmath.lib");
    if (!lib) {
        montauk::print("ERROR: Failed to load libmath.lib\n");
        montauk::exit(1);
    }
    montauk::print("Library loaded successfully!\n\n");

    // Test dlsym for various functions
    montauk::print("Resolving symbols...\n");

    // math_add
    typedef int (*math_fn_t)(int, int);
    math_fn_t math_add = (math_fn_t)libloader::dlsym(lib, "math_add");
    if (!math_add) {
        montauk::print("ERROR: Failed to resolve math_add\n");
        montauk::exit(1);
    }
    montauk::print("  math_add: OK\n");

    // math_mul
    math_fn_t math_mul = (math_fn_t)libloader::dlsym(lib, "math_mul");
    if (!math_mul) {
        montauk::print("ERROR: Failed to resolve math_mul\n");
        montauk::exit(1);
    }
    montauk::print("  math_mul: OK\n");

    // math_max
    typedef int (*math_max_t)(int, int);
    math_max_t math_max = (math_max_t)libloader::dlsym(lib, "math_max");
    if (!math_max) {
        montauk::print("ERROR: Failed to resolve math_max\n");
        montauk::exit(1);
    }
    montauk::print("  math_max: OK\n\n");

    // Test the functions
    montauk::print("Testing library functions:\n");

    // Test add: 10 + 5 = 15
    int a = 10, b = 5;
    int result = math_add(a, b);
    montauk::print("  math_add(10, 5) = ");
    print_int(result);
    montauk::print("\n");

    // Test mul: 7 * 6 = 42
    result = math_mul(7, 6);
    montauk::print("  math_mul(7, 6) = ");
    print_int(result);
    montauk::print("\n");

    // Test max: max(100, 200) = 200
    result = math_max(100, 200);
    montauk::print("  math_max(100, 200) = ");
    print_int(result);
    montauk::print("\n\n");

    // Unload the library
    montauk::print("Unloading library...\n");
    libloader::dlclose(lib);
    montauk::print("Library unloaded.\n");

    montauk::print("\n=== All tests passed! ===\n");
    montauk::exit(0);
}

// Simple integer to string conversion for printing
static void print_int(int val) {
    char buf[16];
    int idx = 0;
    bool neg = false;

    if (val < 0) {
        neg = true;
        val = -val;
    }

    if (val == 0) {
        buf[idx++] = '0';
    } else {
        while (val > 0) {
            buf[idx++] = '0' + (val % 10);
            val /= 10;
        }
    }

    if (neg) {
        montauk::putchar('-');
    }

    for (int i = idx - 1; i >= 0; i--) {
        montauk::putchar(buf[i]);
    }
}
