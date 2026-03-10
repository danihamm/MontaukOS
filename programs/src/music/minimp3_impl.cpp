/*
 * minimp3_impl.cpp
 * Single compilation unit for minimp3 in MontaukOS freestanding environment
 * Copyright (c) 2026 Daniel Hammer
 */

#include <cstdint>
#include <cstddef>

extern "C" {
#include <string.h>
#include <stdlib.h>
}

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
