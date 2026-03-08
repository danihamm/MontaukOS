/*
    * Fat32.cpp
    * FAT32 filesystem driver
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Fat32.hpp"
#include "FsProbe.hpp"
#include <Drivers/Storage/BlockDevice.hpp>
#include <Terminal/Terminal.hpp>
#include <Libraries/Memory.hpp>
#include <Memory/PageFrameAllocator.hpp>

using namespace Kt;

namespace Fs::Fat32 {

    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr int MaxInstances = 8;
    static constexpr int MaxFilesPerInstance = 16;
    static constexpr int MaxDirEntries = 128;
    static constexpr int MaxNameLen = 256;

    // Directory entry attributes
    static constexpr uint8_t ATTR_READ_ONLY = 0x01;
    static constexpr uint8_t ATTR_HIDDEN    = 0x02;
    static constexpr uint8_t ATTR_SYSTEM    = 0x04;
    static constexpr uint8_t ATTR_VOLUME_ID = 0x08;
    static constexpr uint8_t ATTR_DIRECTORY = 0x10;
    static constexpr uint8_t ATTR_ARCHIVE   = 0x20;
    static constexpr uint8_t ATTR_LFN       = 0x0F;

    // Cluster chain sentinels
    static constexpr uint32_t CLUSTER_FREE    = 0x00000000;
    static constexpr uint32_t CLUSTER_BAD     = 0x0FFFFFF7;
    static constexpr uint32_t CLUSTER_END_MIN = 0x0FFFFFF8;

    // =========================================================================
    // Types
    // =========================================================================

    struct Fat32File {
        bool     inUse;
        uint32_t firstCluster;
        uint32_t fileSize;
        bool     isDirectory;
        // Location of the 32-byte SFN directory entry on disk
        uint64_t sfnPartSector;   // partition-relative sector
        uint32_t sfnOffInSector;  // byte offset within that sector
        // Cached position for sequential access (avoids O(n²) chain walks)
        uint32_t cachedClusterIdx;  // cluster index in chain
        uint32_t cachedCluster;     // cluster number at that index
    };

    struct Fat32Instance {
        bool     active;
        int      blockDevIndex;
        uint64_t partStartLba;

        // BPB fields
        uint16_t bytesPerSector;
        uint8_t  sectorsPerCluster;
        uint16_t reservedSectors;
        uint8_t  numFats;
        uint32_t fatSize32;
        uint32_t rootCluster;
        uint32_t totalSectors;
        char     volumeLabel[12];

        // Computed
        uint32_t clusterSize;      // bytesPerSector * sectorsPerCluster
        uint32_t dataStartSector;  // partition-relative sector of first data cluster
        uint32_t clusterCount;

        // Cluster read buffer (page-allocated)
        uint8_t* clusterBuf;
        int      clusterBufPages;

        // In-memory FAT cache (page-allocated)
        uint32_t* fatCache;
        int       fatCachePages;
        uint32_t  fatCacheEntries;  // number of valid 4-byte entries

        // Open file handles
        Fat32File files[MaxFilesPerInstance];

        // ReadDir name cache
        char dirNames[MaxDirEntries][MaxNameLen];
        int  dirNameCount;
    };

    struct ParsedEntry {
        char     name[MaxNameLen];
        uint32_t firstCluster;
        uint32_t fileSize;
        uint8_t  attributes;
        // Location of the SFN entry on disk (for write support)
        uint64_t sfnPartSector;
        uint32_t sfnOffInSector;
    };

    // =========================================================================
    // Instance table
    // =========================================================================

    static Fat32Instance g_instances[MaxInstances] = {};
    static int g_instanceCount = 0;

    // =========================================================================
    // Low-level helpers
    // =========================================================================

    static bool ReadPartSectors(const Fat32Instance& inst, uint64_t partSector,
                                 uint32_t count, void* buf) {
        auto* dev = Drivers::Storage::GetBlockDevice(inst.blockDevIndex);
        if (!dev) return false;
        return dev->ReadSectors(dev->Ctx, inst.partStartLba + partSector, count, buf);
    }

    static uint64_t ClusterToPartSector(const Fat32Instance& inst, uint32_t cluster) {
        return (uint64_t)inst.dataStartSector +
               (uint64_t)(cluster - 2) * inst.sectorsPerCluster;
    }

    static bool ReadCluster(Fat32Instance& inst, uint32_t cluster) {
        if (cluster < 2) return false;
        return ReadPartSectors(inst, ClusterToPartSector(inst, cluster),
                               inst.sectorsPerCluster, inst.clusterBuf);
    }

    static uint32_t GetNextCluster(const Fat32Instance& inst, uint32_t cluster) {
        // Use in-memory FAT cache if available (avoids disk I/O per lookup)
        if (inst.fatCache && cluster < inst.fatCacheEntries) {
            return inst.fatCache[cluster] & 0x0FFFFFFF;
        }

        // Fallback: read from disk
        uint32_t fatOffset = cluster * 4;
        uint32_t fatSector = fatOffset / inst.bytesPerSector;
        uint32_t entryOffset = fatOffset % inst.bytesPerSector;

        uint8_t sectorBuf[512];
        if (!ReadPartSectors(inst, inst.reservedSectors + fatSector, 1, sectorBuf)) {
            return CLUSTER_END_MIN; // treat read error as end of chain
        }

        uint32_t entry;
        memcpy(&entry, sectorBuf + entryOffset, 4);
        return entry & 0x0FFFFFFF;
    }

    static bool IsEndOfChain(uint32_t cluster) {
        return cluster >= CLUSTER_END_MIN || cluster == CLUSTER_BAD || cluster < 2;
    }

    // =========================================================================
    // Write helpers
    // =========================================================================

    static bool WritePartSectors(const Fat32Instance& inst, uint64_t partSector,
                                  uint32_t count, const void* buf) {
        auto* dev = Drivers::Storage::GetBlockDevice(inst.blockDevIndex);
        if (!dev) return false;
        return dev->WriteSectors(dev->Ctx, inst.partStartLba + partSector, count, buf);
    }

    static bool WriteClusterData(Fat32Instance& inst, uint32_t cluster, const void* data) {
        if (cluster < 2) return false;
        return WritePartSectors(inst, ClusterToPartSector(inst, cluster),
                                inst.sectorsPerCluster, data);
    }

    static bool WriteFatEntry(Fat32Instance& inst, uint32_t cluster, uint32_t value) {
        uint32_t fatOffset = cluster * 4;
        uint32_t fatSector = fatOffset / inst.bytesPerSector;
        uint32_t entryOffset = fatOffset % inst.bytesPerSector;

        uint8_t sectorBuf[512];

        for (int f = 0; f < inst.numFats; f++) {
            uint64_t fatStart = inst.reservedSectors + (uint64_t)f * inst.fatSize32;

            if (!ReadPartSectors(inst, fatStart + fatSector, 1, sectorBuf)) return false;

            // Preserve upper 4 bits of the existing entry
            uint32_t existing;
            memcpy(&existing, sectorBuf + entryOffset, 4);
            uint32_t merged = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
            memcpy(sectorBuf + entryOffset, &merged, 4);

            if (!WritePartSectors(inst, fatStart + fatSector, 1, sectorBuf)) return false;
        }

        // Keep in-memory FAT cache in sync
        if (inst.fatCache && cluster < inst.fatCacheEntries) {
            uint32_t existing = inst.fatCache[cluster];
            inst.fatCache[cluster] = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
        }

        return true;
    }

    static uint32_t AllocateCluster(Fat32Instance& inst) {
        // Fast path: scan the in-memory FAT cache
        if (inst.fatCache) {
            uint32_t limit = inst.clusterCount + 2;
            if (limit > inst.fatCacheEntries) limit = inst.fatCacheEntries;
            for (uint32_t cluster = 2; cluster < limit; cluster++) {
                if ((inst.fatCache[cluster] & 0x0FFFFFFF) == CLUSTER_FREE) {
                    if (!WriteFatEntry(inst, cluster, 0x0FFFFFFF)) return 0;
                    return cluster;
                }
            }
            return 0;
        }

        // Slow path: read FAT sectors from disk
        uint8_t sectorBuf[512];
        uint32_t entriesPerSector = inst.bytesPerSector / 4;

        for (uint32_t s = 0; s < inst.fatSize32; s++) {
            if (!ReadPartSectors(inst, inst.reservedSectors + s, 1, sectorBuf)) continue;

            for (uint32_t e = 0; e < entriesPerSector; e++) {
                uint32_t cluster = s * entriesPerSector + e;
                if (cluster < 2) continue;
                if (cluster >= inst.clusterCount + 2) return 0;

                uint32_t entry;
                memcpy(&entry, sectorBuf + e * 4, 4);
                if ((entry & 0x0FFFFFFF) == CLUSTER_FREE) {
                    if (!WriteFatEntry(inst, cluster, 0x0FFFFFFF)) return 0;
                    return cluster;
                }
            }
        }

        return 0;
    }

    // Update the file size field in a file's SFN directory entry on disk
    static bool UpdateDirEntrySize(Fat32Instance& inst, const Fat32File& file) {
        uint8_t sectorBuf[512];
        if (!ReadPartSectors(inst, file.sfnPartSector, 1, sectorBuf)) return false;
        memcpy(sectorBuf + file.sfnOffInSector + 28, &file.fileSize, 4);
        return WritePartSectors(inst, file.sfnPartSector, 1, sectorBuf);
    }

    // Update both first-cluster and file-size in a file's SFN directory entry
    static bool UpdateDirEntry(Fat32Instance& inst, const Fat32File& file) {
        uint8_t sectorBuf[512];
        if (!ReadPartSectors(inst, file.sfnPartSector, 1, sectorBuf)) return false;
        uint8_t* e = sectorBuf + file.sfnOffInSector;
        uint16_t clHi = (uint16_t)(file.firstCluster >> 16);
        uint16_t clLo = (uint16_t)(file.firstCluster & 0xFFFF);
        memcpy(e + 20, &clHi, 2);
        memcpy(e + 26, &clLo, 2);
        memcpy(e + 28, &file.fileSize, 4);
        return WritePartSectors(inst, file.sfnPartSector, 1, sectorBuf);
    }

    // =========================================================================
    // Short name generation and LFN helpers for Create
    // =========================================================================

    static void GenerateShortName(const char* longName, char* shortName) {
        memset(shortName, ' ', 11);

        // Find last dot
        int lastDot = -1;
        int nameLen = 0;
        for (int i = 0; longName[i]; i++) {
            nameLen = i + 1;
            if (longName[i] == '.') lastDot = i;
        }

        // Base name (up to 8 chars, uppercase, skip invalid chars)
        int baseEnd = (lastDot >= 0) ? lastDot : nameLen;
        int j = 0;
        for (int i = 0; i < baseEnd && j < 8; i++) {
            char c = longName[i];
            if (c == ' ' || c == '.') continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            shortName[j++] = c;
        }

        // Extension (up to 3 chars, uppercase)
        if (lastDot >= 0) {
            j = 0;
            for (int i = lastDot + 1; longName[i] && j < 3; i++) {
                char c = longName[i];
                if (c >= 'a' && c <= 'z') c -= 32;
                shortName[8 + j++] = c;
            }
        }
    }

    // Check if a short name already exists in a directory
    static bool ShortNameExists(int inst, uint32_t dirCluster, const char* shortName) {
        auto& self = g_instances[inst];
        uint32_t cluster = dirCluster;

        while (!IsEndOfChain(cluster)) {
            if (!ReadCluster(self, cluster)) return false;

            int perCluster = (int)(self.clusterSize / 32);
            for (int i = 0; i < perCluster; i++) {
                uint8_t* e = self.clusterBuf + i * 32;
                if (e[0] == 0x00) return false;
                if (e[0] == 0xE5) continue;
                if (e[11] == ATTR_LFN) continue;

                if (memcmp(e, shortName, 11) == 0) return true;
            }
            cluster = GetNextCluster(self, cluster);
        }
        return false;
    }

    // Make the short name unique by appending ~N
    static void MakeShortNameUnique(int inst, uint32_t dirCluster, char* shortName) {
        if (!ShortNameExists(inst, dirCluster, shortName)) return;

        // Find base length (non-space chars in name portion)
        int baseLen = 8;
        while (baseLen > 0 && shortName[baseLen - 1] == ' ') baseLen--;

        for (int n = 1; n <= 999; n++) {
            // Build suffix like "~1", "~23"
            char suffix[8];
            suffix[0] = '~';
            int sLen = 1;
            int tmp = n;
            char digits[4];
            int dLen = 0;
            while (tmp > 0) { digits[dLen++] = '0' + (tmp % 10); tmp /= 10; }
            for (int d = dLen - 1; d >= 0; d--) suffix[sLen++] = digits[d];

            int maxBase = 8 - sLen;
            if (baseLen < maxBase) maxBase = baseLen;

            char trial[11];
            memcpy(trial, shortName, 11);
            // Overwrite end of base with suffix
            for (int i = 0; i < sLen; i++) trial[maxBase + i] = suffix[i];
            // Pad remainder with spaces
            for (int i = maxBase + sLen; i < 8; i++) trial[i] = ' ';

            if (!ShortNameExists(inst, dirCluster, trial)) {
                memcpy(shortName, trial, 11);
                return;
            }
        }
    }

    static uint8_t LfnChecksum(const char* shortName) {
        uint8_t sum = 0;
        for (int i = 0; i < 11; i++) {
            sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)shortName[i];
        }
        return sum;
    }

    // Build LFN directory entries in disk order (highest seq first).
    // Returns the number of LFN entries.
    static int BuildLfnEntries(const char* longName, uint8_t checksum,
                                uint8_t* outEntries) {
        // Convert to UTF-16
        uint16_t utf16[MaxNameLen];
        int len = 0;
        while (longName[len] && len < MaxNameLen - 1) {
            utf16[len] = (uint8_t)longName[len];
            len++;
        }
        utf16[len] = 0;

        int totalChars = len + 1; // include null terminator
        int numEntries = (totalChars + 12) / 13;

        // Pad remaining with 0xFFFF
        for (int i = totalChars; i < numEntries * 13; i++) {
            utf16[i] = 0xFFFF;
        }

        // Build in disk order: entry N (0x40|N), then N-1, ..., 1
        for (int e = 0; e < numEntries; e++) {
            int seqNum = numEntries - e; // disk order: highest first
            uint8_t* ent = outEntries + e * 32;
            memset(ent, 0, 32);

            ent[0] = (uint8_t)seqNum;
            if (e == 0) ent[0] |= 0x40; // first physical = last logical

            ent[11] = ATTR_LFN;
            ent[13] = checksum;

            int base = (seqNum - 1) * 13;
            // Chars 1-5 at offset 1
            for (int i = 0; i < 5; i++) memcpy(ent + 1 + i * 2, &utf16[base + i], 2);
            // Chars 6-11 at offset 14
            for (int i = 0; i < 6; i++) memcpy(ent + 14 + i * 2, &utf16[base + 5 + i], 2);
            // Chars 12-13 at offset 28
            for (int i = 0; i < 2; i++) memcpy(ent + 28 + i * 2, &utf16[base + 11 + i], 2);
        }

        return numEntries;
    }

    // Forward declaration (defined below in case-insensitive comparison section)
    static bool StrEqualNoCase(const char* a, const char* b);

    // Check if a short name is needed (i.e. long name differs from 8.3)
    static bool NeedsLfn(const char* longName, const char* shortName) {
        // If name contains lowercase, spaces, or is longer than 8.3, need LFN
        char reconstructed[13];
        int pos = 0;
        for (int i = 0; i < 8 && shortName[i] != ' '; i++)
            reconstructed[pos++] = shortName[i];
        if (shortName[8] != ' ') {
            reconstructed[pos++] = '.';
            for (int i = 8; i < 11 && shortName[i] != ' '; i++)
                reconstructed[pos++] = shortName[i];
        }
        reconstructed[pos] = '\0';

        // Compare case-insensitively
        if (!StrEqualNoCase(longName, reconstructed)) return true;

        // Check if long name has lowercase (short names are uppercase)
        for (int i = 0; longName[i]; i++) {
            if (longName[i] >= 'a' && longName[i] <= 'z') return true;
        }
        return false;
    }

    // =========================================================================
    // Directory entry allocation for Create
    // =========================================================================

    // Find `slotsNeeded` consecutive free (0x00 or 0xE5) 32-byte entries in a
    // directory. If there isn't room, extends the directory by allocating a new
    // cluster. Returns the partition-relative sector and byte offset of the
    // first free slot, plus the cluster it's in.
    struct DirSlotPos {
        uint64_t partSector;   // partition-relative sector of the first slot
        uint32_t offsetInSec;  // byte offset within that sector
        bool     found;
    };

    static DirSlotPos FindFreeDirSlots(int inst, uint32_t dirFirstCluster,
                                        int slotsNeeded) {
        auto& self = g_instances[inst];
        uint32_t cluster = dirFirstCluster;
        uint32_t prevCluster = 0;

        while (!IsEndOfChain(cluster)) {
            if (!ReadCluster(self, cluster)) return {0, 0, false};

            int perCluster = (int)(self.clusterSize / 32);
            int consecutive = 0;
            int startIdx = 0;

            for (int i = 0; i < perCluster; i++) {
                uint8_t* e = self.clusterBuf + i * 32;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    if (consecutive == 0) startIdx = i;
                    consecutive++;
                    if (consecutive >= slotsNeeded) {
                        // Found enough slots
                        uint64_t clusterPartSec = ClusterToPartSector(self, cluster);
                        uint32_t byteOff = startIdx * 32;
                        uint64_t sector = clusterPartSec + (byteOff / self.bytesPerSector);
                        uint32_t offInSec = byteOff % self.bytesPerSector;
                        return {sector, offInSec, true};
                    }
                } else {
                    consecutive = 0;
                }
            }

            prevCluster = cluster;
            cluster = GetNextCluster(self, cluster);
        }

        // No room — extend directory with a new cluster
        uint32_t newCluster = AllocateCluster(self);
        if (newCluster == 0) return {0, 0, false};

        // Link previous last cluster to new one
        if (prevCluster != 0) {
            WriteFatEntry(self, prevCluster, newCluster);
        }

        // Zero the new cluster
        memset(self.clusterBuf, 0, self.clusterSize);
        WriteClusterData(self, newCluster, self.clusterBuf);

        uint64_t sec = ClusterToPartSector(self, newCluster);
        return {sec, 0, true};
    }

    // Split a path into parent directory path and filename.
    // E.g. "foo/bar/baz.txt" → parentPath="foo/bar", name="baz.txt"
    // For "baz.txt" → parentPath="", name="baz.txt"
    static void SplitPath(const char* path, char* parentPath, int parentMax,
                           char* fileName, int nameMax) {
        // Skip leading slashes
        while (*path == '/') path++;

        int len = 0;
        while (path[len]) len++;

        // Find last slash
        int lastSlash = -1;
        for (int i = len - 1; i >= 0; i--) {
            if (path[i] == '/') { lastSlash = i; break; }
        }

        if (lastSlash < 0) {
            parentPath[0] = '\0';
            int j = 0;
            while (j < nameMax - 1 && path[j]) { fileName[j] = path[j]; j++; }
            fileName[j] = '\0';
        } else {
            int j = 0;
            for (int i = 0; i < lastSlash && j < parentMax - 1; i++) {
                parentPath[j++] = path[i];
            }
            parentPath[j] = '\0';

            j = 0;
            for (int i = lastSlash + 1; i < len && j < nameMax - 1; i++) {
                fileName[j++] = path[i];
            }
            fileName[j] = '\0';
        }
    }

    // =========================================================================
    // Short name parsing
    // =========================================================================

    static void ParseShortName(const uint8_t* entry, char* out) {
        int pos = 0;
        uint8_t ntRes = entry[12]; // NTRes case flags

        // Base name (8 bytes, trim trailing spaces)
        for (int i = 0; i < 8 && entry[i] != ' '; i++) {
            char c = (char)entry[i];
            if (ntRes & 0x08) { // lowercase name
                if (c >= 'A' && c <= 'Z') c += 32;
            }
            out[pos++] = c;
        }

        // Extension (3 bytes)
        if (entry[8] != ' ') {
            out[pos++] = '.';
            for (int i = 8; i < 11 && entry[i] != ' '; i++) {
                char c = (char)entry[i];
                if (ntRes & 0x10) { // lowercase extension
                    if (c >= 'A' && c <= 'Z') c += 32;
                }
                out[pos++] = c;
            }
        }

        out[pos] = '\0';
    }

    // =========================================================================
    // LFN support
    // =========================================================================

    static void ExtractLfnChars(const uint8_t* entry, uint16_t* out) {
        // Chars 1-5 at offset 1
        for (int i = 0; i < 5; i++) memcpy(&out[i], entry + 1 + i * 2, 2);
        // Chars 6-11 at offset 14
        for (int i = 0; i < 6; i++) memcpy(&out[5 + i], entry + 14 + i * 2, 2);
        // Chars 12-13 at offset 28
        for (int i = 0; i < 2; i++) memcpy(&out[11 + i], entry + 28 + i * 2, 2);
    }

    static void Utf16ToAscii(const uint16_t* src, int maxLen, char* dst) {
        int j = 0;
        for (int i = 0; i < maxLen && src[i] != 0 && j < MaxNameLen - 1; i++) {
            dst[j++] = (src[i] < 128) ? (char)src[i] : '?';
        }
        dst[j] = '\0';
    }

    // =========================================================================
    // Directory reading
    // =========================================================================

    static int ReadDirectory(int inst, uint32_t dirCluster,
                              ParsedEntry* entries, int maxEntries) {
        auto& self = g_instances[inst];
        uint16_t lfnBuf[MaxNameLen];
        bool hasLfn = false;
        int count = 0;

        uint32_t cluster = dirCluster;
        while (!IsEndOfChain(cluster) && count < maxEntries) {
            if (!ReadCluster(self, cluster)) break;

            int perCluster = (int)(self.clusterSize / 32);
            for (int i = 0; i < perCluster && count < maxEntries; i++) {
                uint8_t* e = self.clusterBuf + i * 32;

                if (e[0] == 0x00) return count; // end of directory
                if (e[0] == 0xE5) { hasLfn = false; continue; } // deleted

                uint8_t attr = e[11];

                if (attr == ATTR_LFN) {
                    uint8_t seq = e[0];
                    int seqNum = seq & 0x1F;

                    if (seq & 0x40) {
                        // Last logical (first physical) LFN fragment
                        for (int k = 0; k < MaxNameLen; k++) lfnBuf[k] = 0;
                        hasLfn = true;
                    }

                    if (hasLfn && seqNum >= 1 && seqNum <= 20) {
                        uint16_t chars[13];
                        ExtractLfnChars(e, chars);
                        int offset = (seqNum - 1) * 13;
                        for (int k = 0; k < 13 && offset + k < MaxNameLen; k++) {
                            lfnBuf[offset + k] = chars[k];
                        }
                    }
                    continue;
                }

                // Skip volume label entries
                if (attr & ATTR_VOLUME_ID) { hasLfn = false; continue; }

                ParsedEntry& pe = entries[count];

                uint16_t clusterHi, clusterLo;
                memcpy(&clusterHi, e + 20, 2);
                memcpy(&clusterLo, e + 26, 2);
                pe.firstCluster = ((uint32_t)clusterHi << 16) | (uint32_t)clusterLo;
                memcpy(&pe.fileSize, e + 28, 4);
                pe.attributes = attr;

                if (hasLfn) {
                    Utf16ToAscii(lfnBuf, MaxNameLen, pe.name);
                } else {
                    ParseShortName(e, pe.name);
                }

                hasLfn = false;

                // Skip '.' and '..' entries
                if (pe.name[0] == '.' &&
                    (pe.name[1] == '\0' || (pe.name[1] == '.' && pe.name[2] == '\0'))) {
                    continue;
                }

                count++;
            }

            cluster = GetNextCluster(self, cluster);
        }

        return count;
    }

    // =========================================================================
    // Case-insensitive string comparison
    // =========================================================================

    static char ToLower(char c) {
        return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }

    static bool StrEqualNoCase(const char* a, const char* b) {
        while (*a && *b) {
            if (ToLower(*a) != ToLower(*b)) return false;
            a++; b++;
        }
        return *a == *b;
    }

    // =========================================================================
    // Path traversal
    // =========================================================================

    // Find a single entry by name in a directory.
    // Returns true and fills 'out' on success.
    static bool FindInDirectory(int inst, uint32_t dirCluster,
                                 const char* name, ParsedEntry* out) {
        uint32_t cluster = dirCluster;
        auto& self = g_instances[inst];
        uint16_t lfnBuf[MaxNameLen];
        bool hasLfn = false;

        while (!IsEndOfChain(cluster)) {
            if (!ReadCluster(self, cluster)) return false;

            int perCluster = (int)(self.clusterSize / 32);
            for (int i = 0; i < perCluster; i++) {
                uint8_t* e = self.clusterBuf + i * 32;

                if (e[0] == 0x00) return false;
                if (e[0] == 0xE5) { hasLfn = false; continue; }

                uint8_t attr = e[11];

                if (attr == ATTR_LFN) {
                    uint8_t seq = e[0];
                    int seqNum = seq & 0x1F;
                    if (seq & 0x40) {
                        for (int k = 0; k < MaxNameLen; k++) lfnBuf[k] = 0;
                        hasLfn = true;
                    }
                    if (hasLfn && seqNum >= 1 && seqNum <= 20) {
                        uint16_t chars[13];
                        ExtractLfnChars(e, chars);
                        int offset = (seqNum - 1) * 13;
                        for (int k = 0; k < 13 && offset + k < MaxNameLen; k++) {
                            lfnBuf[offset + k] = chars[k];
                        }
                    }
                    continue;
                }

                if (attr & ATTR_VOLUME_ID) { hasLfn = false; continue; }

                char entryName[MaxNameLen];
                if (hasLfn) {
                    Utf16ToAscii(lfnBuf, MaxNameLen, entryName);
                } else {
                    ParseShortName(e, entryName);
                }
                hasLfn = false;

                if (StrEqualNoCase(entryName, name)) {
                    uint16_t clHi, clLo;
                    memcpy(&clHi, e + 20, 2);
                    memcpy(&clLo, e + 26, 2);
                    out->firstCluster = ((uint32_t)clHi << 16) | (uint32_t)clLo;
                    memcpy(&out->fileSize, e + 28, 4);
                    out->attributes = attr;
                    int j = 0;
                    while (entryName[j] && j < MaxNameLen - 1) {
                        out->name[j] = entryName[j]; j++;
                    }
                    out->name[j] = '\0';
                    // Record SFN entry disk location
                    uint32_t byteOff = i * 32;
                    uint64_t clusterPartSec = ClusterToPartSector(self, cluster);
                    out->sfnPartSector = clusterPartSec + (byteOff / self.bytesPerSector);
                    out->sfnOffInSector = byteOff % self.bytesPerSector;
                    return true;
                }
            }

            cluster = GetNextCluster(self, cluster);
        }

        return false;
    }

    // Traverse a full path from root. Returns true and fills 'out'.
    // For "/" or "" returns the root directory pseudo-entry.
    static bool TraversePath(int inst, const char* path, ParsedEntry* out) {
        auto& self = g_instances[inst];

        // Skip leading '/'
        while (*path == '/') path++;

        // Empty path = root directory
        if (*path == '\0') {
            out->firstCluster = self.rootCluster;
            out->fileSize = 0;
            out->attributes = ATTR_DIRECTORY;
            out->name[0] = '/';
            out->name[1] = '\0';
            return true;
        }

        uint32_t currentCluster = self.rootCluster;

        while (*path) {
            // Extract next path component
            char component[MaxNameLen];
            int len = 0;
            while (*path && *path != '/' && len < MaxNameLen - 1) {
                component[len++] = *path++;
            }
            component[len] = '\0';

            // Skip trailing slashes
            while (*path == '/') path++;

            ParsedEntry found;
            if (!FindInDirectory(inst, currentCluster, component, &found)) {
                return false;
            }

            if (*path == '\0') {
                // This is the final component
                *out = found;
                return true;
            }

            // Must be a directory to continue traversal
            if (!(found.attributes & ATTR_DIRECTORY)) return false;
            currentCluster = found.firstCluster;
        }

        return false;
    }

    // =========================================================================
    // FsDriver implementation functions
    // =========================================================================

    static int OpenImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;

        ParsedEntry entry;
        if (!TraversePath(inst, path, &entry)) return -1;

        // Find a free file handle
        auto& self = g_instances[inst];
        for (int i = 0; i < MaxFilesPerInstance; i++) {
            if (!self.files[i].inUse) {
                self.files[i].inUse = true;
                self.files[i].firstCluster = entry.firstCluster;
                self.files[i].fileSize = entry.fileSize;
                self.files[i].isDirectory = (entry.attributes & ATTR_DIRECTORY) != 0;
                self.files[i].sfnPartSector = entry.sfnPartSector;
                self.files[i].sfnOffInSector = entry.sfnOffInSector;
                self.files[i].cachedClusterIdx = 0;
                self.files[i].cachedCluster = entry.firstCluster;
                return i;
            }
        }

        return -1; // no free handle
    }

    static int ReadImpl(int inst, int handle, uint8_t* buffer,
                         uint64_t offset, uint64_t size) {
        if (inst < 0 || inst >= g_instanceCount) return -1;
        auto& self = g_instances[inst];
        if (handle < 0 || handle >= MaxFilesPerInstance || !self.files[handle].inUse) return -1;

        auto& file = self.files[handle];

        // Directories don't have a meaningful fileSize for reading
        if (file.isDirectory) return -1;

        if (offset >= file.fileSize) return 0;
        if (offset + size > file.fileSize) size = file.fileSize - offset;
        if (size == 0) return 0;

        uint32_t clusterSize = self.clusterSize;
        uint32_t startClusterIdx = (uint32_t)(offset / clusterSize);
        uint32_t clusterOff = (uint32_t)(offset % clusterSize);

        // Walk cluster chain to starting cluster (use cache if possible)
        uint32_t cluster;
        uint32_t startFrom = 0;
        if (file.cachedCluster >= 2 && file.cachedClusterIdx <= startClusterIdx) {
            cluster = file.cachedCluster;
            startFrom = file.cachedClusterIdx;
        } else {
            cluster = file.firstCluster;
        }
        for (uint32_t i = startFrom; i < startClusterIdx; i++) {
            cluster = GetNextCluster(self, cluster);
            if (IsEndOfChain(cluster)) return 0;
        }

        // Read data cluster by cluster
        uint64_t bytesRead = 0;
        uint32_t lastCluster = cluster;
        while (bytesRead < size && !IsEndOfChain(cluster)) {
            if (!ReadCluster(self, cluster)) break;

            uint32_t available = clusterSize - clusterOff;
            uint64_t toRead = size - bytesRead;
            if (toRead > available) toRead = available;

            memcpy(buffer + bytesRead, self.clusterBuf + clusterOff, toRead);
            bytesRead += toRead;
            clusterOff = 0; // subsequent clusters start from offset 0

            lastCluster = cluster;
            cluster = GetNextCluster(self, cluster);
        }

        // Update cache
        if (bytesRead > 0 && lastCluster >= 2) {
            uint32_t endClusterIdx = (uint32_t)((offset + bytesRead - 1) / clusterSize);
            file.cachedClusterIdx = endClusterIdx;
            file.cachedCluster = lastCluster;
        }

        return (int)bytesRead;
    }

    static uint64_t GetSizeImpl(int inst, int handle) {
        if (inst < 0 || inst >= g_instanceCount) return 0;
        auto& self = g_instances[inst];
        if (handle < 0 || handle >= MaxFilesPerInstance || !self.files[handle].inUse) return 0;
        return self.files[handle].fileSize;
    }

    static void CloseImpl(int inst, int handle) {
        if (inst < 0 || inst >= g_instanceCount) return;
        auto& self = g_instances[inst];
        if (handle < 0 || handle >= MaxFilesPerInstance) return;
        self.files[handle].inUse = false;
    }

    static int ReadDirImpl(int inst, const char* path,
                            const char** outNames, int maxEntries) {
        if (inst < 0 || inst >= g_instanceCount) return -1;
        auto& self = g_instances[inst];

        ParsedEntry dirEntry;
        if (!TraversePath(inst, path, &dirEntry)) return -1;
        if (!(dirEntry.attributes & ATTR_DIRECTORY)) return -1;

        ParsedEntry entries[MaxDirEntries];
        int limit = maxEntries < MaxDirEntries ? maxEntries : MaxDirEntries;
        int count = ReadDirectory(inst, dirEntry.firstCluster, entries, limit);

        // Copy names into persistent cache
        self.dirNameCount = count;
        for (int i = 0; i < count; i++) {
            int j = 0;
            while (entries[i].name[j] && j < MaxNameLen - 1) {
                self.dirNames[i][j] = entries[i].name[j];
                j++;
            }
            self.dirNames[i][j] = '\0';
            outNames[i] = self.dirNames[i];
        }

        return count;
    }

    static int WriteImpl(int inst, int handle, const uint8_t* buffer,
                          uint64_t offset, uint64_t size) {
        if (inst < 0 || inst >= g_instanceCount) return -1;
        auto& self = g_instances[inst];
        if (handle < 0 || handle >= MaxFilesPerInstance || !self.files[handle].inUse) return -1;

        auto& file = self.files[handle];
        if (file.isDirectory) return -1;
        if (size == 0) return 0;

        uint32_t clusterSize = self.clusterSize;

        // If file has no clusters yet, allocate the first one
        if (file.firstCluster < 2) {
            uint32_t cl = AllocateCluster(self);
            if (cl == 0) return -1;
            file.firstCluster = cl;
            // Zero the new cluster
            memset(self.clusterBuf, 0, clusterSize);
            WriteClusterData(self, cl, self.clusterBuf);
            UpdateDirEntry(self, file);
        }

        uint64_t bytesWritten = 0;

        // Walk cluster chain to the starting cluster for this offset
        uint32_t clusterIdx = (uint32_t)(offset / clusterSize);
        uint32_t clusterOff = (uint32_t)(offset % clusterSize);

        // Use cached position if we can skip ahead
        uint32_t cluster;
        uint32_t prevCluster = 0;
        uint32_t startIdx = 0;
        if (file.cachedCluster >= 2 && file.cachedClusterIdx <= clusterIdx) {
            cluster = file.cachedCluster;
            startIdx = file.cachedClusterIdx;
        } else {
            cluster = file.firstCluster;
        }

        for (uint32_t i = startIdx; i < clusterIdx; i++) {
            prevCluster = cluster;
            uint32_t next = GetNextCluster(self, cluster);
            if (IsEndOfChain(next)) {
                // Need to extend the chain
                next = AllocateCluster(self);
                if (next == 0) goto done;
                WriteFatEntry(self, cluster, next);
                memset(self.clusterBuf, 0, clusterSize);
                WriteClusterData(self, next, self.clusterBuf);
            }
            cluster = next;
        }

        // Write data cluster by cluster
        while (bytesWritten < size) {
            if (IsEndOfChain(cluster) || cluster < 2) {
                // Allocate a new cluster and link it
                uint32_t newCl = AllocateCluster(self);
                if (newCl == 0) goto done;
                if (prevCluster != 0) {
                    WriteFatEntry(self, prevCluster, newCl);
                }
                memset(self.clusterBuf, 0, clusterSize);
                WriteClusterData(self, newCl, self.clusterBuf);
                cluster = newCl;
            }

            // Read existing cluster data (for partial writes)
            if (!ReadCluster(self, cluster)) goto done;

            uint32_t available = clusterSize - clusterOff;
            uint64_t toWrite = size - bytesWritten;
            if (toWrite > available) toWrite = available;

            memcpy(self.clusterBuf + clusterOff, buffer + bytesWritten, toWrite);

            if (!WriteClusterData(self, cluster, self.clusterBuf)) goto done;

            bytesWritten += toWrite;
            clusterOff = 0;

            prevCluster = cluster;
            cluster = GetNextCluster(self, cluster);
        }

    done:
        // Update cached position for sequential access
        if (bytesWritten > 0 && prevCluster >= 2) {
            uint32_t endClusterIdx = (uint32_t)((offset + bytesWritten - 1) / clusterSize);
            file.cachedClusterIdx = endClusterIdx;
            file.cachedCluster = prevCluster;
        }

        // Update file size if we wrote past the end
        uint64_t endPos = offset + bytesWritten;
        if (endPos > file.fileSize) {
            file.fileSize = (uint32_t)endPos;
            UpdateDirEntrySize(self, file);
        }

        return (int)bytesWritten;
    }

    static int CreateImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        // Split path into parent directory and filename
        char parentPath[MaxNameLen];
        char fileName[MaxNameLen];
        SplitPath(path, parentPath, MaxNameLen, fileName, MaxNameLen);

        if (fileName[0] == '\0') return -1;

        // Traverse to parent directory
        ParsedEntry parentEntry;
        if (!TraversePath(inst, parentPath, &parentEntry)) return -1;
        if (!(parentEntry.attributes & ATTR_DIRECTORY)) return -1;

        uint32_t parentCluster = parentEntry.firstCluster;

        // If file already exists, truncate it (free its cluster chain, reset size)
        ParsedEntry existing;
        if (FindInDirectory(inst, parentCluster, fileName, &existing)) {
            if (existing.attributes & ATTR_DIRECTORY) return -1; // can't truncate a dir

            // Free the cluster chain
            uint32_t cl = existing.firstCluster;
            while (!IsEndOfChain(cl) && cl >= 2) {
                uint32_t next = GetNextCluster(self, cl);
                WriteFatEntry(self, cl, CLUSTER_FREE);
                cl = next;
            }

            // Update directory entry: zero size, zero first cluster
            uint8_t sectorBuf[512];
            if (!ReadPartSectors(self, existing.sfnPartSector, 1, sectorBuf)) return -1;
            uint8_t* e = sectorBuf + existing.sfnOffInSector;
            uint32_t zero32 = 0;
            uint16_t zero16 = 0;
            memcpy(e + 20, &zero16, 2); // cluster high
            memcpy(e + 26, &zero16, 2); // cluster low
            memcpy(e + 28, &zero32, 4); // file size
            if (!WritePartSectors(self, existing.sfnPartSector, 1, sectorBuf)) return -1;

            // Open a handle to it
            for (int i = 0; i < MaxFilesPerInstance; i++) {
                if (!self.files[i].inUse) {
                    self.files[i].inUse = true;
                    self.files[i].firstCluster = 0;
                    self.files[i].fileSize = 0;
                    self.files[i].isDirectory = false;
                    self.files[i].sfnPartSector = existing.sfnPartSector;
                    self.files[i].sfnOffInSector = existing.sfnOffInSector;
                    self.files[i].cachedClusterIdx = 0;
                    self.files[i].cachedCluster = 0;
                    return i;
                }
            }
            return -1;
        }

        // Generate 8.3 short name
        char shortName[11];
        GenerateShortName(fileName, shortName);
        MakeShortNameUnique(inst, parentCluster, shortName);

        // Build LFN entries if needed
        uint8_t lfnEntries[20 * 32]; // max 20 LFN entries
        int lfnCount = 0;
        bool needsLfn = NeedsLfn(fileName, shortName);

        if (needsLfn) {
            uint8_t checksum = LfnChecksum(shortName);
            lfnCount = BuildLfnEntries(fileName, checksum, lfnEntries);
        }

        int totalSlots = lfnCount + 1; // LFN entries + SFN entry

        // Find free directory slots
        DirSlotPos pos = FindFreeDirSlots(inst, parentCluster, totalSlots);
        if (!pos.found) return -1;

        // Write the entries into the directory sector by sector
        // Build all entries (LFN + SFN) contiguously
        uint8_t allEntries[21 * 32]; // max 21 entries total
        if (lfnCount > 0) {
            memcpy(allEntries, lfnEntries, lfnCount * 32);
        }

        // Build the SFN entry
        uint8_t* sfn = allEntries + lfnCount * 32;
        memset(sfn, 0, 32);
        memcpy(sfn, shortName, 11);
        sfn[11] = ATTR_ARCHIVE;
        // firstCluster = 0, fileSize = 0 (file starts empty)

        // Write entries to disk, handling sector boundaries
        uint64_t curSector = pos.partSector;
        uint32_t curOff = pos.offsetInSec;

        uint8_t sectorBuf[512];
        if (!ReadPartSectors(self, curSector, 1, sectorBuf)) return -1;

        int bytesTotal = totalSlots * 32;
        int written = 0;

        while (written < bytesTotal) {
            int space = (int)self.bytesPerSector - (int)curOff;
            int chunk = bytesTotal - written;
            if (chunk > space) chunk = space;

            memcpy(sectorBuf + curOff, allEntries + written, chunk);
            written += chunk;

            if (written < bytesTotal || chunk == space) {
                // Need to flush this sector and move to next
                if (!WritePartSectors(self, curSector, 1, sectorBuf)) return -1;
                if (written < bytesTotal) {
                    curSector++;
                    curOff = 0;
                    if (!ReadPartSectors(self, curSector, 1, sectorBuf)) return -1;
                }
            } else {
                // Last write didn't fill the sector
                if (!WritePartSectors(self, curSector, 1, sectorBuf)) return -1;
            }
        }

        // Compute the SFN entry's disk location
        uint64_t sfnSector = pos.partSector + (pos.offsetInSec + lfnCount * 32) / self.bytesPerSector;
        uint32_t sfnOff = (pos.offsetInSec + lfnCount * 32) % self.bytesPerSector;

        // Open a handle
        for (int i = 0; i < MaxFilesPerInstance; i++) {
            if (!self.files[i].inUse) {
                self.files[i].inUse = true;
                self.files[i].firstCluster = 0;
                self.files[i].fileSize = 0;
                self.files[i].isDirectory = false;
                self.files[i].sfnPartSector = sfnSector;
                self.files[i].sfnOffInSector = sfnOff;
                self.files[i].cachedClusterIdx = 0;
                self.files[i].cachedCluster = 0;
                return i;
            }
        }

        return -1;
    }

    static int DeleteImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        // Split path into parent directory and filename
        char parentPath[MaxNameLen];
        char fileName[MaxNameLen];
        SplitPath(path, parentPath, MaxNameLen, fileName, MaxNameLen);

        if (fileName[0] == '\0') return -1;

        // Traverse to parent directory
        ParsedEntry parentEntry;
        if (!TraversePath(inst, parentPath, &parentEntry)) return -1;
        if (!(parentEntry.attributes & ATTR_DIRECTORY)) return -1;

        uint32_t parentCluster = parentEntry.firstCluster;

        // Find the file
        ParsedEntry existing;
        if (!FindInDirectory(inst, parentCluster, fileName, &existing)) return -1;
        if (existing.attributes & ATTR_DIRECTORY) return -1; // don't delete directories

        // Free the cluster chain
        uint32_t cl = existing.firstCluster;
        while (!IsEndOfChain(cl) && cl >= 2) {
            uint32_t next = GetNextCluster(self, cl);
            WriteFatEntry(self, cl, CLUSTER_FREE);
            cl = next;
        }

        // Mark directory entries as deleted (0xE5)
        // Walk the directory again to find and mark LFN + SFN entries
        uint32_t cluster = parentCluster;
        bool inLfnRun = false;

        while (!IsEndOfChain(cluster)) {
            if (!ReadCluster(self, cluster)) return -1;

            int perCluster = (int)(self.clusterSize / 32);
            bool modified = false;

            for (int i = 0; i < perCluster; i++) {
                uint8_t* e = self.clusterBuf + i * 32;

                if (e[0] == 0x00) goto write_back;
                if (e[0] == 0xE5) { inLfnRun = false; continue; }

                uint8_t attr = e[11];

                if (attr == ATTR_LFN) {
                    uint8_t seq = e[0];
                    if (seq & 0x40) {
                        // Start of a new LFN run — check if it belongs to our file
                        // by peeking ahead to the SFN entry
                        inLfnRun = false; // will set to true if we find the match

                        // Count how many LFN entries in this run
                        int lfnCount = seq & 0x1F;

                        // Check if the SFN entry after the LFN sequence matches
                        int sfnIdx = i + lfnCount;
                        // The SFN might be in this cluster or a later one — only
                        // handle the simple same-cluster case here.
                        if (sfnIdx < perCluster) {
                            uint8_t* sfnE = self.clusterBuf + sfnIdx * 32;
                            uint64_t clusterPartSec = ClusterToPartSector(self, cluster);
                            uint32_t byteOff = sfnIdx * 32;
                            uint64_t sfnSec = clusterPartSec + (byteOff / self.bytesPerSector);
                            uint32_t sfnOff = byteOff % self.bytesPerSector;

                            if (sfnSec == existing.sfnPartSector &&
                                sfnOff == existing.sfnOffInSector) {
                                inLfnRun = true;
                                e[0] = 0xE5;
                                modified = true;
                            }
                        }
                    } else if (inLfnRun) {
                        e[0] = 0xE5;
                        modified = true;
                    }
                    continue;
                }

                if (inLfnRun || (!inLfnRun && attr != ATTR_LFN)) {
                    // Check if this is the SFN entry we're looking for
                    uint64_t clusterPartSec = ClusterToPartSector(self, cluster);
                    uint32_t byteOff = i * 32;
                    uint64_t entrySec = clusterPartSec + (byteOff / self.bytesPerSector);
                    uint32_t entryOff = byteOff % self.bytesPerSector;

                    if (entrySec == existing.sfnPartSector &&
                        entryOff == existing.sfnOffInSector) {
                        e[0] = 0xE5;
                        modified = true;
                        inLfnRun = false;
                        goto write_back;
                    }
                }

                inLfnRun = false;
            }

        write_back:
            if (modified) {
                WriteClusterData(self, cluster, self.clusterBuf);
            }

            // Check if we already marked the SFN entry
            // (the goto write_back above handles early exit)
            {
                // Re-check if the SFN was in this cluster
                uint64_t clusterPartSec = ClusterToPartSector(self, cluster);
                uint64_t clusterEndSec = clusterPartSec + self.sectorsPerCluster;
                if (existing.sfnPartSector >= clusterPartSec &&
                    existing.sfnPartSector < clusterEndSec) {
                    return 0; // done
                }
            }

            cluster = GetNextCluster(self, cluster);
        }

        return 0;
    }

    // =========================================================================
    // Mkdir — create a directory
    // =========================================================================

    static int MkdirImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        // Split path into parent directory and new dir name
        char parentPath[MaxNameLen];
        char dirName[MaxNameLen];
        SplitPath(path, parentPath, MaxNameLen, dirName, MaxNameLen);

        if (dirName[0] == '\0') return -1;

        // Traverse to parent directory
        ParsedEntry parentEntry;
        if (!TraversePath(inst, parentPath, &parentEntry)) return -1;
        if (!(parentEntry.attributes & ATTR_DIRECTORY)) return -1;

        uint32_t parentCluster = parentEntry.firstCluster;

        // If directory already exists, return success
        ParsedEntry existing;
        if (FindInDirectory(inst, parentCluster, dirName, &existing)) {
            if (existing.attributes & ATTR_DIRECTORY) return 0;
            return -1; // exists as a file
        }

        // Allocate a cluster for the new directory
        uint32_t newCluster = AllocateCluster(self);
        if (newCluster == 0) return -1;

        // Initialize the new directory cluster with . and .. entries
        memset(self.clusterBuf, 0, self.clusterSize);

        // "." entry — points to itself
        uint8_t* dot = self.clusterBuf;
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = ATTR_DIRECTORY;
        uint16_t clHi = (uint16_t)(newCluster >> 16);
        uint16_t clLo = (uint16_t)(newCluster & 0xFFFF);
        memcpy(dot + 20, &clHi, 2);
        memcpy(dot + 26, &clLo, 2);

        // ".." entry — points to parent
        uint8_t* dotdot = self.clusterBuf + 32;
        memset(dotdot, ' ', 11);
        dotdot[0] = '.'; dotdot[1] = '.';
        dotdot[11] = ATTR_DIRECTORY;
        uint32_t parentCl = (parentCluster == self.rootCluster) ? 0 : parentCluster;
        clHi = (uint16_t)(parentCl >> 16);
        clLo = (uint16_t)(parentCl & 0xFFFF);
        memcpy(dotdot + 20, &clHi, 2);
        memcpy(dotdot + 26, &clLo, 2);

        if (!WriteClusterData(self, newCluster, self.clusterBuf)) return -1;

        // Generate 8.3 short name for the directory
        char shortName[11];
        GenerateShortName(dirName, shortName);
        MakeShortNameUnique(inst, parentCluster, shortName);

        // Build LFN entries if needed
        uint8_t lfnEntries[20 * 32];
        int lfnCount = 0;
        bool needsLfn = NeedsLfn(dirName, shortName);

        if (needsLfn) {
            uint8_t checksum = LfnChecksum(shortName);
            lfnCount = BuildLfnEntries(dirName, checksum, lfnEntries);
        }

        int totalSlots = lfnCount + 1;

        // Find free directory slots in parent
        DirSlotPos pos = FindFreeDirSlots(inst, parentCluster, totalSlots);
        if (!pos.found) return -1;

        // Build all entries (LFN + SFN)
        uint8_t allEntries[21 * 32];
        if (lfnCount > 0) {
            memcpy(allEntries, lfnEntries, lfnCount * 32);
        }

        // Build the SFN entry with ATTR_DIRECTORY
        uint8_t* sfn = allEntries + lfnCount * 32;
        memset(sfn, 0, 32);
        memcpy(sfn, shortName, 11);
        sfn[11] = ATTR_DIRECTORY;
        clHi = (uint16_t)(newCluster >> 16);
        clLo = (uint16_t)(newCluster & 0xFFFF);
        memcpy(sfn + 20, &clHi, 2);
        memcpy(sfn + 26, &clLo, 2);
        // Directory size field is 0 per FAT spec

        // Write entries to disk
        uint64_t curSector = pos.partSector;
        uint32_t curOff = pos.offsetInSec;

        uint8_t sectorBuf[512];
        if (!ReadPartSectors(self, curSector, 1, sectorBuf)) return -1;

        int bytesTotal = totalSlots * 32;
        int written = 0;

        while (written < bytesTotal) {
            int space = (int)self.bytesPerSector - (int)curOff;
            int chunk = bytesTotal - written;
            if (chunk > space) chunk = space;

            memcpy(sectorBuf + curOff, allEntries + written, chunk);
            written += chunk;

            if (written < bytesTotal || chunk == space) {
                if (!WritePartSectors(self, curSector, 1, sectorBuf)) return -1;
                if (written < bytesTotal) {
                    curSector++;
                    curOff = 0;
                    if (!ReadPartSectors(self, curSector, 1, sectorBuf)) return -1;
                }
            } else {
                if (!WritePartSectors(self, curSector, 1, sectorBuf)) return -1;
            }
        }

        return 0;
    }

    // =========================================================================
    // Template thunks — generate unique function pointers per instance
    // =========================================================================

    template<int N> struct Thunks {
        static int Open(const char* p) { return OpenImpl(N, p); }
        static int Read(int h, uint8_t* b, uint64_t o, uint64_t s) { return ReadImpl(N, h, b, o, s); }
        static uint64_t GetSize(int h) { return GetSizeImpl(N, h); }
        static void Close(int h) { CloseImpl(N, h); }
        static int ReadDir(const char* p, const char** o, int m) { return ReadDirImpl(N, p, o, m); }
        static int Write(int h, const uint8_t* b, uint64_t o, uint64_t s) { return WriteImpl(N, h, b, o, s); }
        static int Create(const char* p) { return CreateImpl(N, p); }
        static int Delete(const char* p) { return DeleteImpl(N, p); }
        static int Mkdir(const char* p) { return MkdirImpl(N, p); }
    };

    template<int N>
    static Vfs::FsDriver MakeDriver() {
        return {
            Thunks<N>::Open,
            Thunks<N>::Read,
            Thunks<N>::GetSize,
            Thunks<N>::Close,
            Thunks<N>::ReadDir,
            Thunks<N>::Write,
            Thunks<N>::Create,
            Thunks<N>::Delete,
            Thunks<N>::Mkdir,
        };
    }

    static Vfs::FsDriver g_drivers[] = {
        MakeDriver<0>(), MakeDriver<1>(), MakeDriver<2>(), MakeDriver<3>(),
        MakeDriver<4>(), MakeDriver<5>(), MakeDriver<6>(), MakeDriver<7>(),
    };

    // =========================================================================
    // BPB validation and mount
    // =========================================================================

    Vfs::FsDriver* Mount(int blockDevIndex, uint64_t startLba, uint64_t sectorCount) {
        if (g_instanceCount >= MaxInstances) return nullptr;

        auto* dev = Drivers::Storage::GetBlockDevice(blockDevIndex);
        if (!dev) return nullptr;

        // Read the first sector of the partition (BPB / VBR)
        uint8_t bpb[512];
        if (!dev->ReadSectors(dev->Ctx, startLba, 1, bpb)) return nullptr;

        // Validate boot signature
        if (bpb[510] != 0x55 || bpb[511] != 0xAA) return nullptr;

        // Parse BPB fields
        uint16_t bytesPerSector;
        memcpy(&bytesPerSector, bpb + 11, 2);
        if (bytesPerSector != 512 && bytesPerSector != 1024 &&
            bytesPerSector != 2048 && bytesPerSector != 4096) return nullptr;

        uint8_t sectorsPerCluster = bpb[13];
        if (sectorsPerCluster == 0 ||
            (sectorsPerCluster & (sectorsPerCluster - 1)) != 0) return nullptr; // must be power of 2

        uint16_t reservedSectors;
        memcpy(&reservedSectors, bpb + 14, 2);
        if (reservedSectors == 0) return nullptr;

        uint8_t numFats = bpb[16];
        if (numFats == 0) return nullptr;

        // FAT32-specific checks
        uint16_t rootEntryCount;
        memcpy(&rootEntryCount, bpb + 17, 2);
        if (rootEntryCount != 0) return nullptr; // must be 0 for FAT32

        uint16_t fatSize16;
        memcpy(&fatSize16, bpb + 22, 2);
        if (fatSize16 != 0) return nullptr; // must be 0 for FAT32

        uint32_t fatSize32;
        memcpy(&fatSize32, bpb + 36, 4);
        if (fatSize32 == 0) return nullptr;

        uint32_t rootCluster;
        memcpy(&rootCluster, bpb + 44, 4);
        if (rootCluster < 2) return nullptr;

        uint16_t totalSectors16;
        memcpy(&totalSectors16, bpb + 19, 2);
        uint32_t totalSectors32;
        memcpy(&totalSectors32, bpb + 32, 4);
        uint32_t totalSectors = (totalSectors16 != 0) ? totalSectors16 : totalSectors32;
        if (totalSectors == 0) return nullptr;

        // Compute geometry
        uint32_t dataStartSector = reservedSectors + numFats * fatSize32;
        uint32_t dataSectors = totalSectors - dataStartSector;
        uint32_t clusterCount = dataSectors / sectorsPerCluster;

        // FAT32 requires >= 65525 clusters
        if (clusterCount < 65525) return nullptr;

        // Check FS type string for extra confidence (offset 82, "FAT32   ")
        // Not strictly required by spec, but good sanity check
        bool hasFat32Str = (bpb[82] == 'F' && bpb[83] == 'A' && bpb[84] == 'T' &&
                            bpb[85] == '3' && bpb[86] == '2');

        // At least one of: valid cluster count or FS type string
        if (!hasFat32Str && clusterCount < 65525) return nullptr;

        // Success — initialize instance
        int idx = g_instanceCount;
        auto& inst = g_instances[idx];

        inst.active = true;
        inst.blockDevIndex = blockDevIndex;
        inst.partStartLba = startLba;
        inst.bytesPerSector = bytesPerSector;
        inst.sectorsPerCluster = sectorsPerCluster;
        inst.reservedSectors = reservedSectors;
        inst.numFats = numFats;
        inst.fatSize32 = fatSize32;
        inst.rootCluster = rootCluster;
        inst.totalSectors = totalSectors;
        inst.clusterSize = (uint32_t)bytesPerSector * sectorsPerCluster;
        inst.dataStartSector = dataStartSector;
        inst.clusterCount = clusterCount;

        // Volume label (offset 71, 11 bytes)
        memcpy(inst.volumeLabel, bpb + 71, 11);
        inst.volumeLabel[11] = '\0';
        // Trim trailing spaces
        for (int i = 10; i >= 0; i--) {
            if (inst.volumeLabel[i] == ' ') inst.volumeLabel[i] = '\0';
            else break;
        }

        // Allocate cluster buffer
        inst.clusterBufPages = ((int)inst.clusterSize + 0xFFF) / 0x1000;
        if (inst.clusterBufPages == 1) {
            inst.clusterBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        } else {
            inst.clusterBuf = (uint8_t*)Memory::g_pfa->ReallocConsecutive(
                nullptr, inst.clusterBufPages);
        }

        // Load entire FAT into memory for fast cluster chain lookups.
        // fatSize32 is in sectors; each sector is bytesPerSector bytes.
        uint64_t fatBytes = (uint64_t)fatSize32 * bytesPerSector;
        inst.fatCachePages = (int)((fatBytes + 0xFFF) / 0x1000);
        inst.fatCacheEntries = (uint32_t)(fatBytes / 4);
        inst.fatCache = (uint32_t*)Memory::g_pfa->ReallocConsecutive(
            nullptr, inst.fatCachePages);
        if (inst.fatCache) {
            uint8_t* dst = (uint8_t*)inst.fatCache;
            uint64_t remaining = fatBytes;
            uint64_t fatPartSector = reservedSectors;
            while (remaining > 0) {
                uint32_t chunk = (remaining > 4096) ? 4096 : (uint32_t)remaining;
                uint32_t secs = (chunk + bytesPerSector - 1) / bytesPerSector;
                if (!ReadPartSectors(inst, fatPartSector, secs, dst)) {
                    // If read fails, disable cache and fall back to per-lookup reads
                    inst.fatCache = nullptr;
                    break;
                }
                dst += secs * bytesPerSector;
                fatPartSector += secs;
                remaining -= secs * bytesPerSector;
            }
        }

        // Clear file handles
        for (int i = 0; i < MaxFilesPerInstance; i++) {
            inst.files[i].inUse = false;
        }

        g_instanceCount++;

        KernelLogStream(OK, "FAT32") << "Mounted volume \""
            << inst.volumeLabel << "\" (" << clusterCount << " clusters, "
            << (uint64_t)inst.clusterSize << " bytes/cluster)";

        return &g_drivers[idx];
    }

    void RegisterProbe() {
        FsProbe::Register(Mount);
    }

    // =========================================================================
    // FAT32 Format
    // =========================================================================

    int Format(int blockDevIndex, uint64_t startLba, uint64_t sectorCount,
               const char* volumeLabel) {
        auto* dev = Drivers::Storage::GetBlockDevice(blockDevIndex);
        if (!dev) return -1;

        // FAT32 requires at least 65525 clusters. With 8 sectors/cluster (4K),
        // minimum is ~32MB. Reject anything too small.
        if (sectorCount < 2048) {
            KernelLogStream(ERROR, "FAT32") << "Partition too small to format as FAT32";
            return -1;
        }

        // Geometry parameters
        uint16_t bytesPerSector = 512;
        uint8_t  sectorsPerCluster;

        // Choose cluster size based on volume size
        uint64_t sizeMB = (sectorCount * bytesPerSector) / (1024 * 1024);
        if (sizeMB < 256)         sectorsPerCluster = 1;   // 512B clusters
        else if (sizeMB < 8192)   sectorsPerCluster = 8;   // 4K clusters
        else if (sizeMB < 16384)  sectorsPerCluster = 16;  // 8K clusters
        else if (sizeMB < 32768)  sectorsPerCluster = 32;  // 16K clusters
        else                      sectorsPerCluster = 64;  // 32K clusters

        uint16_t reservedSectors = 32;
        uint8_t  numFats = 2;

        // Compute FAT size using the standard formula:
        // fatSize = ceil((totalSectors - reservedSectors) / (spc * 128 + numFats))
        // where 128 = bytesPerSector / 4 (FAT32 entries per sector)
        uint32_t totalSectors = (uint32_t)sectorCount;
        uint32_t denom = (uint32_t)sectorsPerCluster * 128 + numFats;
        uint32_t numer = totalSectors - reservedSectors;
        uint32_t fatSize = (numer + denom - 1) / denom;

        uint32_t dataStart = reservedSectors + numFats * fatSize;
        uint32_t dataSectors = totalSectors - dataStart;
        uint32_t clusterCount = dataSectors / sectorsPerCluster;

        if (clusterCount < 65525) {
            KernelLogStream(ERROR, "FAT32") << "Cluster count " << (uint64_t)clusterCount
                << " too low for FAT32 (need >= 65525)";
            return -1;
        }

        uint32_t rootCluster = 2; // first data cluster

        // --- Build boot sector (BPB) ---
        uint8_t bpb[512];
        memset(bpb, 0, 512);

        // Jump instruction
        bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90;
        // OEM name
        memcpy(bpb + 3, "MNTK    ", 8);

        // BPB fields
        memcpy(bpb + 11, &bytesPerSector, 2);
        bpb[13] = sectorsPerCluster;
        memcpy(bpb + 14, &reservedSectors, 2);
        bpb[16] = numFats;
        // rootEntryCount = 0 (FAT32)
        // totalSectors16 = 0 (FAT32)
        bpb[21] = 0xF8; // media type: hard disk
        // fatSize16 = 0 (FAT32)

        // Sectors per track / heads (for CHS, not critical)
        uint16_t spt = 63; memcpy(bpb + 24, &spt, 2);
        uint16_t heads = 255; memcpy(bpb + 26, &heads, 2);

        // Hidden sectors (LBA of partition start)
        uint32_t hidden = (uint32_t)startLba;
        memcpy(bpb + 28, &hidden, 4);

        // Total sectors 32
        memcpy(bpb + 32, &totalSectors, 4);

        // FAT32-specific fields
        memcpy(bpb + 36, &fatSize, 4);
        // Ext flags = 0, FS version = 0
        memcpy(bpb + 44, &rootCluster, 4);
        uint16_t fsInfoSector = 1; memcpy(bpb + 48, &fsInfoSector, 2);
        uint16_t backupBootSector = 6; memcpy(bpb + 50, &backupBootSector, 2);

        // Extended boot record
        bpb[64] = 0x80;   // drive number
        bpb[66] = 0x29;   // extended boot signature
        // Volume serial (from RDTSC)
        uint32_t lo, hi;
        asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint32_t serial = lo ^ hi;
        memcpy(bpb + 67, &serial, 4);

        // Volume label (11 bytes, space-padded)
        char label[11];
        memset(label, ' ', 11);
        if (volumeLabel) {
            for (int i = 0; i < 11 && volumeLabel[i]; i++) {
                char ch = volumeLabel[i];
                if (ch >= 'a' && ch <= 'z') ch -= 32; // uppercase
                label[i] = ch;
            }
        } else {
            memcpy(label, "NO NAME    ", 11);
        }
        memcpy(bpb + 71, label, 11);

        // FS type
        memcpy(bpb + 82, "FAT32   ", 8);

        // Boot signature
        bpb[510] = 0x55;
        bpb[511] = 0xAA;

        // --- Write boot sector ---
        if (!dev->WriteSectors(dev->Ctx, startLba, 1, bpb)) {
            KernelLogStream(ERROR, "FAT32") << "Failed to write boot sector";
            return -1;
        }

        // --- Write backup boot sector at sector 6 ---
        if (!dev->WriteSectors(dev->Ctx, startLba + backupBootSector, 1, bpb)) {
            KernelLogStream(ERROR, "FAT32") << "Failed to write backup boot sector";
            return -1;
        }

        // --- Build and write FSInfo sector ---
        uint8_t fsinfo[512];
        memset(fsinfo, 0, 512);
        uint32_t fsInfoSig1 = 0x41615252; memcpy(fsinfo + 0, &fsInfoSig1, 4);
        uint32_t fsInfoSig2 = 0x61417272; memcpy(fsinfo + 484, &fsInfoSig2, 4);
        uint32_t freeCount = clusterCount - 1; // minus root dir cluster
        memcpy(fsinfo + 488, &freeCount, 4);
        uint32_t nextFree = 3; // next free cluster after root
        memcpy(fsinfo + 492, &nextFree, 4);
        uint32_t fsInfoSig3 = 0xAA550000; memcpy(fsinfo + 508, &fsInfoSig3, 4);

        if (!dev->WriteSectors(dev->Ctx, startLba + 1, 1, fsinfo)) {
            KernelLogStream(ERROR, "FAT32") << "Failed to write FSInfo";
            return -1;
        }
        // Backup FSInfo at sector 7
        if (!dev->WriteSectors(dev->Ctx, startLba + 7, 1, fsinfo)) {
            KernelLogStream(ERROR, "FAT32") << "Failed to write backup FSInfo";
            return -1;
        }

        // --- Zero out remaining reserved sectors ---
        uint8_t zeroBuf[512];
        memset(zeroBuf, 0, 512);
        for (uint16_t s = 2; s < reservedSectors; s++) {
            if (s == backupBootSector || s == 7) continue; // already written
            dev->WriteSectors(dev->Ctx, startLba + s, 1, zeroBuf);
        }

        // --- Write FAT tables ---
        // First sector of each FAT: media byte + EOC for clusters 0,1 + EOC for root dir (cluster 2)
        uint8_t fatFirstSector[512];
        memset(fatFirstSector, 0, 512);
        uint32_t fat0 = 0x0FFFFFF8; memcpy(fatFirstSector + 0, &fat0, 4);  // cluster 0: media
        uint32_t fat1 = 0x0FFFFFFF; memcpy(fatFirstSector + 4, &fat1, 4);  // cluster 1: EOC
        uint32_t fat2 = 0x0FFFFFFF; memcpy(fatFirstSector + 8, &fat2, 4);  // cluster 2: root dir EOC

        for (int f = 0; f < numFats; f++) {
            uint64_t fatStart = startLba + reservedSectors + (uint64_t)f * fatSize;

            // Write first sector with media byte + root cluster entry
            if (!dev->WriteSectors(dev->Ctx, fatStart, 1, fatFirstSector)) {
                KernelLogStream(ERROR, "FAT32") << "Failed to write FAT " << f;
                return -1;
            }

            // Zero remaining FAT sectors
            for (uint32_t s = 1; s < fatSize; s++) {
                dev->WriteSectors(dev->Ctx, fatStart + s, 1, zeroBuf);
            }
        }

        // --- Zero root directory cluster ---
        uint64_t rootSector = startLba + dataStart;
        for (uint8_t s = 0; s < sectorsPerCluster; s++) {
            dev->WriteSectors(dev->Ctx, rootSector + s, 1, zeroBuf);
        }

        KernelLogStream(OK, "FAT32") << "Formatted: " << (uint64_t)clusterCount
            << " clusters, " << (uint64_t)sectorsPerCluster << " sec/cluster, FAT="
            << (uint64_t)fatSize << " sectors";

        return 0;
    }

};
