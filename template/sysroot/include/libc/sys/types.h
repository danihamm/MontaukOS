#ifndef _LIBC_SYS_TYPES_H
#define _LIBC_SYS_TYPES_H

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long size_t;
typedef long          ssize_t;
typedef long          off_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef long          time_t;

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_SYS_TYPES_H */
