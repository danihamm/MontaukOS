/*
 * stb_truetype_impl.cpp
 * Single compilation unit for stb_truetype in ZenithOS freestanding environment
 * Copyright (c) 2026 Daniel Hammer
 */

#include <cstdint>
#include <cstddef>
#include <zenith/heap.h>
#include <zenith/string.h>
#include <gui/stb_math.h>

// Override all stb_truetype dependencies before including the implementation

#define STBTT_ifloor(x)   ((int) stb_floor(x))
#define STBTT_iceil(x)    ((int) stb_ceil(x))
#define STBTT_sqrt(x)     stb_sqrt(x)
#define STBTT_pow(x,y)    stb_pow(x,y)
#define STBTT_fmod(x,y)   stb_fmod(x,y)
#define STBTT_cos(x)      stb_cos(x)
#define STBTT_acos(x)     stb_acos(x)
#define STBTT_fabs(x)     stb_fabs(x)

#define STBTT_malloc(x,u)  ((void)(u), zenith::malloc(x))
#define STBTT_free(x,u)    ((void)(u), zenith::mfree(x))

#define STBTT_memcpy(d,s,n) zenith::memcpy(d,s,n)
#define STBTT_memset(d,v,n) zenith::memset(d,v,n)

#define STBTT_strlen(x)    zenith::slen(x)

#define STBTT_assert(x)    ((void)(x))

#define STB_TRUETYPE_IMPLEMENTATION
#include <gui/stb_truetype.h>
