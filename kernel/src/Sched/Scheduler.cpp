/*
    * Scheduler.cpp
    * Preemptive process scheduler with SMP support
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include "Scheduler.hpp"
#include "ElfLoader.hpp"
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <CppLib/Spinlock.hpp>
#include <Hal/Apic/Apic.hpp>
#include <Hal/GDT.hpp>
#include <Hal/SmpBoot.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Api/WinServer.hpp>
#include <Ipc/Ipc.hpp>

// Assembly: context switch with CR3 and FPU state parameters
extern "C" void SchedContextSwitch(uint64_t* oldRsp, uint64_t newRsp, uint64_t newCR3,
                                   uint8_t* oldFpuArea, uint8_t* newFpuArea);

// Assembly: jump to user mode via IRETQ
extern "C" void JumpToUserMode(uint64_t rip, uint64_t rsp);

namespace Sched {

    static Process processTable[MaxProcesses];
    static int nextPid = 0;

    // The scheduler lock MUST be a Spinlock (interrupt-disabling).
    // It is held ACROSS context switches to prevent the race where
    // another CPU picks up a process whose RSP hasn't been saved yet.
    // The resumed process releases it.
    static kcp::Spinlock schedLock;

    // Approximate count of Ready processes. Incremented/decremented
    // under schedLock. Idle CPUs check this to avoid scanning all 256
    // process slots on every timer tick.
    static volatile int readyCount = 0;

    // The idle loop runs in the kernel PML4
    static uint64_t GetKernelCR3() {
        return (uint64_t)Memory::VMM::g_paging->PML4;
    }

    static void SwitchAwayFromBlockedCurrentLocked() {
        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;
        if (slot < 0) {
            schedLock.Release();
            return;
        }

        int next = -1;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Ready) {
                next = i;
                break;
            }
        }

        if (next >= 0) {
            cpu->currentSlot = next;
            processTable[next].state = ProcessState::Running;
            readyCount--;
            processTable[next].runningOnCpu = cpu->cpuIndex;
            processTable[next].sliceRemaining = TimeSliceMs;

            cpu->kernelRsp = processTable[next].kernelStackTop;
            cpu->tss->rsp0 = processTable[next].kernelStackTop;

            SchedContextSwitch(&processTable[slot].savedRsp, processTable[next].savedRsp,
                              processTable[next].pml4Phys,
                              processTable[slot].fpuState, processTable[next].fpuState);
            schedLock.Release();
            return;
        }

        cpu->currentSlot = -1;
        SchedContextSwitch(&processTable[slot].savedRsp, cpu->idleSavedRsp,
                          GetKernelCR3(), processTable[slot].fpuState, nullptr);
        schedLock.Release();
    }

    // Startup function for newly spawned processes.
    // SchedContextSwitch "returns" here on first schedule.
    // The schedLock is held (acquired by the switching-from CPU's Schedule).
    static void ProcessStartup() {
        // Release the schedLock that the switching-from CPU held
        schedLock.Release();

        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;

        if (slot >= 0) {
            Process& proc = processTable[slot];

            // Set up per-CPU kernel RSP for SYSCALL entry
            cpu->kernelRsp = proc.kernelStackTop;

            // Set up per-CPU TSS RSP0 for hardware interrupts from ring 3
            cpu->tss->rsp0 = proc.kernelStackTop;

            // Jump to user mode (never returns)
            JumpToUserMode(proc.entryPoint, proc.userStackTop);
        }

        ExitProcess();
        for (;;) {
            asm volatile("hlt");
        }
    }

    void Initialize() {
        for (int i = 0; i < MaxProcesses; i++) {
            processTable[i].pid = i;
            processTable[i].state = ProcessState::Free;
            processTable[i].name[0] = '\0';
            processTable[i].savedRsp = 0;
            processTable[i].stackBase = 0;
            processTable[i].entryPoint = 0;
            processTable[i].sliceRemaining = 0;
            processTable[i].pml4Phys = 0;
            processTable[i].kernelStackTop = 0;
            processTable[i].userStackTop = 0;
            processTable[i].heapNext = 0;
            processTable[i].readdirCursor = 0;
            processTable[i].args[0] = '\0';
            processTable[i].user[0] = '\0';
            processTable[i].cwd[0] = '\0';
            processTable[i].runningOnCpu = -1;
            processTable[i].killPending = false;
            processTable[i].waitingForPid = -1;
            processTable[i].sleepUntilTick = 0;
            processTable[i].waitingOnObject = nullptr;
            processTable[i].redirected = false;
            processTable[i].parentPid = -1;
            processTable[i].outBuf = nullptr;
            processTable[i].outHead = 0;
            processTable[i].outTail = 0;
            processTable[i].inBuf = nullptr;
            processTable[i].inHead = 0;
            processTable[i].inTail = 0;
            processTable[i].keyHead = 0;
            processTable[i].keyTail = 0;
            processTable[i].termCols = 0;
            processTable[i].termRows = 0;
            processTable[i].ioOutHandle = -1;
            processTable[i].ioInHandle = -1;
            processTable[i].ioKeyHandle = -1;
            processTable[i].ioWaitsetHandle = -1;
        }

        nextPid = 0;

        Kt::KernelLogStream(Kt::OK, "Sched") << "Initialized (" << MaxProcesses
            << " process slots, " << (uint64_t)TimeSliceMs << " ms time slice)";
    }

    int Spawn(const char* vfsPath, const char* args) {
        schedLock.Acquire();

        int slot = -1;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Free) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            schedLock.Release();
            Kt::KernelLogStream(Kt::ERROR, "Sched") << "No free process slots";
            return -1;
        }

        // Reserve the slot so another Spawn doesn't claim it.
        // Use Running (not Ready!) so the scheduler doesn't try to
        // dispatch this half-initialized process.
        processTable[slot].state = ProcessState::Running;
        processTable[slot].runningOnCpu = -1;
        schedLock.Release();

        // Create per-process PML4 with kernel-half copied
        uint64_t pml4Phys = Memory::VMM::Paging::CreateUserPML4();

        // Load ELF into the process's address space
        uint64_t entry = ElfLoad(vfsPath, pml4Phys);
        if (entry == 0) {
            Memory::VMM::Paging::FreeUserHalf(pml4Phys);
            Memory::g_pfa->Free((void*)Memory::HHDM(pml4Phys));
            schedLock.Acquire();
            processTable[slot].state = ProcessState::Free;
            schedLock.Release();
            return -1;
        }

        // Allocate kernel stack (used during syscalls and interrupts)
        void* firstPage = Memory::g_pfa->AllocateZeroed();
        if (firstPage == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "Sched") << "Out of memory for kernel stack";
            Memory::VMM::Paging::FreeUserHalf(pml4Phys);
            Memory::g_pfa->Free((void*)Memory::HHDM(pml4Phys));
            schedLock.Acquire();
            processTable[slot].state = ProcessState::Free;
            schedLock.Release();
            return -1;
        }
        void* stackMem = Memory::g_pfa->ReallocConsecutive(firstPage, StackPages);
        if (stackMem == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "Sched") << "Failed to allocate contiguous kernel stack";
            Memory::g_pfa->Free(firstPage);
            Memory::VMM::Paging::FreeUserHalf(pml4Phys);
            Memory::g_pfa->Free((void*)Memory::HHDM(pml4Phys));
            schedLock.Acquire();
            processTable[slot].state = ProcessState::Free;
            schedLock.Release();
            return -1;
        }

        uint8_t* kernelStackBase = (uint8_t*)stackMem;
        uint64_t kernelStackTop = (uint64_t)kernelStackBase + StackSize;

        // Helper to clean up all resources allocated so far on failure
        auto cleanupOnFail = [&]() {
            Memory::VMM::Paging::FreeUserHalf(pml4Phys);
            Memory::g_pfa->Free((void*)Memory::HHDM(pml4Phys));
            Memory::g_pfa->Free(stackMem, StackPages);
            schedLock.Acquire();
            processTable[slot].state = ProcessState::Free;
            schedLock.Release();
        };

        // Allocate user stack pages and map them in the process PML4
        uint64_t userStackBase = UserStackTop - UserStackSize;
        uint64_t topStackPagePhys = 0;
        for (uint64_t i = 0; i < UserStackPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) {
                Kt::KernelLogStream(Kt::ERROR, "Sched") << "Out of memory for user stack";
                cleanupOnFail();
                return -1;
            }
            uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
            if (!Memory::VMM::Paging::MapUserIn(pml4Phys, physAddr, userStackBase + i * 0x1000)) {
                Kt::KernelLogStream(Kt::ERROR, "Sched") << "Failed to map user stack page";
                Memory::g_pfa->Free(page);
                cleanupOnFail();
                return -1;
            }
            if (i == UserStackPages - 1) topStackPagePhys = physAddr;
        }

        // Allocate and map a user-space exit stub page.
        {
            void* stubPage = Memory::g_pfa->AllocateZeroed();
            if (stubPage == nullptr) {
                Kt::KernelLogStream(Kt::ERROR, "Sched") << "Out of memory for exit stub";
                cleanupOnFail();
                return -1;
            }
            uint64_t stubPhys = Memory::SubHHDM((uint64_t)stubPage);
            if (!Memory::VMM::Paging::MapUserIn(pml4Phys, stubPhys, ExitStubAddr)) {
                Kt::KernelLogStream(Kt::ERROR, "Sched") << "Failed to map exit stub";
                Memory::g_pfa->Free(stubPage);
                cleanupOnFail();
                return -1;
            }

            // Write: xor edi, edi; xor eax, eax; syscall
            uint8_t* stub = (uint8_t*)stubPage;
            stub[0] = 0x31; stub[1] = 0xFF;  // xor edi, edi  (exit code 0)
            stub[2] = 0x31; stub[3] = 0xC0;  // xor eax, eax  (SYS_EXIT = 0)
            stub[4] = 0x0F; stub[5] = 0x05;  // syscall
        }

        // Push exit stub address as the return address on the user stack.
        {
            uint8_t* topPage = (uint8_t*)Memory::HHDM(topStackPagePhys);
            *(uint64_t*)(topPage + 0xFF8) = ExitStubAddr;
        }

        // Set up the initial kernel stack frame so that SchedContextSwitch
        // "returns" into ProcessStartup
        uint64_t* sp = (uint64_t*)kernelStackTop;

        *(--sp) = (uint64_t)ProcessStartup;  // return addr
        *(--sp) = 0;  // rbp
        *(--sp) = 0;  // rbx
        *(--sp) = 0;  // r12
        *(--sp) = 0;  // r13
        *(--sp) = 0;  // r14
        *(--sp) = 0;  // r15

        schedLock.Acquire();

        Process& proc = processTable[slot];
        proc.pid = nextPid++;
        proc.state = ProcessState::Ready;
        readyCount++;
        {
            int i = 0;
            for (; i < 63 && vfsPath[i]; i++) proc.name[i] = vfsPath[i];
            proc.name[i] = '\0';
        }
        proc.savedRsp = (uint64_t)sp;
        proc.stackBase = (uint64_t)kernelStackBase;
        proc.entryPoint = entry;
        proc.sliceRemaining = TimeSliceMs;
        proc.pml4Phys = pml4Phys;
        proc.kernelStackTop = kernelStackTop;
        proc.userStackTop = UserStackTop - 8;
        proc.heapNext = UserHeapBase;
        proc.readdirCursor = 0;
        proc.runningOnCpu = -1;
        proc.killPending = false;
        proc.waitingForPid = -1;
        proc.sleepUntilTick = 0;
        proc.waitingOnObject = nullptr;

        // Copy arguments string into process
        proc.args[0] = '\0';
        if (args != nullptr) {
            int i = 0;
            for (; i < 255 && args[i]; i++) {
                proc.args[i] = args[i];
            }
            proc.args[i] = '\0';
        }

        // Inherit user string from parent, or default to "system" if no parent
        {
            auto* cpu = Smp::GetCurrentCpuData();
            int parentSlot = cpu->currentSlot;
            if (parentSlot >= 0) {
                int i = 0;
                for (; i < 31 && processTable[parentSlot].user[i]; i++)
                    proc.user[i] = processTable[parentSlot].user[i];
                proc.user[i] = '\0';
            } else {
                // Spawned from kernel (no parent process) - set to "system"
                proc.user[0] = 's'; proc.user[1] = 'y'; proc.user[2] = 's';
                proc.user[3] = 't'; proc.user[4] = 'e'; proc.user[5] = 'm';
                proc.user[6] = '\0';
            }
        }

        {
            auto* cpu = Smp::GetCurrentCpuData();
            int parentSlot = cpu->currentSlot;
            if (parentSlot >= 0 && processTable[parentSlot].cwd[0]) {
                int i = 0;
                for (; i < 255 && processTable[parentSlot].cwd[i]; i++) {
                    proc.cwd[i] = processTable[parentSlot].cwd[i];
                }
                proc.cwd[i] = '\0';
            } else {
                proc.cwd[0] = '0';
                proc.cwd[1] = ':';
                proc.cwd[2] = '/';
                proc.cwd[3] = '\0';
            }
        }

        proc.redirected = false;
        proc.parentPid = -1;
        proc.outBuf = nullptr;
        proc.outHead = 0;
        proc.outTail = 0;
        proc.inBuf = nullptr;
        proc.inHead = 0;
        proc.inTail = 0;
        proc.keyHead = 0;
        proc.keyTail = 0;
        proc.termCols = 0;
        proc.termRows = 0;
        proc.ioOutHandle = -1;
        proc.ioInHandle = -1;
        proc.ioKeyHandle = -1;
        proc.ioWaitsetHandle = -1;

        // Initialize FPU state: zero out, then set default FCW and MXCSR
        memset(proc.fpuState, 0, 512);
        *(uint16_t*)&proc.fpuState[0] = 0x037F;   // FCW: default x87 control word
        *(uint32_t*)&proc.fpuState[24] = 0x1F80;   // MXCSR: default SSE control/status

        Ipc::ProcessStartedInSlot(slot, proc.pid);

        int resultPid = proc.pid;
        schedLock.Release();

        return resultPid;
    }

    // ====================================================================
    // Schedule -- core context switch logic
    //
    // The schedLock is held ACROSS the context switch. This is critical:
    // setting the old process to Ready and saving its RSP must be atomic
    // with respect to other CPUs. If we released the lock before saving
    // RSP, another CPU could pick up the process with a stale savedRsp.
    //
    // The RESUMED process releases the lock (it was acquired by whatever
    // CPU called Schedule() and switched away from that process).
    // New processes release it in ProcessStartup().
    // ====================================================================

    // Reclaim terminated process slots. Called from BSP's Tick only,
    // NOT from every Schedule() call on every CPU. This avoids holding
    // schedLock (with interrupts disabled) during PFA::Free on the
    // hot scheduling path.
    static void ReclaimTerminated() {
        schedLock.Acquire();
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Terminated) {
                // Grab the pointers, mark Free, then free memory after releasing lock
                void* stackBase = (processTable[i].stackBase != 0)
                    ? (void*)processTable[i].stackBase : nullptr;
                void* pml4 = (processTable[i].pml4Phys != 0)
                    ? (void*)Memory::HHDM(processTable[i].pml4Phys) : nullptr;
                processTable[i].stackBase = 0;
                processTable[i].pml4Phys = 0;
                processTable[i].state = ProcessState::Free;

                // Release lock during PFA::Free to minimize hold time
                schedLock.Release();
                if (stackBase) Memory::g_pfa->Free(stackBase, StackPages);
                if (pml4) Memory::g_pfa->Free(pml4);
                schedLock.Acquire();
            }
        }
        schedLock.Release();
    }

    void Schedule() {
        auto* cpu = Smp::GetCurrentCpuData();

        schedLock.Acquire();

        // Find the next Ready process (round-robin from after current slot)
        int next = -1;
        int start = (cpu->currentSlot >= 0) ? cpu->currentSlot + 1 : 0;

        for (int i = 0; i < MaxProcesses; i++) {
            int idx = (start + i) % MaxProcesses;
            if (processTable[idx].state == ProcessState::Ready) {
                next = idx;
                break;
            }
        }

        if (next < 0) {
            // No ready processes. If we were running one, return to idle.
            if (cpu->currentSlot >= 0) {
                int oldSlot = cpu->currentSlot;
                processTable[oldSlot].state = ProcessState::Ready;
                readyCount++;
                processTable[oldSlot].runningOnCpu = -1;
                cpu->currentSlot = -1;

                // Lock held across context switch -- the idle loop
                // doesn't release it; the next Schedule() that resumes
                // a process from idle will release it via the resumed
                // process path.  But idle is special: we release here
                // because idle doesn't go through ProcessStartup.
                // The RSP save is safe because interrupts are disabled
                // (Spinlock), so no timer can fire between Ready and save.
                SchedContextSwitch(&processTable[oldSlot].savedRsp, cpu->idleSavedRsp,
                                  GetKernelCR3(), processTable[oldSlot].fpuState, nullptr);
                // Resumed from idle -- lock was held by whoever switched to us
                schedLock.Release();
            } else {
                schedLock.Release();
            }
            return;
        }

        if (next == cpu->currentSlot) {
            // Same process, just reset time slice
            processTable[next].sliceRemaining = TimeSliceMs;
            schedLock.Release();
            return;
        }

        // Prepare the context switch
        uint64_t* oldRspPtr;
        int oldSlot = cpu->currentSlot;

        if (oldSlot >= 0) {
            processTable[oldSlot].state = ProcessState::Ready;
            readyCount++;
            processTable[oldSlot].runningOnCpu = -1;
            oldRspPtr = &processTable[oldSlot].savedRsp;
        } else {
            oldRspPtr = &cpu->idleSavedRsp;
        }

        cpu->currentSlot = next;
        processTable[next].state = ProcessState::Running;
        readyCount--;
        processTable[next].runningOnCpu = cpu->cpuIndex;
        processTable[next].sliceRemaining = TimeSliceMs;

        uint64_t newCR3 = processTable[next].pml4Phys;

        // Update per-CPU kernel RSP and TSS RSP0
        cpu->kernelRsp = processTable[next].kernelStackTop;
        cpu->tss->rsp0 = processTable[next].kernelStackTop;

        uint8_t* oldFpu = (oldSlot >= 0) ? processTable[oldSlot].fpuState : nullptr;
        uint8_t* newFpu = processTable[next].fpuState;

        // DO NOT release schedLock here! It is held across the context
        // switch so that setting Ready + saving RSP is atomic. The
        // resumed process releases it.
        SchedContextSwitch(oldRspPtr, processTable[next].savedRsp, newCR3, oldFpu, newFpu);

        // We reach here when this process is resumed by another CPU's
        // Schedule(). That CPU held the lock across its context switch.
        // Release it now.
        schedLock.Release();
    }

    void Tick() {
        auto* cpu = Smp::GetCurrentCpuData();

        // BSP: wake sleeping processes and reclaim terminated slots
        if (cpu->cpuIndex == 0) {
            schedLock.Acquire();
            uint64_t now = Timekeeping::GetTicks();
            for (int i = 0; i < MaxProcesses; i++) {
                if (processTable[i].state == ProcessState::Blocked &&
                    processTable[i].sleepUntilTick != 0 &&
                    now >= processTable[i].sleepUntilTick) {
                    processTable[i].sleepUntilTick = 0;
                    processTable[i].waitingForPid = -1;
                    processTable[i].waitingOnObject = nullptr;
                    processTable[i].state = ProcessState::Ready;
                    readyCount++;
                }
            }
            schedLock.Release();

            // Reclaim terminated process memory (BSP only, once per tick)
            ReclaimTerminated();
        }

        int slot = cpu->currentSlot;

        if (slot < 0) {
            // Idle CPU. Check the approximate ready count to avoid
            // scanning 256 process slots on every tick. On a 32-core
            // system with 27 idle CPUs, this avoids ~7M cache-line
            // reads/sec from the process table.
            if (readyCount > 0) {
                Schedule();
            }
            return;
        }

        // Check if another CPU requested this process be killed.
        // We are on the CPU running it, so ExitProcess is safe here.
        if (processTable[slot].killPending) {
            processTable[slot].killPending = false;
            ExitProcess();
            return;
        }

        if (processTable[slot].sliceRemaining > 0) {
            processTable[slot].sliceRemaining--;
        }

        if (processTable[slot].sliceRemaining == 0) {
            Schedule();
        }
    }

    int GetCurrentPid() {
        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;
        return (slot >= 0) ? processTable[slot].pid : -1;
    }

    Process* GetCurrentProcessPtr() {
        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;
        if (slot < 0) return nullptr;
        return &processTable[slot];
    }

    void ExitProcess() {
        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;

        if (slot < 0) {
            return;
        }

        Process& proc = processTable[slot];
        proc.killPending = false;
        int exitingPid = proc.pid;

        // Clean up any windows owned by this process
        WinServer::CleanupProcess(exitingPid);

        // Release process-scoped IPC handles/mappings before tearing down the address space.
        Ipc::CleanupProcessSlot(slot, exitingPid, proc.pml4Phys);

        proc.waitingForPid = -1;
        proc.sleepUntilTick = 0;
        proc.waitingOnObject = nullptr;
        proc.redirected = false;
        proc.parentPid = -1;
        proc.outBuf = nullptr;
        proc.outHead = 0;
        proc.outTail = 0;
        proc.inBuf = nullptr;
        proc.inHead = 0;
        proc.inTail = 0;
        proc.keyHead = 0;
        proc.keyTail = 0;
        proc.termCols = 0;
        proc.termRows = 0;
        proc.ioOutHandle = -1;
        proc.ioInHandle = -1;
        proc.ioKeyHandle = -1;
        proc.ioWaitsetHandle = -1;

        Ipc::ProcessExitedInSlot(slot, exitingPid);

        // Free all user-space physical pages and page table structures
        Memory::VMM::Paging::FreeUserHalf(proc.pml4Phys);

        schedLock.Acquire();

        proc.state = ProcessState::Terminated;
        proc.runningOnCpu = -1;

        // Wake any processes blocked on this PID
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Blocked &&
                processTable[i].waitingForPid == exitingPid) {
                processTable[i].state = ProcessState::Ready;
                readyCount++;
                processTable[i].waitingForPid = -1;
                processTable[i].waitingOnObject = nullptr;
                processTable[i].sleepUntilTick = 0;
            }
        }

        // Find next ready process
        int next = -1;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Ready) {
                next = i;
                break;
            }
        }

        if (next >= 0) {
            cpu->currentSlot = next;
            processTable[next].state = ProcessState::Running;
            readyCount--;
            processTable[next].runningOnCpu = cpu->cpuIndex;
            processTable[next].sliceRemaining = TimeSliceMs;

            uint64_t newCR3 = processTable[next].pml4Phys;
            cpu->kernelRsp = processTable[next].kernelStackTop;
            cpu->tss->rsp0 = processTable[next].kernelStackTop;

            // Lock held across context switch -- resumed process releases it
            SchedContextSwitch(&processTable[slot].savedRsp, processTable[next].savedRsp, newCR3,
                              processTable[slot].fpuState, processTable[next].fpuState);
            schedLock.Release();
        } else {
            cpu->currentSlot = -1;

            // Switch to idle -- release after resuming from idle
            SchedContextSwitch(&processTable[slot].savedRsp, cpu->idleSavedRsp, GetKernelCR3(),
                              processTable[slot].fpuState, nullptr);
            schedLock.Release();
        }

        for (;;) {
            asm volatile("hlt");
        }
    }

    int KillProcess(int pid) {
        // Refuse to kill PID 0 (init) or caller's own process
        if (pid == 0) return -1;
        if (pid == GetCurrentPid()) return -1;

        schedLock.Acquire();

        // Find the process by PID
        int slot = -1;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].pid == pid) {
                auto s = processTable[i].state;
                if (s == ProcessState::Ready || s == ProcessState::Running ||
                    s == ProcessState::Blocked) {
                    slot = i;
                }
                break;
            }
        }

        if (slot < 0) {
            schedLock.Release();
            return -1;
        }

        Process& proc = processTable[slot];

        if (proc.runningOnCpu >= 0) {
            // Process is currently running on another CPU. We cannot
            // safely free its resources (kernel stack, PML4, user pages)
            // because that CPU is actively using them. Set a kill-pending
            // flag; the target CPU's Tick() will call ExitProcess().
            proc.killPending = true;
            schedLock.Release();
            return 0;
        }

        // Process is Ready or Blocked (not running on any CPU).
        // Mark it Terminated so the scheduler won't pick it up.
        int killedPid = proc.pid;
        if (proc.state == ProcessState::Ready)
            readyCount--;
        proc.state = ProcessState::Terminated;
        proc.killPending = false;
        proc.waitingForPid = -1;
        proc.sleepUntilTick = 0;
        proc.waitingOnObject = nullptr;

        // Wake any processes blocked on this PID
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Blocked &&
                processTable[i].waitingForPid == killedPid) {
                processTable[i].state = ProcessState::Ready;
                readyCount++;
                processTable[i].waitingForPid = -1;
                processTable[i].waitingOnObject = nullptr;
                processTable[i].sleepUntilTick = 0;
            }
        }

        schedLock.Release();

        // Safe to clean up resources now -- process is not running anywhere.
        WinServer::CleanupProcess(killedPid);
        Ipc::CleanupProcessSlot(slot, killedPid, proc.pml4Phys);

        proc.redirected = false;
        proc.parentPid = -1;
        proc.outBuf = nullptr;
        proc.outHead = 0;
        proc.outTail = 0;
        proc.inBuf = nullptr;
        proc.inHead = 0;
        proc.inTail = 0;
        proc.keyHead = 0;
        proc.keyTail = 0;
        proc.termCols = 0;
        proc.termRows = 0;
        proc.ioOutHandle = -1;
        proc.ioInHandle = -1;
        proc.ioKeyHandle = -1;
        proc.ioWaitsetHandle = -1;

        Ipc::ProcessExitedInSlot(slot, killedPid);

        Memory::VMM::Paging::FreeUserHalf(proc.pml4Phys);

        // Kernel stack and PML4 freed by ReclaimTerminated on BSP tick.
        return 0;
    }

    void BlockOnPid(int pid) {
        // If the target is already dead, return immediately
        if (!IsAlive(pid)) return;

        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;
        if (slot < 0) return;

        schedLock.Acquire();

        // Double-check under lock (target might have exited between
        // the lockless IsAlive check and acquiring the lock)
        bool stillAlive = false;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].pid == pid) {
                auto s = processTable[i].state;
                stillAlive = (s == ProcessState::Ready ||
                              s == ProcessState::Running ||
                              s == ProcessState::Blocked);
                break;
            }
        }

        if (!stillAlive) {
            schedLock.Release();
            return;
        }

        // Mark current process as Blocked -- scheduler will skip it.
        // ExitProcess will wake us when the target terminates.
        processTable[slot].state = ProcessState::Blocked;
        processTable[slot].waitingForPid = pid;
        processTable[slot].waitingOnObject = nullptr;
        processTable[slot].sleepUntilTick = 0;
        processTable[slot].runningOnCpu = -1;
        SwitchAwayFromBlockedCurrentLocked();
    }

    void BlockForSleep(uint64_t ms) {
        if (ms == 0) return;

        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;
        if (slot < 0) return;

        schedLock.Acquire();

        processTable[slot].state = ProcessState::Blocked;
        processTable[slot].waitingForPid = -1;
        processTable[slot].waitingOnObject = nullptr;
        processTable[slot].sleepUntilTick = Timekeeping::GetTicks() + ms;
        processTable[slot].runningOnCpu = -1;
        SwitchAwayFromBlockedCurrentLocked();
    }

    void BlockOnObject(void* object, uint64_t timeoutMs) {
        if (object == nullptr) return;

        auto* cpu = Smp::GetCurrentCpuData();
        int slot = cpu->currentSlot;
        if (slot < 0) return;

        schedLock.Acquire();
        processTable[slot].state = ProcessState::Blocked;
        processTable[slot].waitingForPid = -1;
        processTable[slot].waitingOnObject = object;
        processTable[slot].sleepUntilTick = (timeoutMs > 0)
            ? (Timekeeping::GetTicks() + timeoutMs)
            : 0;
        processTable[slot].runningOnCpu = -1;
        SwitchAwayFromBlockedCurrentLocked();
    }

    void WakeObjectWaiters(void* object) {
        if (object == nullptr) return;

        schedLock.Acquire();
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state != ProcessState::Blocked) continue;
            if (processTable[i].waitingOnObject != object) continue;

            processTable[i].waitingOnObject = nullptr;
            processTable[i].sleepUntilTick = 0;
            processTable[i].waitingForPid = -1;
            processTable[i].state = ProcessState::Ready;
            readyCount++;
        }
        schedLock.Release();
    }

    bool IsAlive(int pid) {
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].pid == pid) {
                auto s = processTable[i].state;
                return s == ProcessState::Ready
                    || s == ProcessState::Running
                    || s == ProcessState::Blocked;
            }
        }
        return false;
    }

    Process* GetProcessByPid(int pid) {
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].pid == pid) {
                auto s = processTable[i].state;
                if (s == ProcessState::Ready || s == ProcessState::Running ||
                    s == ProcessState::Blocked) {
                    return &processTable[i];
                }
            }
        }
        return nullptr;
    }

    Process* GetProcessSlot(int slot) {
        if (slot < 0 || slot >= MaxProcesses) return nullptr;
        return &processTable[slot];
    }

}
