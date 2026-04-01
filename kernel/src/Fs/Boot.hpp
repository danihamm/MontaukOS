/*
    * Boot.hpp
    * Boot-time filesystem initialization
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <limine.h>

namespace Fs {

    void InitializeBootFilesystems(const volatile limine_module_response* moduleResponse);

}
