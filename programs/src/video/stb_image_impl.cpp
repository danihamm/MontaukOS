/*
 * stb_image_impl.cpp
 * Single compilation unit for JPEG decoding in the MontaukOS Videos app
 * Copyright (c) 2026 Daniel Hammer
 */

#include <cstdint>
#include <cstddef>

extern "C" {
#include <stdlib.h>
#include <string.h>
}

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

#define STBI_MALLOC(sz)         malloc(sz)
#define STBI_FREE(p)            free(p)
#define STBI_REALLOC(p, newsz)  realloc(p, newsz)
#define STBI_ASSERT(x)          ((void)(x))

#define STB_IMAGE_IMPLEMENTATION
#include <gui/stb_image.h>
