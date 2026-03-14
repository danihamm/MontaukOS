#ifndef _LIBC_ASSERT_H
#define _LIBC_ASSERT_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *expr, const char *file, int line, const char *func);

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_ASSERT_H */
