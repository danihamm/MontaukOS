#ifndef _LIBC_STDIO_H
#define _LIBC_STDIO_H

#pragma once

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF      (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   1024
#define FILENAME_MAX 256

typedef struct _FILE {
    int           handle;
    unsigned long pos;
    unsigned long size;
    int           eof;
    int           error;
    int           is_std;
    int           ungetc_buf;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int    printf(const char *fmt, ...);
int    fprintf(FILE *stream, const char *fmt, ...);
int    sprintf(char *str, const char *fmt, ...);
int    snprintf(char *str, size_t size, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *stream, const char *fmt, va_list ap);
int    vsprintf(char *str, const char *fmt, va_list ap);
int    vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

int    puts(const char *s);
int    putchar(int c);

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
int    fflush(FILE *stream);

int    rename(const char *oldpath, const char *newpath);
int    remove(const char *path);

int    sscanf(const char *str, const char *fmt, ...);

int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);

int    fgetc(FILE *stream);
int    getc(FILE *stream);
int    ungetc(int c, FILE *stream);
char  *fgets(char *s, int size, FILE *stream);
int    fputs(const char *s, FILE *stream);

void   perror(const char *s);
FILE  *tmpfile(void);
char  *tmpnam(char *s);

#ifdef __cplusplus
}
#endif

#endif /* _LIBC_STDIO_H */
