/*
    * Scheduler.hpp
    * Preemptive process scheduler with user-mode support
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Api/Syscall.hpp>

namespace Sched {

    static constexpr int MaxProcesses = 16;
    static constexpr uint64_t StackPages = 4;  // 16 KiB kernel stack per process
    static constexpr uint64_t StackSize = StackPages * 0x1000;
    static constexpr uint64_t UserStackPages = 4;  // 16 KiB user stack
    static constexpr uint64_t UserStackSize = UserStackPages * 0x1000;
    static constexpr uint64_t UserStackTop = 0x7FFFFFF000ULL;  // User stack top VA
    static constexpr uint64_t UserHeapBase = 0x40000000ULL;    // User heap start VA
    static constexpr uint64_t ExitStubAddr = 0x3FF000ULL;     // User-space exit stub page
    static constexpr uint64_t TimeSliceMs = 10; // 10 ms time slice

    enum class ProcessState {
        Free,
        Ready,
        Running,
        Terminated
    };

    struct Process {
        int pid;
        ProcessState state;
        const char* name;
        uint64_t savedRsp;
        uint64_t stackBase;       // Bottom of allocated kernel stack (lowest address)
        uint64_t entryPoint;
        uint64_t sliceRemaining;  // Ticks left in current time slice
        uint64_t pml4Phys;        // Physical address of per-process PML4
        uint64_t kernelStackTop;  // Top of kernel stack (for TSS RSP0 / SYSCALL)
        uint64_t userStackTop;    // User-space stack top
        uint64_t heapNext;        // Simple bump allocator for user heap
        char args[256];           // Command-line arguments (set by parent via Spawn)

        // I/O redirection for GUI terminal
        bool redirected = false;
        int parentPid = -1;
        uint8_t* outBuf = nullptr;   // 4KB ring: child writes (print/putchar), parent reads
        uint32_t outHead = 0;
        uint32_t outTail = 0;
        uint8_t* inBuf = nullptr;    // 4KB ring: parent writes, child reads (getchar)
        uint32_t inHead = 0;
        uint32_t inTail = 0;
        Zenith::KeyEvent keyBuf[64]; // parent injects, child reads (getkey/iskeyavailable)
        uint32_t keyHead = 0;
        uint32_t keyTail = 0;
        static constexpr uint32_t IoBufSize = 4096;

        // GUI terminal dimensions (set by desktop, read by SYS_TERMSIZE)
        int termCols = 0;
        int termRows = 0;
    };

    void Initialize();
    int Spawn(const char* vfsPath, const char* args = nullptr);
    void Schedule();

    // Called from the APIC timer handler on every tick.
    void Tick();

    // Get the PID of the currently running process (-1 if idle)
    int GetCurrentPid();

    // Get a pointer to the currently running process (nullptr if idle)
    Process* GetCurrentProcessPtr();

    // Called by terminated processes to mark themselves done
    void ExitProcess();

    // Check if a process is still alive (Ready or Running)
    bool IsAlive(int pid);

    // Find a process by PID (returns nullptr if not found or not alive)
    Process* GetProcessByPid(int pid);

}
