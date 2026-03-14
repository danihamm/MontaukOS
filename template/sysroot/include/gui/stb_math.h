/*
    * stb_math.h
    * Math functions for stb_truetype in freestanding environment
    * Copyright (c) 2026 Daniel Hammer
*/

#ifndef STB_MATH_H
#define STB_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

static inline double stb_floor(double x) {
    double i = (double)(long long)x;
    return (x < i) ? i - 1.0 : i;
}

static inline double stb_ceil(double x) {
    double f = stb_floor(x);
    return (x > f) ? f + 1.0 : f;
}

static inline double stb_fabs(double x) {
    return x < 0.0 ? -x : x;
}

static inline double stb_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - (double)((long long)(x / y)) * y;
}

static inline double stb_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double guess = x;
    for (int i = 0; i < 30; i++)
        guess = (guess + x / guess) * 0.5;
    return guess;
}

static inline double stb_pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (exp == 1.0) return base;
    if (base == 0.0) return 0.0;
    // Integer exponent fast path
    if (exp == (double)(long long)exp) {
        long long e = (long long)exp;
        int neg = 0;
        if (e < 0) { neg = 1; e = -e; }
        double r = 1.0;
        double b = base;
        while (e > 0) {
            if (e & 1) r *= b;
            b *= b;
            e >>= 1;
        }
        return neg ? 1.0 / r : r;
    }
    return 0.0;
}

static inline double stb_cos(double x) {
    // Reduce to [0, 2*pi]
    const double PI = 3.14159265358979323846;
    const double TWO_PI = 6.28318530717958647692;
    x = stb_fmod(stb_fabs(x), TWO_PI);
    // Taylor series: cos(x) = 1 - x^2/2! + x^4/4! - x^6/6! + ...
    double x2 = x * x;
    double term = 1.0;
    double result = 1.0;
    for (int i = 1; i <= 10; i++) {
        term *= -x2 / (double)((2 * i - 1) * (2 * i));
        result += term;
    }
    return result;
}

static inline double stb_acos(double x) {
    // Clamp input
    if (x <= -1.0) return 3.14159265358979323846;
    if (x >= 1.0) return 0.0;
    // Polynomial approximation (Abramowitz & Stegun style)
    double ax = stb_fabs(x);
    double result = (-0.0187293 * ax + 0.0742610) * ax - 0.2121144;
    result = (result * ax + 1.5707288) * stb_sqrt(1.0 - ax);
    if (x < 0.0)
        return 3.14159265358979323846 - result;
    return result;
}

#ifdef __cplusplus
}
#endif

#endif // STB_MATH_H
