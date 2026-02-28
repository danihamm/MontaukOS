/*
    * Info.hpp
    * SYS_GETINFO syscall
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <Sched/Scheduler.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static void Sys_GetInfo(SysInfo* outInfo) {
        if (outInfo == nullptr) return;

        // Copy strings into fixed-size arrays (user-accessible)
        const char* name = "MontaukOS";
        const char* ver = "0.1.0";
        for (int i = 0; name[i]; i++) outInfo->osName[i] = name[i];
        outInfo->osName[9] = '\0';
        for (int i = 0; ver[i]; i++) outInfo->osVersion[i] = ver[i];
        outInfo->osVersion[5] = '\0';

        outInfo->apiVersion = 2;
        outInfo->maxProcesses = Sched::MaxProcesses;
    }
};
