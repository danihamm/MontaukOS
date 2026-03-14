#ifndef _LIBC_MATH_H
#define _LIBC_MATH_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define HUGE_VAL  __builtin_huge_val()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")

double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double atan2(double y, double x);
double pow(double base, double exp);
double log(double x);
double exp(double x);
double fmod(double x, double y);
double round(double x);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_MATH_H */
