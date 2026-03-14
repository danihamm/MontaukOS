#ifndef _LIBC_SYS_STAT_H
#define _LIBC_SYS_STAT_H

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat {
    unsigned long st_size;
};

int mkdir(const char *path, unsigned int mode);
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SYS_STAT_H */
