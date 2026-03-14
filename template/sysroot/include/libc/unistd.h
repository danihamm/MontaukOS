#ifndef _LIBC_UNISTD_H
#define _LIBC_UNISTD_H

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int    read(int fd, void *buf, size_t count);
int    write(int fd, const void *buf, size_t count);
int    close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_UNISTD_H */
