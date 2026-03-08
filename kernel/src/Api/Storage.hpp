/*
    * Storage.hpp
    * Storage, GPT, format, and mount syscall implementations
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Drivers/Storage/BlockDevice.hpp>
#include <Drivers/Storage/Gpt.hpp>
#include <Fs/FsProbe.hpp>
#include <Fs/Fat32.hpp>
#include <Fs/Ext2.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static void dl_strcpy_s(char* dst, const char* src, int max) {
        int i = 0;
        for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
        dst[i] = '\0';
    }

    // Fill a user buffer with partition info. Returns the number of entries written.
    static int Sys_PartList(PartInfo* buf, int maxCount) {
        if (buf == nullptr || maxCount <= 0) return 0;

        int total = Drivers::Storage::Gpt::GetPartitionCount();
        int count = total < maxCount ? total : maxCount;

        for (int i = 0; i < count; i++) {
            auto* p = Drivers::Storage::Gpt::GetPartition(i);
            if (!p) break;

            buf[i].blockDev = p->BlockDevIndex;
            buf[i]._pad0 = 0;
            buf[i].startLba = p->StartLba;
            buf[i].endLba = p->EndLba;
            buf[i].sectorCount = p->SectorCount;

            // Copy GUIDs (layout-compatible)
            __builtin_memcpy(&buf[i].typeGuid, &p->TypeGuid, sizeof(PartGuid));
            __builtin_memcpy(&buf[i].uniqueGuid, &p->UniqueGuid, sizeof(PartGuid));

            buf[i].attributes = p->Attributes;
            dl_strcpy_s(buf[i].name, p->Name, 72);
            dl_strcpy_s(buf[i].typeName,
                Drivers::Storage::Gpt::GetTypeName(p->TypeGuid), 24);
        }

        return count;
    }

    // Read sectors from a block device. Returns bytes read, or -1 on error.
    static int64_t Sys_DiskRead(int blockDev, uint64_t lba, uint32_t count, void* buffer) {
        if (buffer == nullptr || count == 0) return -1;
        if (count > 128) return -1; // same limit as AHCI

        auto* dev = Drivers::Storage::GetBlockDevice(blockDev);
        if (!dev) return -1;

        if (lba + count > dev->SectorCount) return -1;

        if (!dev->ReadSectors(dev->Ctx, lba, count, buffer)) return -1;

        return (int64_t)(count * dev->SectorSize);
    }

    // Write sectors to a block device. Returns bytes written, or -1 on error.
    static int64_t Sys_DiskWrite(int blockDev, uint64_t lba, uint32_t count, const void* buffer) {
        if (buffer == nullptr || count == 0) return -1;
        if (count > 128) return -1;

        auto* dev = Drivers::Storage::GetBlockDevice(blockDev);
        if (!dev) return -1;

        if (lba + count > dev->SectorCount) return -1;

        if (!dev->WriteSectors(dev->Ctx, lba, count, buffer)) return -1;

        return (int64_t)(count * dev->SectorSize);
    }

    // Initialize a new GPT on a block device. Returns 0 on success, -1 on error.
    static int64_t Sys_GptInit(int blockDev) {
        return (int64_t)Drivers::Storage::Gpt::InitializeGpt(blockDev);
    }

    // Add a partition to an existing GPT. Returns partition index on success, -1 on error.
    static int64_t Sys_GptAdd(const GptAddParams* params) {
        if (params == nullptr) return -1;

        Drivers::Storage::Gpt::Guid typeGuid;
        __builtin_memcpy(&typeGuid, &params->typeGuid, sizeof(typeGuid));

        return (int64_t)Drivers::Storage::Gpt::AddPartition(
            params->blockDev, params->startLba, params->endLba,
            typeGuid, params->name);
    }

    // Mount a partition as a VFS drive. Returns 0 on success, -1 on error.
    static int64_t Sys_FsMount(int partIndex, int driveNum) {
        return (int64_t)Fs::FsProbe::MountPartition(partIndex, driveNum);
    }

    // Format a partition with a filesystem. Returns 0 on success, -1 on error.
    static int64_t Sys_FsFormat(const FsFormatParams* params) {
        if (params == nullptr) return -1;

        auto* part = Drivers::Storage::Gpt::GetPartition(params->partIndex);
        if (!part) return -1;

        switch (params->fsType) {
        case FS_TYPE_FAT32:
            return (int64_t)Fs::Fat32::Format(
                part->BlockDevIndex, part->StartLba, part->SectorCount,
                params->label[0] ? params->label : nullptr);
        case FS_TYPE_EXT2:
            return (int64_t)Fs::Ext2::Format(
                part->BlockDevIndex, part->StartLba, part->SectorCount,
                params->label[0] ? params->label : nullptr);
        default:
            return -1; // unknown filesystem type
        }
    }

};
