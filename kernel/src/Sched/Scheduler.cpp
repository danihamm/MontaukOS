/*
    * Scheduler.cpp
    * Preemptive process scheduler with user-mode support
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Scheduler.hpp"
#include "ElfLoader.hpp"
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Hal/Apic/Apic.hpp>
#include <Hal/GDT.hpp>

// Assembly: context switch with CR3 parameter
extern "C" void SchedContextSwitch(uint64_t* oldRsp, uint64_t newRsp, uint64_t newCR3);

// Assembly: jump to user mode via IRETQ
extern "C" void JumpToUserMode(uint64_t rip, uint64_t rsp);

// Global kernel RSP for SYSCALL entry (written by scheduler, read by SyscallEntry.asm)
extern "C" uint64_t g_kernelRsp;
uint64_t g_kernelRsp = 0;

namespace Sched {

    static Process processTable[MaxProcesses];
    static int currentPid = -1;  // -1 = idle (kernel main loop)
    static int nextPid = 0;
    static uint64_t idleSavedRsp = 0;

    // The idle loop runs in the kernel PML4
    static uint64_t GetKernelCR3() {
        return (uint64_t)Memory::VMM::g_paging->PML4;
    }

    // Startup function for newly spawned processes.
    // SchedContextSwitch "returns" here on first schedule.
    static void ProcessStartup() {
        // Send EOI for the timer IRQ that triggered the context switch
        Hal::LocalApic::SendEOI();

        if (currentPid >= 0) {
            Process& proc = processTable[currentPid];

            // Set up kernel RSP for SYSCALL entry
            g_kernelRsp = proc.kernelStackTop;

            // Set up TSS RSP0 for hardware interrupts from ring 3
            Hal::g_tss.rsp0 = proc.kernelStackTop;

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
            processTable[i].name = nullptr;
            processTable[i].savedRsp = 0;
            processTable[i].stackBase = 0;
            processTable[i].entryPoint = 0;
            processTable[i].sliceRemaining = 0;
            processTable[i].pml4Phys = 0;
            processTable[i].kernelStackTop = 0;
            processTable[i].userStackTop = 0;
            processTable[i].heapNext = 0;
            processTable[i].args[0] = '\0';
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
        }

        currentPid = -1;
        nextPid = 0;
        idleSavedRsp = 0;

        Kt::KernelLogStream(Kt::OK, "Sched") << "Initialized (" << MaxProcesses
            << " process slots, " << (uint64_t)TimeSliceMs << " ms time slice)";
    }

    int Spawn(const char* vfsPath, const char* args) {
        int slot = -1;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Free) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            Kt::KernelLogStream(Kt::ERROR, "Sched") << "No free process slots";
            return -1;
        }

        // Create per-process PML4 with kernel-half copied
        uint64_t pml4Phys = Memory::VMM::Paging::CreateUserPML4();

        // Load ELF into the process's address space
        uint64_t entry = ElfLoad(vfsPath, pml4Phys);
        if (entry == 0) {
            return -1;
        }

        // Allocate kernel stack (used during syscalls and interrupts)
        void* firstPage = Memory::g_pfa->AllocateZeroed();
        if (firstPage == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "Sched") << "Out of memory for kernel stack";
            return -1;
        }
        void* stackMem = Memory::g_pfa->ReallocConsecutive(firstPage, StackPages);
        if (stackMem == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "Sched") << "Failed to allocate contiguous kernel stack";
            Memory::g_pfa->Free(firstPage);
            return -1;
        }

        uint8_t* kernelStackBase = (uint8_t*)stackMem;
        uint64_t kernelStackTop = (uint64_t)kernelStackBase + StackSize;

        // Allocate user stack pages and map them in the process PML4
        uint64_t userStackBase = UserStackTop - UserStackSize;
        uint64_t topStackPagePhys = 0;
        for (uint64_t i = 0; i < UserStackPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) {
                Kt::KernelLogStream(Kt::ERROR, "Sched") << "Out of memory for user stack";
                return -1;
            }
            uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
            Memory::VMM::Paging::MapUserIn(pml4Phys, physAddr, userStackBase + i * 0x1000);
            if (i == UserStackPages - 1) topStackPagePhys = physAddr;
        }

        // Allocate and map a user-space exit stub page.
        // When _start() returns, it jumps here and calls SYS_EXIT(0).
        {
            void* stubPage = Memory::g_pfa->AllocateZeroed();
            if (stubPage == nullptr) {
                Kt::KernelLogStream(Kt::ERROR, "Sched") << "Out of memory for exit stub";
                return -1;
            }
            uint64_t stubPhys = Memory::SubHHDM((uint64_t)stubPage);
            Memory::VMM::Paging::MapUserIn(pml4Phys, stubPhys, ExitStubAddr);

            // Write: xor edi, edi; xor eax, eax; syscall
            uint8_t* stub = (uint8_t*)stubPage;
            stub[0] = 0x31; stub[1] = 0xFF;  // xor edi, edi  (exit code 0)
            stub[2] = 0x31; stub[3] = 0xC0;  // xor eax, eax  (SYS_EXIT = 0)
            stub[4] = 0x0F; stub[5] = 0x05;  // syscall
        }

        // Push exit stub address as the return address on the user stack.
        // UserStackTop - 8 falls at offset 0xFF8 within the top stack page.
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

        Process& proc = processTable[slot];
        proc.pid = nextPid++;
        proc.state = ProcessState::Ready;
        proc.name = vfsPath;
        proc.savedRsp = (uint64_t)sp;
        proc.stackBase = (uint64_t)kernelStackBase;
        proc.entryPoint = entry;
        proc.sliceRemaining = TimeSliceMs;
        proc.pml4Phys = pml4Phys;
        proc.kernelStackTop = kernelStackTop;
        proc.userStackTop = UserStackTop - 8;  // account for pushed exit stub return address
        proc.heapNext = UserHeapBase;

        // Copy arguments string into process
        proc.args[0] = '\0';
        if (args != nullptr) {
            int i = 0;
            for (; i < 255 && args[i]; i++) {
                proc.args[i] = args[i];
            }
            proc.args[i] = '\0';
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

        return proc.pid;
    }

    void Schedule() {
        // Reclaim terminated process slots so they can be reused
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Terminated) {
                processTable[i].state = ProcessState::Free;
            }
        }

        int next = -1;
        int start = (currentPid >= 0) ? currentPid + 1 : 0;

        for (int i = 0; i < MaxProcesses; i++) {
            int idx = (start + i) % MaxProcesses;
            if (processTable[idx].state == ProcessState::Ready) {
                next = idx;
                break;
            }
        }

        if (next < 0) {
            return;
        }

        if (next == currentPid) {
            return;
        }

        uint64_t* oldRspPtr;
        uint64_t oldCR3;

        if (currentPid >= 0) {
            processTable[currentPid].state = ProcessState::Ready;
            oldRspPtr = &processTable[currentPid].savedRsp;
        } else {
            oldRspPtr = &idleSavedRsp;
        }

        currentPid = next;
        processTable[next].state = ProcessState::Running;
        processTable[next].sliceRemaining = TimeSliceMs;

        uint64_t newCR3 = processTable[next].pml4Phys;

        // Update kernel RSP for SYSCALL entry
        g_kernelRsp = processTable[next].kernelStackTop;

        // Update TSS RSP0 for hardware interrupts from ring 3
        Hal::g_tss.rsp0 = processTable[next].kernelStackTop;

        SchedContextSwitch(oldRspPtr, processTable[next].savedRsp, newCR3);
    }

    void Tick() {
        if (currentPid < 0) {
            // Idle — check if any process became ready
            Schedule();
            return;
        }

        if (processTable[currentPid].sliceRemaining > 0) {
            processTable[currentPid].sliceRemaining--;
        }

        if (processTable[currentPid].sliceRemaining == 0) {
            Schedule();
        }
    }

    int GetCurrentPid() {
        return (currentPid >= 0) ? processTable[currentPid].pid : -1;
    }

    Process* GetCurrentProcessPtr() {
        if (currentPid < 0) return nullptr;
        return &processTable[currentPid];
    }

    void ExitProcess() {
        if (currentPid < 0) {
            return;
        }

        processTable[currentPid].state = ProcessState::Terminated;

        int next = -1;
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].state == ProcessState::Ready) {
                next = i;
                break;
            }
        }

        if (next >= 0) {
            int old = currentPid;
            currentPid = next;
            processTable[next].state = ProcessState::Running;
            processTable[next].sliceRemaining = TimeSliceMs;

            uint64_t newCR3 = processTable[next].pml4Phys;
            g_kernelRsp = processTable[next].kernelStackTop;
            Hal::g_tss.rsp0 = processTable[next].kernelStackTop;

            SchedContextSwitch(&processTable[old].savedRsp, processTable[next].savedRsp, newCR3);
        } else {
            int old = currentPid;
            currentPid = -1;
            SchedContextSwitch(&processTable[old].savedRsp, idleSavedRsp, GetKernelCR3());
        }

        for (;;) {
            asm volatile("hlt");
        }
    }

    bool IsAlive(int pid) {
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].pid == pid) {
                return processTable[i].state == ProcessState::Ready
                    || processTable[i].state == ProcessState::Running;
            }
        }
        return false;
    }

    Process* GetProcessByPid(int pid) {
        for (int i = 0; i < MaxProcesses; i++) {
            if (processTable[i].pid == pid &&
                (processTable[i].state == ProcessState::Ready ||
                 processTable[i].state == ProcessState::Running)) {
                return &processTable[i];
            }
        }
        return nullptr;
    }

}
