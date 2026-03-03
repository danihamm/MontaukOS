/*
    * Cursor.hpp
    * Framebuffer information storage
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <limine.h>

namespace Graphics::Cursor {

    void Initialize(limine_framebuffer* framebuffer);

    uint32_t* GetFramebufferBase();
    uint64_t  GetFramebufferWidth();
    uint64_t  GetFramebufferHeight();
    uint64_t  GetFramebufferPitch();

    void SetFramebuffer(uint32_t* base, uint64_t width, uint64_t height, uint64_t pitch);
    uint64_t GetFramebufferPhysBase();

    void MapWriteCombining();

};
