/*
    * libmath.cpp
    * Sample shared math library for MontaukOS
    * Copyright (c) 2026 Daniel Hammer
*/

extern "C" {

// Basic math functions

int math_add(int a, int b) {
    return a + b;
}

int math_sub(int a, int b) {
    return a - b;
}

int math_mul(int a, int b) {
    return a * b;
}

int math_div(int a, int b) {
    if (b == 0) return 0;
    return a / b;
}

int math_mod(int a, int b) {
    if (b == 0) return 0;
    return a % b;
}

// Comparison
int math_max(int a, int b) {
    return (a > b) ? a : b;
}

int math_min(int a, int b) {
    return (a < b) ? a : b;
}

int math_abs(int a) {
    return (a < 0) ? -a : a;
}

// Utility
int math_pow(int base, int exp) {
    int result = 1;
    for (int i = 0; i < exp; i++) {
        result *= base;
    }
    return result;
}

}  // extern "C"
