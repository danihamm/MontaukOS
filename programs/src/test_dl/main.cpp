/*
    * test_dl.cpp
    * Test application for dynamic library loading
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <libloader/libloader.h>

// Forward declarations
static void print_int(int val);
static void print_hex(uint64_t val);

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

    // Test loading libhello (GUI library)
    montauk::print("\nLoading libhello (GUI library)...\n");
    LibHandle* hellolib = libloader::dlopen("0:/os/libhello.lib");
    if (!hellolib) {
        montauk::print("ERROR: Failed to load libhello.lib\n");
        montauk::exit(1);
    }
    montauk::print("Library loaded successfully!\n\n");

    // Resolve hello_run
    typedef int (*hello_run_t)(void);
    hello_run_t hello_run = (hello_run_t)libloader::dlsym(hellolib, "hello_run");
    if (!hello_run) {
        montauk::print("ERROR: Failed to resolve hello_run\n");
        montauk::exit(1);
    }
    montauk::print("  hello_run: OK\n\n");

    // Debug: print hello_run address
    montauk::print("  DEBUG: hello_run @ ");
    print_hex((uint64_t)hello_run);
    montauk::print("\n");

    // Run the GUI demo (this creates a window and event loop)
    montauk::print("Running GUI demo...\n");
    hello_run();

    // Unload the library
    montauk::print("GUI demo closed.\n");
    montauk::print("Unloading library...\n");
    libloader::dlclose(hellolib);
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

// Print hex value (uppercase, with 0x prefix)
static void print_hex(uint64_t val) {
    montauk::print("0x");
    char buf[20];
    int idx = 0;

    if (val == 0) {
        buf[idx++] = '0';
    } else {
        while (val > 0) {
            int digit = val & 0xF;
            buf[idx++] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
            val >>= 4;
        }
    }

    // Reverse the buffer
    for (int i = idx - 1; i >= 0; i--) {
        montauk::putchar(buf[i]);
    }
}
