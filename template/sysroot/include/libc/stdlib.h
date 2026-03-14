#ifndef _LIBC_STDLIB_H
#define _LIBC_STDLIB_H

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7fffffff

typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

void  *malloc(size_t size);
void   free(void *ptr);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);

int    atoi(const char *s);
long   atol(const char *s);
double atof(const char *s);

int    abs(int j);
long   labs(long j);

void   exit(int status);
void   abort(void);
int    atexit(void (*func)(void));

char  *getenv(const char *name);

void   qsort(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

long          strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

int    rand(void);
void   srand(unsigned int seed);

div_t  div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);

int    system(const char *command);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_STDLIB_H */
