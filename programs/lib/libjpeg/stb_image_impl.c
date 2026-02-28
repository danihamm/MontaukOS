/*
 * stb_image_impl.c
 * Single compilation unit for stb_image JPEG decoder on MontaukOS
 * Copyright (c) 2026 Daniel Hammer
 */

#include <stdlib.h>
#include <string.h>

/* Only enable JPEG; disable everything else. */
#define STBI_ONLY_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS

/* Route allocations through libc (which uses MontaukOS heap). */
#define STBI_MALLOC(sz)         malloc(sz)
#define STBI_FREE(p)            free(p)
#define STBI_REALLOC(p, newsz)  realloc(p, newsz)

#define STB_IMAGE_IMPLEMENTATION
#include <gui/stb_image.h>
