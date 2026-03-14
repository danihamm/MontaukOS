#ifndef _LIBC_FCNTL_H
#define _LIBC_FCNTL_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_FCNTL_H */
