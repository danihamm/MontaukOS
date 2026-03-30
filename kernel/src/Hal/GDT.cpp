/*
    * gdt.cpp
    * Intel Global Descriptor Table
    * Copyright (c) 2025 Daniel Hammer
*/

#include "GDT.hpp"
#include "../Terminal/Terminal.hpp"
#include <Libraries/Memory.hpp>

namespace Hal {
    using namespace Kt;

    GDTPointer gdtPointer{};
    BasicGDT kernelGDT{};
    TSS64 g_tss{};

    void PrepareGDT() {
        // Zero the TSS
        memset(&g_tss, 0, sizeof(g_tss));
        g_tss.iopbOffset = sizeof(TSS64);

        kernelGDT = {
            {0xFFFF, 0, 0, 0x00, 0x00, 0},    // Null
            {0xFFFF, 0, 0, 0x9A, 0xA0, 0},    // KernelCode (DPL=0, code, 64-bit)
            {0xFFFF, 0, 0, 0x92, 0xA0, 0},    // KernelData (DPL=0, data)
            {0xFFFF, 0, 0, 0xF2, 0xA0, 0},    // UserData   (DPL=3, data)
            {0xFFFF, 0, 0, 0xFA, 0xA0, 0},    // UserCode   (DPL=3, code, 64-bit)
            {0, 0, 0, 0, 0, 0},               // TSS low  (filled below)
            {0, 0, 0, 0, 0, 0},               // TSS high (filled below)
        };

        // Encode 16-byte TSS descriptor
        uint64_t base = (uint64_t)&g_tss;
        uint32_t limit = sizeof(TSS64) - 1;

        // Low 8 bytes (normal GDT entry format)
        kernelGDT.TSS.LimitLow      = limit & 0xFFFF;
        kernelGDT.TSS.BaseLow       = base & 0xFFFF;
        kernelGDT.TSS.BaseMiddle     = (base >> 16) & 0xFF;
        kernelGDT.TSS.AccessByte     = 0x89;  // Present, 64-bit TSS Available
        kernelGDT.TSS.GranularityByte = (limit >> 16) & 0x0F;
        kernelGDT.TSS.BaseHigh       = (base >> 24) & 0xFF;

        // High 8 bytes (base[63:32] + reserved)
        uint32_t baseUpper = (uint32_t)(base >> 32);
        kernelGDT.TSSHigh.LimitLow       = baseUpper & 0xFFFF;
        kernelGDT.TSSHigh.BaseLow        = (baseUpper >> 16) & 0xFFFF;
        kernelGDT.TSSHigh.BaseMiddle      = 0;
        kernelGDT.TSSHigh.AccessByte      = 0;
        kernelGDT.TSSHigh.GranularityByte = 0;
        kernelGDT.TSSHigh.BaseHigh        = 0;

        gdtPointer = GDTPointer{
            .Size = sizeof(kernelGDT) - 1,
            .GDTAddress = (uint64_t)&kernelGDT
        };
    }

    // Helpers implemented in gdt.asm
    extern "C" void LoadGDT(GDTPointer *gdtPointer);
    extern "C" void ReloadSegments();
    extern "C" void LoadTR();

    void BridgeLoadGDT() {
        LoadGDT(&gdtPointer);
        ReloadSegments();

        KernelLogStream(DEBUG, "Hal") << "Set new GDT (0x" << base::hex << (uint64_t)&kernelGDT << ")";
    }

    void LoadTSS() {
        // LTR marks the TSS descriptor busy in the GDT (type 0xB). S3 loses
        // the CPU's loaded task register state, but the busy bit in RAM
        // persists, so a plain second LTR on resume can #GP. Re-mark it as an
        // available 64-bit TSS before reloading TR.
        kernelGDT.TSS.AccessByte = 0x89;
        LoadTR();
        KernelLogStream(OK, "Hal") << "Loaded TSS (selector 0x28)";
    }
};
