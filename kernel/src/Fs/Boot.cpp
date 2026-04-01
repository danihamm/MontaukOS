/*
    * Boot.cpp
    * Boot-time filesystem initialization
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Boot.hpp"
#include <Terminal/Terminal.hpp>
#include <Fs/Ramdisk.hpp>
#include <Fs/Vfs.hpp>
#include <Fs/Fat32.hpp>
#include <Fs/Ext2.hpp>
#include <Fs/FsProbe.hpp>

namespace Fs {

    namespace {

        bool StringsEqual(const char* lhs, const char* rhs) {
            if (lhs == nullptr || rhs == nullptr) return false;

            while (*lhs != '\0' && *rhs != '\0') {
                if (*lhs != *rhs) return false;
                lhs++;
                rhs++;
            }

            return *lhs == *rhs;
        }

        bool InitializeRamdiskFromModules(const volatile limine_module_response* moduleResponse) {
            if (moduleResponse == nullptr || moduleResponse->module_count == 0) {
                Kt::KernelLogStream(Kt::WARNING, "Modules") << "No modules loaded (ramdisk unavailable)";
                return false;
            }

            Kt::KernelLogStream(Kt::OK, "Modules")
                << "Found " << (uint64_t)moduleResponse->module_count << " module(s)";

            bool hasRamdisk = false;
            for (uint64_t i = 0; i < moduleResponse->module_count; i++) {
                limine_file* module = moduleResponse->modules[i];
                if (module == nullptr || !StringsEqual(module->string, "ramdisk")) {
                    continue;
                }

                Kt::KernelLogStream(Kt::OK, "Modules")
                    << "Ramdisk module at " << kcp::hex << (uint64_t)module->address
                    << kcp::dec << ", size=" << module->size;
                Ramdisk::Initialize(module->address, module->size);
                hasRamdisk = true;
            }

            return hasRamdisk;
        }

        Vfs::FsDriver g_ramdiskDriver = {
            Ramdisk::Open,
            Ramdisk::Read,
            Ramdisk::GetSize,
            Ramdisk::Close,
            Ramdisk::ReadDir,
            Ramdisk::Write,
            Ramdisk::Create,
            Ramdisk::Delete,
            Ramdisk::Mkdir,
            nullptr,
        };

    }

    void InitializeBootFilesystems(const volatile limine_module_response* moduleResponse) {
        bool hasRamdisk = InitializeRamdiskFromModules(moduleResponse);

        Vfs::Initialize();
        if (hasRamdisk) {
            Vfs::RegisterDrive(0, &g_ramdiskDriver);
        }

        // When no ramdisk, disk partitions start at drive 0 so init.elf is found there.
        Fat32::RegisterProbe();
        Ext2::RegisterProbe();
        FsProbe::MountPartitions(hasRamdisk ? 1 : 0);
    }

}
