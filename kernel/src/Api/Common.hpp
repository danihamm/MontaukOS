#pragma once
#include <Sched/Scheduler.hpp>

namespace Montauk {
    // Find the process that owns the I/O ring buffers for a redirected process.
    // If proc owns buffers itself (spawned via spawn_redir), returns proc.
    // If proc inherited redirection (spawned via spawn from a redirected parent),
    // follows parentPid to find the buffer owner.
    static Sched::Process* GetRedirTarget(Sched::Process* proc) {
        if (!proc || !proc->redirected) return nullptr;
        if (proc->outBuf) return proc;  // owns buffers
        return Sched::GetProcessByPid(proc->parentPid);
    }

    static void RingWrite(uint8_t* buf, uint32_t& head, uint32_t /*tail*/, uint32_t size, uint8_t byte) {
        buf[head] = byte;
        head = (head + 1) % size;
    }

    static int RingRead(uint8_t* buf, uint32_t& head, uint32_t& tail, uint32_t size, uint8_t* out, int maxLen) {
        int count = 0;
        while (tail != head && count < maxLen) {
            out[count++] = buf[tail];
            tail = (tail + 1) % size;
        }
        return count;
    }
}