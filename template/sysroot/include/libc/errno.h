#ifndef _LIBC_ERRNO_H
#define _LIBC_ERRNO_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#define ENOENT  2
#define EIO     5
#define ENOMEM  12
#define EACCES  13
#define EINVAL  22
#define ERANGE  34
#define ENOSYS  38
#define EISDIR  21
#define ENOTDIR 20
#define EEXIST  17
#define EBADF   9
#define EPERM   1

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_ERRNO_H */
