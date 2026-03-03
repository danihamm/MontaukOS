/*
    * Init.cpp
    * Driver initialization orchestration
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Init.hpp"
#include <Drivers/Graphics/IntelGPU.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>
#include <Graphics/Cursor.hpp>
#include <Net/Net.hpp>
#include <Terminal/Terminal.hpp>

namespace Drivers {

    void InitializeGraphics() {
        Graphics::IntelGPU::Initialize();
        if (Graphics::IntelGPU::IsInitialized()) {
            ::Graphics::Cursor::SetFramebuffer(
                Graphics::IntelGPU::GetFramebufferBase(),
                Graphics::IntelGPU::GetWidth(),
                Graphics::IntelGPU::GetHeight(),
                Graphics::IntelGPU::GetPitch()
            );
        }
    }

    void InitializeNetwork() {
        Net::E1000::Initialize();
        if (!Net::E1000::IsInitialized()) {
            Kt::KernelLogStream(Kt::INFO, "Init") << "E1000 not found, trying E1000E...";
            Net::E1000E::Initialize();
        }
        ::Net::Initialize();
    }

}
