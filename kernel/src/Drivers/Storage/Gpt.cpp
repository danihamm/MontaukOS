/*
    * Gpt.cpp
    * GUID Partition Table (GPT) parser
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Gpt.hpp"
#include "BlockDevice.hpp"
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Libraries/Memory.hpp>
#include <Memory/PageFrameAllocator.hpp>

using namespace Kt;

namespace Drivers::Storage::Gpt {

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    static PartitionInfo g_partitions[MaxPartitions] = {};
    static int g_partitionCount = 0;

    // -------------------------------------------------------------------------
    // CRC32 (ISO 3309 / UEFI spec, polynomial 0xEDB88320)
    // -------------------------------------------------------------------------

    static uint32_t Crc32(const void* data, uint32_t length) {
        const uint8_t* buf = (const uint8_t*)data;
        uint32_t crc = 0xFFFFFFFF;

        for (uint32_t i = 0; i < length; i++) {
            crc ^= buf[i];
            for (int bit = 0; bit < 8; bit++) {
                if (crc & 1)
                    crc = (crc >> 1) ^ 0xEDB88320;
                else
                    crc >>= 1;
            }
        }

        return ~crc;
    }

    // -------------------------------------------------------------------------
    // GUID helpers
    // -------------------------------------------------------------------------

    static bool GuidIsZero(const Guid& g) {
        return g.Data1 == 0 && g.Data2 == 0 && g.Data3 == 0 &&
               g.Data4[0] == 0 && g.Data4[1] == 0 && g.Data4[2] == 0 &&
               g.Data4[3] == 0 && g.Data4[4] == 0 && g.Data4[5] == 0 &&
               g.Data4[6] == 0 && g.Data4[7] == 0;
    }

    static bool GuidEquals(const Guid& a, const Guid& b) {
        return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
               a.Data4[0] == b.Data4[0] && a.Data4[1] == b.Data4[1] &&
               a.Data4[2] == b.Data4[2] && a.Data4[3] == b.Data4[3] &&
               a.Data4[4] == b.Data4[4] && a.Data4[5] == b.Data4[5] &&
               a.Data4[6] == b.Data4[6] && a.Data4[7] == b.Data4[7];
    }

    // -------------------------------------------------------------------------
    // UTF-16LE to ASCII narrowing
    // -------------------------------------------------------------------------

    static void Utf16ToAscii(const uint16_t* src, int srcLen, char* dst, int dstMax) {
        int j = 0;
        for (int i = 0; i < srcLen && j < dstMax - 1; i++) {
            uint16_t c = src[i];
            if (c == 0) break;
            dst[j++] = (c < 128) ? (char)c : '?';
        }
        dst[j] = '\0';
    }

    // -------------------------------------------------------------------------
    // Validate protective MBR
    // -------------------------------------------------------------------------

    static bool ValidateProtectiveMbr(const uint8_t* sector0) {
        const ProtectiveMbr* mbr = (const ProtectiveMbr*)sector0;

        if (mbr->Signature != 0xAA55) return false;

        // At least one partition entry should have type 0xEE (GPT protective)
        for (int i = 0; i < 4; i++) {
            if (mbr->Partitions[i].Type == 0xEE) return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // Validate GPT header
    // -------------------------------------------------------------------------

    static bool ValidateHeader(GptHeader* hdr, uint64_t expectedLba) {
        if (hdr->Signature != GPT_HEADER_SIGNATURE) {
            KernelLogStream(ERROR, "GPT") << "Invalid signature";
            return false;
        }

        if (hdr->Revision < GPT_HEADER_REVISION) {
            KernelLogStream(ERROR, "GPT") << "Unsupported revision";
            return false;
        }

        if (hdr->HeaderSize < 92 || hdr->HeaderSize > 512) {
            KernelLogStream(ERROR, "GPT") << "Invalid header size: " << (uint64_t)hdr->HeaderSize;
            return false;
        }

        if (hdr->MyLba != expectedLba) {
            KernelLogStream(ERROR, "GPT") << "MyLBA mismatch: expected "
                << expectedLba << ", got " << hdr->MyLba;
            return false;
        }

        // Verify header CRC32
        uint32_t savedCrc = hdr->HeaderCrc32;
        hdr->HeaderCrc32 = 0;
        uint32_t computed = Crc32(hdr, hdr->HeaderSize);
        hdr->HeaderCrc32 = savedCrc;

        if (computed != savedCrc) {
            KernelLogStream(ERROR, "GPT") << "Header CRC32 mismatch: expected "
                << base::hex << (uint64_t)savedCrc << ", computed " << (uint64_t)computed;
            return false;
        }

        if (hdr->SizeOfPartitionEntry < 128) {
            KernelLogStream(ERROR, "GPT") << "Partition entry size too small: "
                << (uint64_t)hdr->SizeOfPartitionEntry;
            return false;
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // Validate partition entry array CRC32
    // -------------------------------------------------------------------------

    static bool ValidatePartitionArrayCrc(const BlockDevice* dev, const GptHeader* hdr) {
        uint32_t totalBytes = hdr->NumberOfPartitionEntries * hdr->SizeOfPartitionEntry;
        uint32_t totalSectors = (totalBytes + 511) / 512;

        // Read all partition entry sectors into a temporary buffer
        // Max 128 entries * 128 bytes = 16384 bytes = 32 sectors = 4 pages
        int pagesNeeded = (totalBytes + 0xFFF) / 0x1000;
        if (pagesNeeded > 8) {
            KernelLogStream(ERROR, "GPT") << "Partition array too large";
            return false;
        }

        void* buf;
        if (pagesNeeded == 1) {
            buf = Memory::g_pfa->AllocateZeroed();
        } else {
            buf = Memory::g_pfa->ReallocConsecutive(nullptr, pagesNeeded);
            memset(buf, 0, pagesNeeded * 0x1000);
        }

        // Read in chunks of 128 sectors max
        uint8_t* dst = (uint8_t*)buf;
        uint64_t lba = hdr->PartitionEntryLba;
        uint32_t remaining = totalSectors;

        while (remaining > 0) {
            uint32_t chunk = remaining > 128 ? 128 : remaining;
            if (!dev->ReadSectors(dev->Ctx, lba, chunk, dst)) {
                KernelLogStream(ERROR, "GPT") << "Failed to read partition entries at LBA " << lba;
                Memory::g_pfa->Free(buf, pagesNeeded);
                return false;
            }
            dst += chunk * 512;
            lba += chunk;
            remaining -= chunk;
        }

        uint32_t computed = Crc32(buf, totalBytes);
        Memory::g_pfa->Free(buf, pagesNeeded);

        if (computed != hdr->PartitionEntryArrayCrc32) {
            KernelLogStream(ERROR, "GPT") << "Partition array CRC32 mismatch: expected "
                << base::hex << (uint64_t)hdr->PartitionEntryArrayCrc32
                << ", computed " << (uint64_t)computed;
            return false;
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // Parse partition entries
    // -------------------------------------------------------------------------

    static int ParsePartitions(const BlockDevice* dev, const GptHeader* hdr, int blockDevIndex) {
        uint32_t totalBytes = hdr->NumberOfPartitionEntries * hdr->SizeOfPartitionEntry;
        uint32_t totalSectors = (totalBytes + 511) / 512;

        int pagesNeeded = (totalBytes + 0xFFF) / 0x1000;
        if (pagesNeeded > 8) return 0;

        void* buf;
        if (pagesNeeded == 1) {
            buf = Memory::g_pfa->AllocateZeroed();
        } else {
            buf = Memory::g_pfa->ReallocConsecutive(nullptr, pagesNeeded);
            memset(buf, 0, pagesNeeded * 0x1000);
        }

        uint8_t* dst = (uint8_t*)buf;
        uint64_t lba = hdr->PartitionEntryLba;
        uint32_t remaining = totalSectors;

        while (remaining > 0) {
            uint32_t chunk = remaining > 128 ? 128 : remaining;
            if (!dev->ReadSectors(dev->Ctx, lba, chunk, dst)) {
                Memory::g_pfa->Free(buf, pagesNeeded);
                return 0;
            }
            dst += chunk * 512;
            lba += chunk;
            remaining -= chunk;
        }

        int found = 0;

        for (uint32_t i = 0; i < hdr->NumberOfPartitionEntries && g_partitionCount < MaxPartitions; i++) {
            const GptPartitionEntry* entry = (const GptPartitionEntry*)
                ((uint8_t*)buf + i * hdr->SizeOfPartitionEntry);

            if (GuidIsZero(entry->TypeGuid)) continue;
            if (entry->StartingLba == 0 || entry->EndingLba == 0) continue;
            if (entry->StartingLba > entry->EndingLba) continue;

            PartitionInfo& part = g_partitions[g_partitionCount];
            part.BlockDevIndex = blockDevIndex;
            part.StartLba = entry->StartingLba;
            part.EndLba = entry->EndingLba;
            part.SectorCount = entry->EndingLba - entry->StartingLba + 1;
            part.TypeGuid = entry->TypeGuid;
            part.UniqueGuid = entry->UniqueGuid;
            part.Attributes = entry->Attributes;
            uint16_t nameCopy[36];
            memcpy(nameCopy, entry->Name, sizeof(nameCopy));
            Utf16ToAscii(nameCopy, 36, part.Name, 72);

            g_partitionCount++;
            found++;
        }

        Memory::g_pfa->Free(buf, pagesNeeded);
        return found;
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    const char* GetTypeName(const Guid& typeGuid) {
        if (GuidEquals(typeGuid, GUID_EFI_SYSTEM)) return "EFI System";
        if (GuidEquals(typeGuid, GUID_BASIC_DATA)) return "Basic Data";
        if (GuidEquals(typeGuid, GUID_LINUX_FS))   return "Linux Filesystem";
        if (GuidEquals(typeGuid, GUID_LINUX_SWAP)) return "Linux Swap";
        return "Unknown";
    }

    int ProbeDevice(int blockDevIndex) {
        const BlockDevice* dev = GetBlockDevice(blockDevIndex);
        if (!dev) return 0;

        // Need at least 34 sectors (MBR + GPT header + 32 sectors of entries)
        if (dev->SectorCount < 34) return 0;

        // Read LBA 0 (protective MBR) and LBA 1 (GPT header) — 2 sectors
        uint8_t sectorBuf[1024];

        if (!dev->ReadSectors(dev->Ctx, 0, 2, sectorBuf)) {
            return 0;
        }

        // Validate protective MBR
        if (!ValidateProtectiveMbr(sectorBuf)) {
            return 0;
        }

        // Validate primary GPT header (LBA 1)
        GptHeader* hdr = (GptHeader*)(sectorBuf + 512);

        if (!ValidateHeader(hdr, 1)) {
            KernelLogStream(WARNING, "GPT") << "Primary header invalid on device " << blockDevIndex
                << ", trying backup...";

            // Try backup header at last LBA
            uint64_t lastLba = dev->SectorCount - 1;
            if (!dev->ReadSectors(dev->Ctx, lastLba, 1, sectorBuf + 512)) {
                KernelLogStream(ERROR, "GPT") << "Failed to read backup header";
                return 0;
            }

            if (!ValidateHeader(hdr, lastLba)) {
                KernelLogStream(ERROR, "GPT") << "Backup header also invalid";
                return 0;
            }

            KernelLogStream(OK, "GPT") << "Using backup GPT header";
        }

        // Validate partition entry array CRC
        if (!ValidatePartitionArrayCrc(dev, hdr)) {
            return 0;
        }

        KernelLogStream(OK, "GPT") << "Valid GPT on device " << blockDevIndex
            << " (" << dev->Model << "): "
            << (uint64_t)hdr->NumberOfPartitionEntries << " entry slots, "
            << (uint64_t)hdr->SizeOfPartitionEntry << " bytes each";

        // Parse partition entries
        int found = ParsePartitions(dev, hdr, blockDevIndex);

        // Log discovered partitions
        for (int i = g_partitionCount - found; i < g_partitionCount; i++) {
            const PartitionInfo& p = g_partitions[i];
            uint64_t sizeMB = (p.SectorCount * dev->SectorSize) / (1024 * 1024);
            uint64_t sizeGB = sizeMB / 1024;

            if (sizeGB > 0) {
                KernelLogStream(OK, "GPT") << "  Partition " << (i - (g_partitionCount - found))
                    << ": " << (p.Name[0] ? p.Name : "(unnamed)")
                    << " [" << GetTypeName(p.TypeGuid) << "] "
                    << sizeGB << " GiB"
                    << " (LBA " << p.StartLba << "-" << p.EndLba << ")";
            } else {
                KernelLogStream(OK, "GPT") << "  Partition " << (i - (g_partitionCount - found))
                    << ": " << (p.Name[0] ? p.Name : "(unnamed)")
                    << " [" << GetTypeName(p.TypeGuid) << "] "
                    << sizeMB << " MiB"
                    << " (LBA " << p.StartLba << "-" << p.EndLba << ")";
            }
        }

        return found;
    }

    void ProbeAll() {
        int devCount = GetBlockDeviceCount();
        if (devCount == 0) return;

        KernelLogStream(INFO, "GPT") << "Probing " << devCount << " block device(s) for GPT...";

        int totalPartitions = 0;
        for (int i = 0; i < devCount; i++) {
            totalPartitions += ProbeDevice(i);
        }

        if (totalPartitions > 0) {
            KernelLogStream(OK, "GPT") << "Found " << totalPartitions << " partition(s) total";
        } else {
            KernelLogStream(INFO, "GPT") << "No GPT partitions found";
        }
    }

    int GetPartitionCount() {
        return g_partitionCount;
    }

    const PartitionInfo* GetPartition(int index) {
        if (index < 0 || index >= g_partitionCount) return nullptr;
        return &g_partitions[index];
    }

    // -------------------------------------------------------------------------
    // ASCII to UTF-16LE for partition names
    // -------------------------------------------------------------------------

    static void AsciiToUtf16(const char* src, uint16_t* dst, int maxChars) {
        int i = 0;
        for (; i < maxChars - 1 && src[i]; i++) {
            dst[i] = (uint16_t)(uint8_t)src[i];
        }
        for (; i < maxChars; i++) {
            dst[i] = 0;
        }
    }

    // -------------------------------------------------------------------------
    // Simple GUID generation from RDTSC + mixing
    // -------------------------------------------------------------------------

    static uint64_t SimpleRand64() {
        uint32_t lo, hi;
        asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t val = ((uint64_t)hi << 32) | lo;
        // xorshift64
        val ^= val << 13;
        val ^= val >> 7;
        val ^= val << 17;
        return val;
    }

    static Guid GenerateGuid() {
        uint64_t a = SimpleRand64();
        uint64_t b = SimpleRand64();
        Guid g;
        memcpy(&g, &a, 8);
        memcpy(((uint8_t*)&g) + 8, &b, 8);
        // Set version 4 (random) and variant 1
        g.Data3 = (g.Data3 & 0x0FFF) | 0x4000;
        g.Data4[0] = (g.Data4[0] & 0x3F) | 0x80;
        return g;
    }

    // -------------------------------------------------------------------------
    // Write helpers
    // -------------------------------------------------------------------------

    static bool WriteSector(const BlockDevice* dev, uint64_t lba, const void* buf) {
        return dev->WriteSectors(dev->Ctx, lba, 1, buf);
    }

    // Rebuild and write the partition entry array + both GPT headers.
    // entries is the full 128-entry array in memory.
    static bool WriteGptStructures(const BlockDevice* dev, GptHeader* primary,
                                    uint8_t* entryArray, uint32_t entryArrayBytes) {
        uint32_t entryArraySectors = (entryArrayBytes + 511) / 512;

        // Compute partition entry array CRC
        uint32_t entryCrc = Crc32(entryArray, entryArrayBytes);
        primary->PartitionEntryArrayCrc32 = entryCrc;

        // Write primary entry array (starts at LBA 2)
        for (uint32_t s = 0; s < entryArraySectors; s++) {
            if (!dev->WriteSectors(dev->Ctx, primary->PartitionEntryLba + s, 1,
                                   entryArray + s * 512))
                return false;
        }

        // Write primary header (LBA 1)
        primary->HeaderCrc32 = 0;
        primary->HeaderCrc32 = Crc32(primary, primary->HeaderSize);

        uint8_t hdrSector[512];
        memset(hdrSector, 0, 512);
        memcpy(hdrSector, primary, primary->HeaderSize);
        if (!WriteSector(dev, 1, hdrSector)) return false;

        // Build and write backup header at last LBA
        GptHeader backup = *primary;
        backup.MyLba = primary->AlternateLba;
        backup.AlternateLba = primary->MyLba;
        // Backup partition entries are right before the backup header
        backup.PartitionEntryLba = backup.MyLba - entryArraySectors;

        // Write backup entry array
        for (uint32_t s = 0; s < entryArraySectors; s++) {
            if (!dev->WriteSectors(dev->Ctx, backup.PartitionEntryLba + s, 1,
                                   entryArray + s * 512))
                return false;
        }

        // Write backup header CRC
        backup.HeaderCrc32 = 0;
        backup.HeaderCrc32 = Crc32(&backup, backup.HeaderSize);
        memset(hdrSector, 0, 512);
        memcpy(hdrSector, &backup, backup.HeaderSize);
        if (!WriteSector(dev, backup.MyLba, hdrSector)) return false;

        return true;
    }

    // -------------------------------------------------------------------------
    // InitializeGpt
    // -------------------------------------------------------------------------

    int InitializeGpt(int blockDevIndex) {
        const BlockDevice* dev = GetBlockDevice(blockDevIndex);
        if (!dev) return -1;
        if (dev->SectorCount < 68) return -1; // minimum for GPT

        // Remove stale in-memory partitions for this device
        int dst = 0;
        for (int src = 0; src < g_partitionCount; src++) {
            if (g_partitions[src].BlockDevIndex != blockDevIndex) {
                if (dst != src) g_partitions[dst] = g_partitions[src];
                dst++;
            }
        }
        g_partitionCount = dst;

        // Write protective MBR
        ProtectiveMbr mbr;
        memset(&mbr, 0, sizeof(mbr));
        mbr.Signature = 0xAA55;
        mbr.Partitions[0].Status = 0x00;
        mbr.Partitions[0].Type = 0xEE;
        mbr.Partitions[0].LbaFirst = 1;
        uint64_t mbrSectors = dev->SectorCount - 1;
        mbr.Partitions[0].SectorCount = (mbrSectors > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)mbrSectors;

        if (!WriteSector(dev, 0, &mbr)) {
            KernelLogStream(ERROR, "GPT") << "Failed to write protective MBR";
            return -1;
        }

        // Build primary GPT header
        uint32_t numEntries = 128;
        uint32_t entrySize = 128;
        uint32_t entryArrayBytes = numEntries * entrySize;
        uint32_t entryArraySectors = (entryArrayBytes + 511) / 512; // 32 sectors

        GptHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.Signature = GPT_HEADER_SIGNATURE;
        hdr.Revision = GPT_HEADER_REVISION;
        hdr.HeaderSize = 92;
        hdr.MyLba = 1;
        hdr.AlternateLba = dev->SectorCount - 1;
        hdr.FirstUsableLba = 2 + entryArraySectors; // after primary entries
        hdr.LastUsableLba = dev->SectorCount - 2 - entryArraySectors; // before backup entries
        hdr.DiskGuid = GenerateGuid();
        hdr.PartitionEntryLba = 2;
        hdr.NumberOfPartitionEntries = numEntries;
        hdr.SizeOfPartitionEntry = entrySize;

        // Empty partition entry array
        uint8_t* entryArray = (uint8_t*)Memory::g_pfa->ReallocConsecutive(
            nullptr, (entryArrayBytes + 0xFFF) / 0x1000);
        memset(entryArray, 0, entryArrayBytes);

        bool ok = WriteGptStructures(dev, &hdr, entryArray, entryArrayBytes);
        Memory::g_pfa->Free(entryArray, (entryArrayBytes + 0xFFF) / 0x1000);

        if (!ok) {
            KernelLogStream(ERROR, "GPT") << "Failed to write GPT structures";
            return -1;
        }

        KernelLogStream(OK, "GPT") << "Initialized GPT on device " << blockDevIndex
            << " (usable LBA " << hdr.FirstUsableLba << "-" << hdr.LastUsableLba << ")";

        return 0;
    }

    // -------------------------------------------------------------------------
    // AddPartition
    // -------------------------------------------------------------------------

    int AddPartition(int blockDevIndex, uint64_t startLba, uint64_t endLba,
                     const Guid& typeGuid, const char* name) {
        const BlockDevice* dev = GetBlockDevice(blockDevIndex);
        if (!dev) return -1;

        // Read existing primary GPT header
        uint8_t hdrBuf[512];
        if (!dev->ReadSectors(dev->Ctx, 1, 1, hdrBuf)) return -1;

        GptHeader* hdr = (GptHeader*)hdrBuf;
        if (!ValidateHeader(hdr, 1)) {
            KernelLogStream(ERROR, "GPT") << "No valid GPT on device " << blockDevIndex;
            return -1;
        }

        // Read partition entry array
        uint32_t entryArrayBytes = hdr->NumberOfPartitionEntries * hdr->SizeOfPartitionEntry;
        uint32_t pages = (entryArrayBytes + 0xFFF) / 0x1000;
        uint8_t* entryArray = (uint8_t*)Memory::g_pfa->ReallocConsecutive(nullptr, pages);
        memset(entryArray, 0, pages * 0x1000);

        uint32_t entryArraySectors = (entryArrayBytes + 511) / 512;
        for (uint32_t s = 0; s < entryArraySectors; s++) {
            if (!dev->ReadSectors(dev->Ctx, hdr->PartitionEntryLba + s, 1, entryArray + s * 512)) {
                Memory::g_pfa->Free(entryArray, pages);
                return -1;
            }
        }

        // Find a free entry slot
        int freeSlot = -1;
        for (uint32_t i = 0; i < hdr->NumberOfPartitionEntries; i++) {
            GptPartitionEntry* e = (GptPartitionEntry*)(entryArray + i * hdr->SizeOfPartitionEntry);
            if (GuidIsZero(e->TypeGuid)) {
                freeSlot = (int)i;
                break;
            }
        }

        if (freeSlot < 0) {
            KernelLogStream(ERROR, "GPT") << "No free partition entry slots";
            Memory::g_pfa->Free(entryArray, pages);
            return -1;
        }

        // Auto-fill: find largest free region if startLba/endLba are both 0
        if (startLba == 0 && endLba == 0) {
            // Collect used ranges
            struct Range { uint64_t start; uint64_t end; };
            Range used[MaxPartitions];
            int usedCount = 0;

            for (uint32_t i = 0; i < hdr->NumberOfPartitionEntries && usedCount < MaxPartitions; i++) {
                GptPartitionEntry* e = (GptPartitionEntry*)(entryArray + i * hdr->SizeOfPartitionEntry);
                if (!GuidIsZero(e->TypeGuid)) {
                    used[usedCount].start = e->StartingLba;
                    used[usedCount].end = e->EndingLba;
                    usedCount++;
                }
            }

            // Simple bubble sort by start LBA
            for (int i = 0; i < usedCount - 1; i++) {
                for (int j = i + 1; j < usedCount; j++) {
                    if (used[j].start < used[i].start) {
                        Range tmp = used[i]; used[i] = used[j]; used[j] = tmp;
                    }
                }
            }

            // Find largest gap
            uint64_t bestStart = 0, bestEnd = 0, bestSize = 0;

            // Gap before first partition
            uint64_t gapStart = hdr->FirstUsableLba;
            uint64_t gapEnd = (usedCount > 0) ? used[0].start - 1 : hdr->LastUsableLba;
            if (gapEnd >= gapStart && gapEnd - gapStart + 1 > bestSize) {
                bestStart = gapStart; bestEnd = gapEnd;
                bestSize = gapEnd - gapStart + 1;
            }

            // Gaps between partitions
            for (int i = 0; i < usedCount - 1; i++) {
                gapStart = used[i].end + 1;
                gapEnd = used[i + 1].start - 1;
                if (gapEnd >= gapStart && gapEnd - gapStart + 1 > bestSize) {
                    bestStart = gapStart; bestEnd = gapEnd;
                    bestSize = gapEnd - gapStart + 1;
                }
            }

            // Gap after last partition
            if (usedCount > 0) {
                gapStart = used[usedCount - 1].end + 1;
                gapEnd = hdr->LastUsableLba;
                if (gapEnd >= gapStart && gapEnd - gapStart + 1 > bestSize) {
                    bestStart = gapStart; bestEnd = gapEnd;
                    bestSize = gapEnd - gapStart + 1;
                }
            }

            if (bestSize == 0) {
                KernelLogStream(ERROR, "GPT") << "No free space for new partition";
                Memory::g_pfa->Free(entryArray, pages);
                return -1;
            }

            startLba = bestStart;
            endLba = bestEnd;
        }

        // Validate range
        if (startLba < hdr->FirstUsableLba || endLba > hdr->LastUsableLba || startLba > endLba) {
            KernelLogStream(ERROR, "GPT") << "Invalid partition range";
            Memory::g_pfa->Free(entryArray, pages);
            return -1;
        }

        // Fill the entry
        GptPartitionEntry* newEntry = (GptPartitionEntry*)(entryArray + freeSlot * hdr->SizeOfPartitionEntry);
        newEntry->TypeGuid = typeGuid;
        newEntry->UniqueGuid = GenerateGuid();
        newEntry->StartingLba = startLba;
        newEntry->EndingLba = endLba;
        newEntry->Attributes = 0;
        AsciiToUtf16(name ? name : "", newEntry->Name, 36);

        // Write updated GPT structures
        bool ok = WriteGptStructures(dev, hdr, entryArray, entryArrayBytes);
        Memory::g_pfa->Free(entryArray, pages);

        if (!ok) {
            KernelLogStream(ERROR, "GPT") << "Failed to write updated GPT";
            return -1;
        }

        // Add to in-memory partition table
        if (g_partitionCount < MaxPartitions) {
            PartitionInfo& part = g_partitions[g_partitionCount];
            part.BlockDevIndex = blockDevIndex;
            part.StartLba = startLba;
            part.EndLba = endLba;
            part.SectorCount = endLba - startLba + 1;
            part.TypeGuid = typeGuid;
            part.UniqueGuid = newEntry->UniqueGuid;
            part.Attributes = 0;
            if (name) {
                int i = 0;
                for (; i < 71 && name[i]; i++) part.Name[i] = name[i];
                part.Name[i] = '\0';
            } else {
                part.Name[0] = '\0';
            }

            int idx = g_partitionCount++;

            KernelLogStream(OK, "GPT") << "Added partition " << freeSlot
                << " [" << GetTypeName(typeGuid) << "] LBA "
                << startLba << "-" << endLba;

            return idx;
        }

        return -1;
    }

};
