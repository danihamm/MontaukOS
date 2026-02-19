/*
    * Ramdisk.cpp
    * USTAR tar-based ramdisk filesystem backed by Limine modules
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Ramdisk.hpp"
#include <Terminal/Terminal.hpp>
#include <Libraries/String.hpp>
#include <Libraries/Memory.hpp>
#include <Memory/Heap.hpp>

namespace Fs::Ramdisk {

    static FileEntry fileTable[MaxFiles];
    static int fileCount = 0;

    static uint64_t OctalToUint(const char* str, int len) {
        uint64_t result = 0;
        for (int i = 0; i < len && str[i] != '\0' && str[i] != ' '; i++) {
            result = result * 8 + (str[i] - '0');
        }
        return result;
    }

    static bool StrEqual(const char* a, const char* b) {
        while (*a && *b) {
            if (*a != *b) return false;
            a++;
            b++;
        }
        return *a == *b;
    }

    static int StrLen(const char* s) {
        int n = 0;
        while (s[n]) n++;
        return n;
    }

    static bool StartsWith(const char* str, const char* prefix) {
        while (*prefix) {
            if (*str != *prefix) return false;
            str++;
            prefix++;
        }
        return true;
    }

    void Initialize(void* moduleData, uint64_t moduleSize) {
        Kt::KernelLogStream(Kt::OK, "Ramdisk") << "Parsing USTAR archive (" << moduleSize << " bytes)";

        uint8_t* ptr = (uint8_t*)moduleData;
        uint8_t* end = ptr + moduleSize;
        fileCount = 0;

        while (ptr + 512 <= end && fileCount < MaxFiles) {
            // Check for end-of-archive (two consecutive zero blocks)
            bool allZero = true;
            for (int i = 0; i < 512; i++) {
                if (ptr[i] != 0) {
                    allZero = false;
                    break;
                }
            }
            if (allZero) break;

            // Verify USTAR magic at offset 257
            const char* magic = (const char*)(ptr + 257);
            if (magic[0] != 'u' || magic[1] != 's' || magic[2] != 't' ||
                magic[3] != 'a' || magic[4] != 'r') {
                Kt::KernelLogStream(Kt::WARNING, "Ramdisk") << "Invalid USTAR magic, stopping parse";
                break;
            }

            // File name at offset 0 (100 bytes)
            const char* name = (const char*)ptr;
            // File size at offset 124 (12 bytes, octal ASCII)
            uint64_t size = OctalToUint((const char*)(ptr + 124), 12);
            // Type flag at offset 156
            char typeFlag = (char)ptr[156];

            FileEntry& entry = fileTable[fileCount];

            // Copy name
            int nameLen = 0;
            while (nameLen < MaxNameLen - 1 && name[nameLen] != '\0') {
                entry.name[nameLen] = name[nameLen];
                nameLen++;
            }
            entry.name[nameLen] = '\0';

            // Strip leading "./" if present
            if (entry.name[0] == '.' && entry.name[1] == '/') {
                char temp[MaxNameLen];
                int srcIdx = 2;
                int dstIdx = 0;
                while (entry.name[srcIdx] && dstIdx < MaxNameLen - 1) {
                    temp[dstIdx++] = entry.name[srcIdx++];
                }
                temp[dstIdx] = '\0';
                for (int i = 0; i <= dstIdx; i++) {
                    entry.name[i] = temp[i];
                }
            }

            entry.isDirectory = (typeFlag == '5');
            entry.size = size;
            entry.capacity = size;
            entry.heapAllocated = false;

            // Data starts at next 512-byte block
            entry.data = ptr + 512;

            // Skip entries that are just the root "." or empty name
            if (entry.name[0] == '\0' || (entry.name[0] == '.' && entry.name[1] == '\0')) {
                // Advance past header + data blocks
                uint64_t dataBlocks = (size + 511) / 512;
                ptr += 512 + dataBlocks * 512;
                continue;
            }

            fileCount++;

            // Advance past header + data (rounded up to 512-byte blocks)
            uint64_t dataBlocks = (size + 511) / 512;
            ptr += 512 + dataBlocks * 512;
        }

        Kt::KernelLogStream(Kt::OK, "Ramdisk") << "Loaded " << fileCount << " entries";
    }

    int Open(const char* path) {
        // Normalize: skip leading '/'
        if (path[0] == '/') path++;

        for (int i = 0; i < fileCount; i++) {
            if (StrEqual(fileTable[i].name, path)) {
                return i;
            }
            // Also try matching with trailing slash stripped from table entry
            int entryLen = StrLen(fileTable[i].name);
            if (entryLen > 0 && fileTable[i].name[entryLen - 1] == '/') {
                // Compare without trailing slash
                bool match = true;
                int pathLen = StrLen(path);
                if (pathLen == entryLen - 1) {
                    for (int j = 0; j < pathLen; j++) {
                        if (path[j] != fileTable[i].name[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) return i;
                }
            }
        }
        return -1;
    }

    int Read(int handle, uint8_t* buffer, uint64_t offset, uint64_t size) {
        if (handle < 0 || handle >= fileCount) return -1;

        const FileEntry& entry = fileTable[handle];
        if (offset >= entry.size) return 0;

        uint64_t bytesToRead = size;
        if (offset + bytesToRead > entry.size) {
            bytesToRead = entry.size - offset;
        }

        memcpy(buffer, entry.data + offset, bytesToRead);
        return (int)bytesToRead;
    }

    uint64_t GetSize(int handle) {
        if (handle < 0 || handle >= fileCount) return 0;
        return fileTable[handle].size;
    }

    void Close(int handle) {
        // No-op for ramdisk: files are memory-mapped and read-only
        (void)handle;
    }

    int ReadDir(const char* path, const char** outNames, int maxEntries) {
        // Normalize path: skip leading '/'
        if (path[0] == '/') path++;

        int pathLen = StrLen(path);
        int count = 0;

        for (int i = 0; i < fileCount && count < maxEntries; i++) {
            const char* entryName = fileTable[i].name;

            if (pathLen == 0) {
                // Root directory: find entries without '/' in them (or only trailing '/')
                bool hasSlash = false;
                int entryLen = StrLen(entryName);
                for (int j = 0; j < entryLen; j++) {
                    if (entryName[j] == '/' && j < entryLen - 1) {
                        hasSlash = true;
                        break;
                    }
                }
                if (!hasSlash) {
                    outNames[count++] = entryName;
                }
            } else {
                // Subdirectory: match entries starting with "path/"
                // and that are direct children (no additional '/' beyond the prefix)
                if (!StartsWith(entryName, path)) continue;

                // Check that path prefix is followed by '/'
                char separator = entryName[pathLen];
                if (separator != '/') continue;

                // Check it's a direct child (no more '/' except trailing)
                const char* rest = entryName + pathLen + 1;
                int restLen = StrLen(rest);
                bool hasDeepSlash = false;
                for (int j = 0; j < restLen; j++) {
                    if (rest[j] == '/' && j < restLen - 1) {
                        hasDeepSlash = true;
                        break;
                    }
                }
                if (!hasDeepSlash && restLen > 0) {
                    outNames[count++] = entryName;
                }
            }
        }

        return count;
    }

    int Write(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size) {
        if (handle < 0 || handle >= fileCount) return -1;
        if (buffer == nullptr || size == 0) return 0;

        FileEntry& entry = fileTable[handle];
        if (entry.isDirectory) return -1;

        uint64_t endOffset = offset + size;

        // Copy-on-write: if data points into tar memory, copy to heap
        if (!entry.heapAllocated) {
            uint64_t newCap = entry.size;
            if (endOffset > newCap) newCap = endOffset;
            if (newCap < 256) newCap = 256;
            // Round up to next power of 2 for growth
            uint64_t rounded = 256;
            while (rounded < newCap) rounded *= 2;
            newCap = rounded;

            uint8_t* newBuf = (uint8_t*)Memory::g_heap->Request(newCap);
            if (newBuf == nullptr) return -1;

            if (entry.data && entry.size > 0) {
                memcpy(newBuf, entry.data, entry.size);
            }

            entry.data = newBuf;
            entry.capacity = newCap;
            entry.heapAllocated = true;
        }

        // Grow buffer if needed
        if (endOffset > entry.capacity) {
            uint64_t newCap = entry.capacity;
            while (newCap < endOffset) newCap *= 2;

            uint8_t* newBuf = (uint8_t*)Memory::g_heap->Request(newCap);
            if (newBuf == nullptr) return -1;

            if (entry.data && entry.size > 0) {
                memcpy(newBuf, entry.data, entry.size);
            }
            Memory::g_heap->Free(entry.data);

            entry.data = newBuf;
            entry.capacity = newCap;
        }

        memcpy(entry.data + offset, buffer, size);

        if (endOffset > entry.size) {
            entry.size = endOffset;
        }

        return (int)size;
    }

    int Create(const char* path) {
        if (path == nullptr) return -1;
        if (fileCount >= MaxFiles) return -1;

        // Normalize: skip leading '/'
        if (path[0] == '/') path++;

        // Check if file already exists
        for (int i = 0; i < fileCount; i++) {
            if (StrEqual(fileTable[i].name, path)) {
                // File exists — truncate it
                FileEntry& entry = fileTable[i];
                if (!entry.heapAllocated) {
                    uint8_t* newBuf = (uint8_t*)Memory::g_heap->Request(256);
                    if (newBuf == nullptr) return -1;
                    entry.data = newBuf;
                    entry.capacity = 256;
                    entry.heapAllocated = true;
                }
                entry.size = 0;
                entry.isDirectory = false;
                return i;
            }
        }

        // Create new file entry
        FileEntry& entry = fileTable[fileCount];

        int nameLen = 0;
        while (nameLen < MaxNameLen - 1 && path[nameLen] != '\0') {
            entry.name[nameLen] = path[nameLen];
            nameLen++;
        }
        entry.name[nameLen] = '\0';

        uint8_t* buf = (uint8_t*)Memory::g_heap->Request(256);
        if (buf == nullptr) return -1;

        entry.data = buf;
        entry.size = 0;
        entry.capacity = 256;
        entry.isDirectory = false;
        entry.heapAllocated = true;

        return fileCount++;
    }

    int GetFileCount() {
        return fileCount;
    }

}
