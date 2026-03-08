/*
    * FsProbe.cpp
    * Filesystem probe registry
    * Copyright (c) 2026 Daniel Hammer
*/

#include "FsProbe.hpp"
#include <Drivers/Storage/Gpt.hpp>
#include <Terminal/Terminal.hpp>
#include <Libraries/Memory.hpp>

namespace Fs::FsProbe {

    static ProbeFn g_probes[MaxProbes] = {};
    static int g_probeCount = 0;

    void Register(ProbeFn fn) {
        if (g_probeCount < MaxProbes && fn) {
            g_probes[g_probeCount++] = fn;
        }
    }

    static bool IsEfiPartition(const Drivers::Storage::Gpt::PartitionInfo* part) {
        auto& a = part->TypeGuid;
        auto& b = Drivers::Storage::Gpt::GUID_EFI_SYSTEM;
        return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
               memcmp(a.Data4, b.Data4, 8) == 0;
    }

    void MountPartitions(int firstDrive) {
        int partCount = Drivers::Storage::Gpt::GetPartitionCount();
        if (partCount == 0 || g_probeCount == 0) return;

        int driveNum = firstDrive;

        // First pass: mount non-EFI partitions so they get lower drive numbers
        for (int i = 0; i < partCount && driveNum < Vfs::MaxDrives; i++) {
            auto* part = Drivers::Storage::Gpt::GetPartition(i);
            if (!part) continue;
            if (IsEfiPartition(part)) continue;

            for (int p = 0; p < g_probeCount; p++) {
                Vfs::FsDriver* driver = g_probes[p](
                    part->BlockDevIndex, part->StartLba, part->SectorCount);

                if (driver) {
                    Vfs::RegisterDrive(driveNum, driver);
                    Kt::KernelLogStream(Kt::OK, "FsProbe") << "Mounted partition "
                        << i << " as drive " << driveNum;
                    driveNum++;
                    break;
                }
            }
        }

        // Second pass: mount EFI system partitions after all others
        for (int i = 0; i < partCount && driveNum < Vfs::MaxDrives; i++) {
            auto* part = Drivers::Storage::Gpt::GetPartition(i);
            if (!part) continue;
            if (!IsEfiPartition(part)) continue;

            for (int p = 0; p < g_probeCount; p++) {
                Vfs::FsDriver* driver = g_probes[p](
                    part->BlockDevIndex, part->StartLba, part->SectorCount);

                if (driver) {
                    Vfs::RegisterDrive(driveNum, driver);
                    Kt::KernelLogStream(Kt::OK, "FsProbe") << "Mounted EFI partition "
                        << i << " as drive " << driveNum;
                    driveNum++;
                    break;
                }
            }
        }
    }

    int MountPartition(int partIndex, int driveNum) {
        if (driveNum < 0 || driveNum >= Vfs::MaxDrives) return -1;
        if (g_probeCount == 0) return -1;

        auto* part = Drivers::Storage::Gpt::GetPartition(partIndex);
        if (!part) return -1;

        for (int p = 0; p < g_probeCount; p++) {
            Vfs::FsDriver* driver = g_probes[p](
                part->BlockDevIndex, part->StartLba, part->SectorCount);

            if (driver) {
                Vfs::RegisterDrive(driveNum, driver);
                Kt::KernelLogStream(Kt::OK, "FsProbe") << "Mounted partition "
                    << partIndex << " as drive " << driveNum;
                return 0;
            }
        }

        Kt::KernelLogStream(Kt::WARNING, "FsProbe") << "No filesystem recognized on partition " << partIndex;
        return -1;
    }

};
