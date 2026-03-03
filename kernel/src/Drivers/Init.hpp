/*
    * Init.hpp
    * Driver initialization orchestration
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once

namespace Drivers {

    // Initialize Intel GPU driver and update the cursor framebuffer if found.
    void InitializeGraphics();

    // Initialize network driver (E1000, falling back to E1000E) and the net stack.
    void InitializeNetwork();

}
