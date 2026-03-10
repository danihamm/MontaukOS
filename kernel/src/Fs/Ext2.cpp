/*
    * Ext2.cpp
    * ext2 filesystem driver
    * Copyright (c) 2026 Daniel Hammer
*/

#include "Ext2.hpp"
#include "FsProbe.hpp"
#include <Drivers/Storage/BlockDevice.hpp>
#include <Terminal/Terminal.hpp>
#include <Libraries/Memory.hpp>
#include <Memory/PageFrameAllocator.hpp>

using namespace Kt;

namespace Fs::Ext2 {

    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr int MaxInstances = 8;
    static constexpr int MaxFilesPerInstance = 16;
    static constexpr int MaxDirEntries = 128;
    static constexpr int MaxNameLen = 256;

    static constexpr uint16_t EXT2_MAGIC = 0xEF53;

    // Inode types (from i_mode, upper 4 bits)
    static constexpr uint16_t IMODE_FIFO    = 0x1000;
    static constexpr uint16_t IMODE_CHARDEV = 0x2000;
    static constexpr uint16_t IMODE_DIR     = 0x4000;
    static constexpr uint16_t IMODE_BLKDEV  = 0x6000;
    static constexpr uint16_t IMODE_REG     = 0x8000;
    static constexpr uint16_t IMODE_SYMLINK = 0xA000;
    static constexpr uint16_t IMODE_SOCKET  = 0xC000;
    static constexpr uint16_t IMODE_TYPE_MASK = 0xF000;

    // Directory entry file types
    static constexpr uint8_t EXT2_FT_UNKNOWN  = 0;
    static constexpr uint8_t EXT2_FT_REG_FILE = 1;
    static constexpr uint8_t EXT2_FT_DIR      = 2;
    static constexpr uint8_t EXT2_FT_CHRDEV   = 3;
    static constexpr uint8_t EXT2_FT_BLKDEV   = 4;
    static constexpr uint8_t EXT2_FT_FIFO     = 5;
    static constexpr uint8_t EXT2_FT_SOCK     = 6;
    static constexpr uint8_t EXT2_FT_SYMLINK  = 7;

    // Special inode numbers
    static constexpr uint32_t EXT2_ROOT_INODE = 2;

    // =========================================================================
    // On-disk structures
    // =========================================================================

    struct Superblock {
        uint32_t s_inodes_count;
        uint32_t s_blocks_count;
        uint32_t s_r_blocks_count;
        uint32_t s_free_blocks_count;
        uint32_t s_free_inodes_count;
        uint32_t s_first_data_block;
        uint32_t s_log_block_size;
        uint32_t s_log_frag_size;
        uint32_t s_blocks_per_group;
        uint32_t s_frags_per_group;
        uint32_t s_inodes_per_group;
        uint32_t s_mtime;
        uint32_t s_wtime;
        uint16_t s_mnt_count;
        uint16_t s_max_mnt_count;
        uint16_t s_magic;
        uint16_t s_state;
        uint16_t s_errors;
        uint16_t s_minor_rev_level;
        uint32_t s_lastcheck;
        uint32_t s_checkinterval;
        uint32_t s_creator_os;
        uint32_t s_rev_level;
        uint16_t s_def_resuid;
        uint16_t s_def_resgid;
        // Rev 1 fields
        uint32_t s_first_ino;
        uint16_t s_inode_size;
        uint16_t s_block_group_nr;
        uint32_t s_feature_compat;
        uint32_t s_feature_incompat;
        uint32_t s_feature_ro_compat;
        uint8_t  s_uuid[16];
        char     s_volume_name[16];
        // ... more fields follow but are not needed
    } __attribute__((packed));

    struct BlockGroupDescriptor {
        uint32_t bg_block_bitmap;
        uint32_t bg_inode_bitmap;
        uint32_t bg_inode_table;
        uint16_t bg_free_blocks_count;
        uint16_t bg_free_inodes_count;
        uint16_t bg_used_dirs_count;
        uint16_t bg_pad;
        uint8_t  bg_reserved[12];
    } __attribute__((packed));

    struct Inode {
        uint16_t i_mode;
        uint16_t i_uid;
        uint32_t i_size;
        uint32_t i_atime;
        uint32_t i_ctime;
        uint32_t i_mtime;
        uint32_t i_dtime;
        uint16_t i_gid;
        uint16_t i_links_count;
        uint32_t i_blocks;
        uint32_t i_flags;
        uint32_t i_osd1;
        uint32_t i_block[15];   // 0-11: direct, 12: indirect, 13: double-indirect, 14: triple-indirect
        uint32_t i_generation;
        uint32_t i_file_acl;
        uint32_t i_dir_acl;     // upper 32 bits of size for regular files in rev 1
        uint32_t i_faddr;
        uint8_t  i_osd2[12];
    } __attribute__((packed));

    struct DirEntry {
        uint32_t inode;
        uint16_t rec_len;
        uint8_t  name_len;
        uint8_t  file_type;
        // name follows (up to 255 bytes, NOT null-terminated on disk)
    } __attribute__((packed));

    // =========================================================================
    // Types
    // =========================================================================

    struct Ext2File {
        bool     inUse;
        uint32_t inodeNum;
        Inode    inode;
        bool     isDirectory;
    };

    struct Ext2Instance {
        bool     active;
        int      blockDevIndex;
        uint64_t partStartLba;

        // Superblock fields
        uint32_t blockSize;        // 1024 << s_log_block_size
        uint32_t inodeSize;
        uint32_t inodesPerGroup;
        uint32_t blocksPerGroup;
        uint32_t totalInodes;
        uint32_t totalBlocks;
        uint32_t firstDataBlock;
        uint32_t groupCount;
        char     volumeLabel[17];

        // Block group descriptor table (cached in memory)
        BlockGroupDescriptor* bgdt;
        int bgdtPages;

        // Temporary block buffer (one block, page-aligned)
        uint8_t* blockBuf;
        int      blockBufPages;

        // Open file handles
        Ext2File files[MaxFilesPerInstance];

        // ReadDir name cache
        char dirNames[MaxDirEntries][MaxNameLen];
        int  dirNameCount;
    };

    // =========================================================================
    // Instance table
    // =========================================================================

    static Ext2Instance g_instances[MaxInstances] = {};
    static int g_instanceCount = 0;

    // =========================================================================
    // Low-level helpers
    // =========================================================================

    static bool ReadPartSectors(const Ext2Instance& inst, uint64_t partSector,
                                 uint32_t count, void* buf) {
        auto* dev = Drivers::Storage::GetBlockDevice(inst.blockDevIndex);
        if (!dev) return false;
        return dev->ReadSectors(dev->Ctx, inst.partStartLba + partSector, count, buf);
    }

    static bool WritePartSectors(const Ext2Instance& inst, uint64_t partSector,
                                  uint32_t count, const void* buf) {
        auto* dev = Drivers::Storage::GetBlockDevice(inst.blockDevIndex);
        if (!dev) return false;
        return dev->WriteSectors(dev->Ctx, inst.partStartLba + partSector, count, buf);
    }

    // Convert a block number to a partition-relative sector number
    static uint64_t BlockToPartSector(const Ext2Instance& inst, uint32_t block) {
        return (uint64_t)block * (inst.blockSize / 512);
    }

    static uint32_t SectorsPerBlock(const Ext2Instance& inst) {
        return inst.blockSize / 512;
    }

    static bool ReadBlock(const Ext2Instance& inst, uint32_t blockNum, void* buf) {
        if (blockNum == 0) return false;
        return ReadPartSectors(inst, BlockToPartSector(inst, blockNum),
                               SectorsPerBlock(inst), buf);
    }

    static bool WriteBlock(const Ext2Instance& inst, uint32_t blockNum, const void* buf) {
        if (blockNum == 0) return false;
        return WritePartSectors(inst, BlockToPartSector(inst, blockNum),
                                SectorsPerBlock(inst), buf);
    }

    // =========================================================================
    // Inode operations
    // =========================================================================

    static bool ReadInode(Ext2Instance& inst, uint32_t inodeNum, Inode* out) {
        if (inodeNum == 0 || inodeNum > inst.totalInodes) return false;

        uint32_t group = (inodeNum - 1) / inst.inodesPerGroup;
        uint32_t indexInGroup = (inodeNum - 1) % inst.inodesPerGroup;

        if (group >= inst.groupCount) return false;

        uint32_t inodeTableBlock = inst.bgdt[group].bg_inode_table;
        uint32_t inodeByteOffset = indexInGroup * inst.inodeSize;
        uint32_t blockOffset = inodeByteOffset / inst.blockSize;
        uint32_t offsetInBlock = inodeByteOffset % inst.blockSize;

        if (!ReadBlock(inst, inodeTableBlock + blockOffset, inst.blockBuf)) return false;

        memcpy(out, inst.blockBuf + offsetInBlock, sizeof(Inode));
        return true;
    }

    static bool WriteInode(Ext2Instance& inst, uint32_t inodeNum, const Inode* inode) {
        if (inodeNum == 0 || inodeNum > inst.totalInodes) return false;

        uint32_t group = (inodeNum - 1) / inst.inodesPerGroup;
        uint32_t indexInGroup = (inodeNum - 1) % inst.inodesPerGroup;

        if (group >= inst.groupCount) return false;

        uint32_t inodeTableBlock = inst.bgdt[group].bg_inode_table;
        uint32_t inodeByteOffset = indexInGroup * inst.inodeSize;
        uint32_t blockOffset = inodeByteOffset / inst.blockSize;
        uint32_t offsetInBlock = inodeByteOffset % inst.blockSize;

        if (!ReadBlock(inst, inodeTableBlock + blockOffset, inst.blockBuf)) return false;
        memcpy(inst.blockBuf + offsetInBlock, inode, sizeof(Inode));
        return WriteBlock(inst, inodeTableBlock + blockOffset, inst.blockBuf);
    }

    // =========================================================================
    // Block addressing — resolve logical block index to physical block number
    // =========================================================================

    // Returns the physical block number for logical block index `logicalIdx`
    // within the given inode. Handles direct, indirect, double-indirect, and
    // triple-indirect blocks.
    static uint32_t GetPhysicalBlock(Ext2Instance& inst, const Inode& inode,
                                      uint32_t logicalIdx) {
        uint32_t ptrsPerBlock = inst.blockSize / 4;

        // Direct blocks (0-11)
        if (logicalIdx < 12) {
            return inode.i_block[logicalIdx];
        }
        logicalIdx -= 12;

        // Single indirect (block 12)
        if (logicalIdx < ptrsPerBlock) {
            if (inode.i_block[12] == 0) return 0;
            if (!ReadBlock(inst, inode.i_block[12], inst.blockBuf)) return 0;
            uint32_t block;
            memcpy(&block, inst.blockBuf + logicalIdx * 4, 4);
            return block;
        }
        logicalIdx -= ptrsPerBlock;

        // Double indirect (block 13)
        uint32_t dblRange = ptrsPerBlock * ptrsPerBlock;
        if (logicalIdx < dblRange) {
            if (inode.i_block[13] == 0) return 0;
            if (!ReadBlock(inst, inode.i_block[13], inst.blockBuf)) return 0;

            uint32_t idx1 = logicalIdx / ptrsPerBlock;
            uint32_t idx2 = logicalIdx % ptrsPerBlock;

            uint32_t indirectBlock;
            memcpy(&indirectBlock, inst.blockBuf + idx1 * 4, 4);
            if (indirectBlock == 0) return 0;

            if (!ReadBlock(inst, indirectBlock, inst.blockBuf)) return 0;
            uint32_t block;
            memcpy(&block, inst.blockBuf + idx2 * 4, 4);
            return block;
        }
        logicalIdx -= dblRange;

        // Triple indirect (block 14)
        uint32_t triRange = ptrsPerBlock * ptrsPerBlock * ptrsPerBlock;
        if (logicalIdx < triRange) {
            if (inode.i_block[14] == 0) return 0;
            if (!ReadBlock(inst, inode.i_block[14], inst.blockBuf)) return 0;

            uint32_t idx1 = logicalIdx / (ptrsPerBlock * ptrsPerBlock);
            uint32_t rem = logicalIdx % (ptrsPerBlock * ptrsPerBlock);
            uint32_t idx2 = rem / ptrsPerBlock;
            uint32_t idx3 = rem % ptrsPerBlock;

            uint32_t dblBlock;
            memcpy(&dblBlock, inst.blockBuf + idx1 * 4, 4);
            if (dblBlock == 0) return 0;

            if (!ReadBlock(inst, dblBlock, inst.blockBuf)) return 0;
            uint32_t indBlock;
            memcpy(&indBlock, inst.blockBuf + idx2 * 4, 4);
            if (indBlock == 0) return 0;

            if (!ReadBlock(inst, indBlock, inst.blockBuf)) return 0;
            uint32_t block;
            memcpy(&block, inst.blockBuf + idx3 * 4, 4);
            return block;
        }

        return 0; // beyond addressable range
    }

    // =========================================================================
    // Block allocation
    // =========================================================================

    static uint32_t AllocateBlock(Ext2Instance& inst, uint32_t preferGroup) {
        // Try the preferred group first, then scan all groups
        for (uint32_t attempt = 0; attempt < inst.groupCount; attempt++) {
            uint32_t g = (preferGroup + attempt) % inst.groupCount;
            if (inst.bgdt[g].bg_free_blocks_count == 0) continue;

            uint32_t bitmapBlock = inst.bgdt[g].bg_block_bitmap;
            if (!ReadBlock(inst, bitmapBlock, inst.blockBuf)) continue;

            uint32_t blocksInGroup = inst.blocksPerGroup;
            // Last group may have fewer blocks
            if (g == inst.groupCount - 1) {
                uint32_t remaining = inst.totalBlocks - g * inst.blocksPerGroup;
                if (remaining < blocksInGroup) blocksInGroup = remaining;
            }

            for (uint32_t bit = 0; bit < blocksInGroup; bit++) {
                uint32_t byteIdx = bit / 8;
                uint8_t bitMask = 1 << (bit % 8);
                if (!(inst.blockBuf[byteIdx] & bitMask)) {
                    // Found a free block — mark it used
                    inst.blockBuf[byteIdx] |= bitMask;
                    if (!WriteBlock(inst, bitmapBlock, inst.blockBuf)) continue;

                    inst.bgdt[g].bg_free_blocks_count--;

                    // Write updated BGDT entry back to disk
                    uint32_t bgdtBlock = inst.firstDataBlock + 1;
                    uint32_t bgdtOffset = g * sizeof(BlockGroupDescriptor);
                    uint32_t bgdtBlockIdx = bgdtBlock + bgdtOffset / inst.blockSize;
                    uint32_t bgdtOffInBlock = bgdtOffset % inst.blockSize;

                    uint8_t* tmpBuf = inst.blockBuf;
                    if (ReadBlock(inst, bgdtBlockIdx, tmpBuf)) {
                        memcpy(tmpBuf + bgdtOffInBlock, &inst.bgdt[g],
                               sizeof(BlockGroupDescriptor));
                        WriteBlock(inst, bgdtBlockIdx, tmpBuf);
                    }

                    return inst.firstDataBlock + g * inst.blocksPerGroup + bit;
                }
            }
        }
        return 0; // no free blocks
    }

    static void FreeBlock(Ext2Instance& inst, uint32_t blockNum) {
        if (blockNum < inst.firstDataBlock || blockNum >= inst.totalBlocks) return;

        uint32_t adjusted = blockNum - inst.firstDataBlock;
        uint32_t g = adjusted / inst.blocksPerGroup;
        uint32_t bit = adjusted % inst.blocksPerGroup;

        if (g >= inst.groupCount) return;

        uint32_t bitmapBlock = inst.bgdt[g].bg_block_bitmap;
        if (!ReadBlock(inst, bitmapBlock, inst.blockBuf)) return;

        uint32_t byteIdx = bit / 8;
        uint8_t bitMask = 1 << (bit % 8);
        inst.blockBuf[byteIdx] &= ~bitMask;
        WriteBlock(inst, bitmapBlock, inst.blockBuf);

        inst.bgdt[g].bg_free_blocks_count++;

        // Write updated BGDT
        uint32_t bgdtBlock = inst.firstDataBlock + 1;
        uint32_t bgdtOffset = g * sizeof(BlockGroupDescriptor);
        uint32_t bgdtBlockIdx = bgdtBlock + bgdtOffset / inst.blockSize;
        uint32_t bgdtOffInBlock = bgdtOffset % inst.blockSize;

        if (ReadBlock(inst, bgdtBlockIdx, inst.blockBuf)) {
            memcpy(inst.blockBuf + bgdtOffInBlock, &inst.bgdt[g],
                   sizeof(BlockGroupDescriptor));
            WriteBlock(inst, bgdtBlockIdx, inst.blockBuf);
        }
    }

    // =========================================================================
    // Inode allocation
    // =========================================================================

    static uint32_t AllocateInode(Ext2Instance& inst, uint32_t preferGroup) {
        for (uint32_t attempt = 0; attempt < inst.groupCount; attempt++) {
            uint32_t g = (preferGroup + attempt) % inst.groupCount;
            if (inst.bgdt[g].bg_free_inodes_count == 0) continue;

            uint32_t bitmapBlock = inst.bgdt[g].bg_inode_bitmap;
            if (!ReadBlock(inst, bitmapBlock, inst.blockBuf)) continue;

            for (uint32_t bit = 0; bit < inst.inodesPerGroup; bit++) {
                uint32_t byteIdx = bit / 8;
                uint8_t bitMask = 1 << (bit % 8);
                if (!(inst.blockBuf[byteIdx] & bitMask)) {
                    inst.blockBuf[byteIdx] |= bitMask;
                    if (!WriteBlock(inst, bitmapBlock, inst.blockBuf)) continue;

                    inst.bgdt[g].bg_free_inodes_count--;

                    // Write updated BGDT entry
                    uint32_t bgdtBlock = inst.firstDataBlock + 1;
                    uint32_t bgdtOffset = g * sizeof(BlockGroupDescriptor);
                    uint32_t bgdtBlockIdx = bgdtBlock + bgdtOffset / inst.blockSize;
                    uint32_t bgdtOffInBlock = bgdtOffset % inst.blockSize;

                    if (ReadBlock(inst, bgdtBlockIdx, inst.blockBuf)) {
                        memcpy(inst.blockBuf + bgdtOffInBlock, &inst.bgdt[g],
                               sizeof(BlockGroupDescriptor));
                        WriteBlock(inst, bgdtBlockIdx, inst.blockBuf);
                    }

                    return g * inst.inodesPerGroup + bit + 1; // inodes are 1-based
                }
            }
        }
        return 0;
    }

    static void FreeInode(Ext2Instance& inst, uint32_t inodeNum) {
        if (inodeNum == 0 || inodeNum > inst.totalInodes) return;

        uint32_t g = (inodeNum - 1) / inst.inodesPerGroup;
        uint32_t bit = (inodeNum - 1) % inst.inodesPerGroup;

        if (g >= inst.groupCount) return;

        uint32_t bitmapBlock = inst.bgdt[g].bg_inode_bitmap;
        if (!ReadBlock(inst, bitmapBlock, inst.blockBuf)) return;

        uint32_t byteIdx = bit / 8;
        uint8_t bitMask = 1 << (bit % 8);
        inst.blockBuf[byteIdx] &= ~bitMask;
        WriteBlock(inst, bitmapBlock, inst.blockBuf);

        inst.bgdt[g].bg_free_inodes_count++;

        uint32_t bgdtBlock = inst.firstDataBlock + 1;
        uint32_t bgdtOffset = g * sizeof(BlockGroupDescriptor);
        uint32_t bgdtBlockIdx = bgdtBlock + bgdtOffset / inst.blockSize;
        uint32_t bgdtOffInBlock = bgdtOffset % inst.blockSize;

        if (ReadBlock(inst, bgdtBlockIdx, inst.blockBuf)) {
            memcpy(inst.blockBuf + bgdtOffInBlock, &inst.bgdt[g],
                   sizeof(BlockGroupDescriptor));
            WriteBlock(inst, bgdtBlockIdx, inst.blockBuf);
        }
    }

    // =========================================================================
    // Block assignment to inode — set a logical block index to a physical block
    // =========================================================================

    // Allocate an indirect block if needed, zero it, and return its number.
    static uint32_t EnsureIndirectBlock(Ext2Instance& inst, uint32_t* blockPtr,
                                         uint32_t preferGroup) {
        if (*blockPtr != 0) return *blockPtr;
        uint32_t newBlock = AllocateBlock(inst, preferGroup);
        if (newBlock == 0) return 0;
        // Zero the new indirect block
        memset(inst.blockBuf, 0, inst.blockSize);
        WriteBlock(inst, newBlock, inst.blockBuf);
        *blockPtr = newBlock;
        return newBlock;
    }

    // Set logical block `logicalIdx` in `inode` to point to `physBlock`.
    // Allocates indirect blocks as needed. Returns true on success.
    static bool SetPhysicalBlock(Ext2Instance& inst, Inode& inode,
                                  uint32_t logicalIdx, uint32_t physBlock,
                                  uint32_t preferGroup) {
        uint32_t ptrsPerBlock = inst.blockSize / 4;

        // Direct blocks (0-11)
        if (logicalIdx < 12) {
            inode.i_block[logicalIdx] = physBlock;
            return true;
        }
        logicalIdx -= 12;

        // Single indirect
        if (logicalIdx < ptrsPerBlock) {
            uint32_t indBlock = EnsureIndirectBlock(inst, &inode.i_block[12], preferGroup);
            if (indBlock == 0) return false;
            if (!ReadBlock(inst, indBlock, inst.blockBuf)) return false;
            memcpy(inst.blockBuf + logicalIdx * 4, &physBlock, 4);
            return WriteBlock(inst, indBlock, inst.blockBuf);
        }
        logicalIdx -= ptrsPerBlock;

        // Double indirect
        uint32_t dblRange = ptrsPerBlock * ptrsPerBlock;
        if (logicalIdx < dblRange) {
            uint32_t dblBlock = EnsureIndirectBlock(inst, &inode.i_block[13], preferGroup);
            if (dblBlock == 0) return false;
            if (!ReadBlock(inst, dblBlock, inst.blockBuf)) return false;

            uint32_t idx1 = logicalIdx / ptrsPerBlock;
            uint32_t idx2 = logicalIdx % ptrsPerBlock;

            uint32_t indBlock;
            memcpy(&indBlock, inst.blockBuf + idx1 * 4, 4);

            // Need a separate buffer for the double-indirect table since
            // EnsureIndirectBlock uses blockBuf. Save and restore.
            uint8_t savedPtr[4];
            if (indBlock == 0) {
                indBlock = AllocateBlock(inst, preferGroup);
                if (indBlock == 0) return false;
                memset(inst.blockBuf, 0, inst.blockSize);
                WriteBlock(inst, indBlock, inst.blockBuf);

                // Re-read the double-indirect block and update
                if (!ReadBlock(inst, dblBlock, inst.blockBuf)) return false;
                memcpy(inst.blockBuf + idx1 * 4, &indBlock, 4);
                if (!WriteBlock(inst, dblBlock, inst.blockBuf)) return false;
            }

            if (!ReadBlock(inst, indBlock, inst.blockBuf)) return false;
            memcpy(inst.blockBuf + idx2 * 4, &physBlock, 4);
            return WriteBlock(inst, indBlock, inst.blockBuf);
        }
        logicalIdx -= dblRange;

        // Triple indirect — not implemented (would require very large files)
        return false;
    }

    // Free all data blocks belonging to an inode (direct + indirect trees)
    static void FreeInodeBlocks(Ext2Instance& inst, Inode& inode) {
        uint32_t ptrsPerBlock = inst.blockSize / 4;

        // Free direct blocks
        for (int i = 0; i < 12; i++) {
            if (inode.i_block[i]) {
                FreeBlock(inst, inode.i_block[i]);
                inode.i_block[i] = 0;
            }
        }

        // Free single indirect
        if (inode.i_block[12]) {
            // Need a temporary buffer since blockBuf is used by FreeBlock
            uint32_t indBlock = inode.i_block[12];
            // Allocate a temp page for reading the indirect block
            uint8_t* tmpBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
            if (tmpBuf && ReadBlock(inst, indBlock, tmpBuf)) {
                for (uint32_t i = 0; i < ptrsPerBlock; i++) {
                    uint32_t b;
                    memcpy(&b, tmpBuf + i * 4, 4);
                    if (b) FreeBlock(inst, b);
                }
            }
            if (tmpBuf) Memory::g_pfa->Free(tmpBuf);
            FreeBlock(inst, indBlock);
            inode.i_block[12] = 0;
        }

        // Free double indirect
        if (inode.i_block[13]) {
            uint32_t dblBlock = inode.i_block[13];
            uint8_t* tmpBuf1 = (uint8_t*)Memory::g_pfa->AllocateZeroed();
            if (tmpBuf1 && ReadBlock(inst, dblBlock, tmpBuf1)) {
                uint8_t* tmpBuf2 = (uint8_t*)Memory::g_pfa->AllocateZeroed();
                for (uint32_t i = 0; i < ptrsPerBlock; i++) {
                    uint32_t indBlock;
                    memcpy(&indBlock, tmpBuf1 + i * 4, 4);
                    if (indBlock == 0) continue;
                    if (tmpBuf2 && ReadBlock(inst, indBlock, tmpBuf2)) {
                        for (uint32_t j = 0; j < ptrsPerBlock; j++) {
                            uint32_t b;
                            memcpy(&b, tmpBuf2 + j * 4, 4);
                            if (b) FreeBlock(inst, b);
                        }
                    }
                    FreeBlock(inst, indBlock);
                }
                if (tmpBuf2) Memory::g_pfa->Free(tmpBuf2);
            }
            if (tmpBuf1) Memory::g_pfa->Free(tmpBuf1);
            FreeBlock(inst, dblBlock);
            inode.i_block[13] = 0;
        }

        // Triple indirect — free recursively if present
        if (inode.i_block[14]) {
            // For simplicity, just free the top-level triple indirect block.
            // Full triple-indirect traversal is rare and complex.
            FreeBlock(inst, inode.i_block[14]);
            inode.i_block[14] = 0;
        }

        inode.i_blocks = 0;
        inode.i_size = 0;
    }

    // =========================================================================
    // String helpers
    // =========================================================================

    static char ToLower(char c) {
        return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }

    static bool StrEqual(const char* a, const char* b) {
        while (*a && *b) {
            if (*a != *b) return false;
            a++; b++;
        }
        return *a == *b;
    }

    // Split a path into parent directory path and filename
    static void SplitPath(const char* path, char* parentPath, int parentMax,
                           char* fileName, int nameMax) {
        while (*path == '/') path++;

        int len = 0;
        while (path[len]) len++;

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
    // Directory operations
    // =========================================================================

    struct ParsedEntry {
        char     name[MaxNameLen];
        uint32_t inodeNum;
        uint8_t  fileType;
    };

    // Find a single entry by name in a directory inode.
    static bool FindInDirectory(Ext2Instance& inst, const Inode& dirInode,
                                 const char* name, ParsedEntry* out) {
        uint32_t dirSize = dirInode.i_size;
        uint32_t blockSize = inst.blockSize;
        uint32_t numBlocks = (dirSize + blockSize - 1) / blockSize;

        // We need a separate buffer for directory data since GetPhysicalBlock
        // uses inst.blockBuf for indirect block reads
        uint8_t* dirBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dirBuf) return false;

        for (uint32_t bi = 0; bi < numBlocks; bi++) {
            uint32_t physBlock = GetPhysicalBlock(inst, dirInode, bi);
            if (physBlock == 0) continue;
            if (!ReadBlock(inst, physBlock, dirBuf)) continue;

            uint32_t pos = 0;
            uint32_t remaining = dirSize - bi * blockSize;
            if (remaining > blockSize) remaining = blockSize;

            while (pos + 8 <= remaining) {
                DirEntry* de = (DirEntry*)(dirBuf + pos);
                if (de->rec_len == 0) break;
                if (de->rec_len < 8 || pos + de->rec_len > blockSize) break;

                if (de->inode != 0 && de->name_len > 0) {
                    char entryName[MaxNameLen];
                    int nameLen = de->name_len;
                    if (nameLen >= MaxNameLen) nameLen = MaxNameLen - 1;
                    memcpy(entryName, (uint8_t*)de + sizeof(DirEntry), nameLen);
                    entryName[nameLen] = '\0';

                    if (StrEqual(entryName, name)) {
                        out->inodeNum = de->inode;
                        out->fileType = de->file_type;
                        memcpy(out->name, entryName, nameLen + 1);
                        Memory::g_pfa->Free(dirBuf);
                        return true;
                    }
                }

                pos += de->rec_len;
            }
        }

        Memory::g_pfa->Free(dirBuf);
        return false;
    }

    // Read all entries from a directory
    static int ReadDirectoryEntries(Ext2Instance& inst, const Inode& dirInode,
                                     ParsedEntry* entries, int maxEntries) {
        uint32_t dirSize = dirInode.i_size;
        uint32_t blockSize = inst.blockSize;
        uint32_t numBlocks = (dirSize + blockSize - 1) / blockSize;
        int count = 0;

        uint8_t* dirBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dirBuf) return 0;

        for (uint32_t bi = 0; bi < numBlocks && count < maxEntries; bi++) {
            uint32_t physBlock = GetPhysicalBlock(inst, dirInode, bi);
            if (physBlock == 0) continue;
            if (!ReadBlock(inst, physBlock, dirBuf)) continue;

            uint32_t pos = 0;
            uint32_t remaining = dirSize - bi * blockSize;
            if (remaining > blockSize) remaining = blockSize;

            while (pos + 8 <= remaining && count < maxEntries) {
                DirEntry* de = (DirEntry*)(dirBuf + pos);
                if (de->rec_len == 0) break;
                if (de->rec_len < 8 || pos + de->rec_len > blockSize) break;

                if (de->inode != 0 && de->name_len > 0) {
                    int nameLen = de->name_len;
                    if (nameLen >= MaxNameLen) nameLen = MaxNameLen - 1;
                    memcpy(entries[count].name,
                           (uint8_t*)de + sizeof(DirEntry), nameLen);
                    entries[count].name[nameLen] = '\0';

                    // Skip . and ..
                    if (entries[count].name[0] == '.' &&
                        (entries[count].name[1] == '\0' ||
                         (entries[count].name[1] == '.' && entries[count].name[2] == '\0'))) {
                        pos += de->rec_len;
                        continue;
                    }

                    entries[count].inodeNum = de->inode;
                    entries[count].fileType = de->file_type;
                    count++;
                }

                pos += de->rec_len;
            }
        }

        Memory::g_pfa->Free(dirBuf);
        return count;
    }

    // Add a directory entry to a directory inode
    static bool AddDirEntry(Ext2Instance& inst, uint32_t dirInodeNum, Inode& dirInode,
                             uint32_t childInodeNum, const char* name, uint8_t fileType) {
        uint32_t nameLen = 0;
        while (name[nameLen]) nameLen++;

        uint32_t neededLen = ((sizeof(DirEntry) + nameLen + 3) / 4) * 4; // 4-byte aligned
        uint32_t blockSize = inst.blockSize;
        uint32_t numBlocks = (dirInode.i_size + blockSize - 1) / blockSize;

        uint8_t* dirBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dirBuf) return false;

        // Try to find space in existing directory blocks
        for (uint32_t bi = 0; bi < numBlocks; bi++) {
            uint32_t physBlock = GetPhysicalBlock(inst, dirInode, bi);
            if (physBlock == 0) continue;
            if (!ReadBlock(inst, physBlock, dirBuf)) continue;

            uint32_t pos = 0;
            while (pos + 8 <= blockSize) {
                DirEntry* de = (DirEntry*)(dirBuf + pos);
                if (de->rec_len == 0) break;
                if (de->rec_len < 8 || pos + de->rec_len > blockSize) break;

                // Check if this entry has slack space we can use
                uint32_t actualLen;
                if (de->inode == 0) {
                    actualLen = 0; // unused entry — entire rec_len is available
                } else {
                    actualLen = ((sizeof(DirEntry) + de->name_len + 3) / 4) * 4;
                }

                uint32_t slack = de->rec_len - actualLen;
                if (slack >= neededLen) {
                    // Split this entry
                    if (de->inode != 0) {
                        uint16_t oldRecLen = de->rec_len;
                        de->rec_len = (uint16_t)actualLen;

                        DirEntry* newDe = (DirEntry*)(dirBuf + pos + actualLen);
                        newDe->inode = childInodeNum;
                        newDe->rec_len = (uint16_t)(oldRecLen - actualLen);
                        newDe->name_len = (uint8_t)nameLen;
                        newDe->file_type = fileType;
                        memcpy((uint8_t*)newDe + sizeof(DirEntry), name, nameLen);
                    } else {
                        // Reuse this empty entry
                        de->inode = childInodeNum;
                        // Keep rec_len as-is
                        de->name_len = (uint8_t)nameLen;
                        de->file_type = fileType;
                        memcpy((uint8_t*)de + sizeof(DirEntry), name, nameLen);
                    }

                    WriteBlock(inst, physBlock, dirBuf);
                    Memory::g_pfa->Free(dirBuf);
                    return true;
                }

                pos += de->rec_len;
            }
        }

        // No space — allocate a new block for the directory
        uint32_t group = (dirInodeNum - 1) / inst.inodesPerGroup;
        uint32_t newBlock = AllocateBlock(inst, group);
        if (newBlock == 0) {
            Memory::g_pfa->Free(dirBuf);
            return false;
        }

        // Initialize new block with the entry
        memset(dirBuf, 0, blockSize);
        DirEntry* de = (DirEntry*)dirBuf;
        de->inode = childInodeNum;
        de->rec_len = (uint16_t)blockSize; // fills entire block
        de->name_len = (uint8_t)nameLen;
        de->file_type = fileType;
        memcpy((uint8_t*)de + sizeof(DirEntry), name, nameLen);

        if (!WriteBlock(inst, newBlock, dirBuf)) {
            FreeBlock(inst, newBlock);
            Memory::g_pfa->Free(dirBuf);
            return false;
        }

        // Assign new block to directory inode
        uint32_t newLogicalIdx = numBlocks;
        if (!SetPhysicalBlock(inst, dirInode, newLogicalIdx, newBlock, group)) {
            FreeBlock(inst, newBlock);
            Memory::g_pfa->Free(dirBuf);
            return false;
        }

        dirInode.i_size += blockSize;
        dirInode.i_blocks += blockSize / 512;
        WriteInode(inst, dirInodeNum, &dirInode);

        Memory::g_pfa->Free(dirBuf);
        return true;
    }

    // Remove a directory entry by name
    static bool RemoveDirEntry(Ext2Instance& inst, const Inode& dirInode,
                                const char* name) {
        uint32_t dirSize = dirInode.i_size;
        uint32_t blockSize = inst.blockSize;
        uint32_t numBlocks = (dirSize + blockSize - 1) / blockSize;

        uint8_t* dirBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dirBuf) return false;

        for (uint32_t bi = 0; bi < numBlocks; bi++) {
            uint32_t physBlock = GetPhysicalBlock(inst, dirInode, bi);
            if (physBlock == 0) continue;
            if (!ReadBlock(inst, physBlock, dirBuf)) continue;

            uint32_t pos = 0;
            DirEntry* prevDe = nullptr;

            while (pos + 8 <= blockSize) {
                DirEntry* de = (DirEntry*)(dirBuf + pos);
                if (de->rec_len == 0) break;
                if (de->rec_len < 8 || pos + de->rec_len > blockSize) break;

                if (de->inode != 0 && de->name_len > 0) {
                    char entryName[MaxNameLen];
                    int nameLen = de->name_len;
                    if (nameLen >= MaxNameLen) nameLen = MaxNameLen - 1;
                    memcpy(entryName, (uint8_t*)de + sizeof(DirEntry), nameLen);
                    entryName[nameLen] = '\0';

                    if (StrEqual(entryName, name)) {
                        if (prevDe) {
                            // Merge with previous entry
                            prevDe->rec_len += de->rec_len;
                        } else {
                            // First entry in block — just zero the inode
                            de->inode = 0;
                        }
                        WriteBlock(inst, physBlock, dirBuf);
                        Memory::g_pfa->Free(dirBuf);
                        return true;
                    }
                }

                prevDe = de;
                pos += de->rec_len;
            }
        }

        Memory::g_pfa->Free(dirBuf);
        return false;
    }

    // =========================================================================
    // Path traversal
    // =========================================================================

    // Traverse a full path from root. Returns true and fills out inode number
    // and inode data.
    static bool TraversePath(Ext2Instance& inst, const char* path,
                              uint32_t* outInodeNum, Inode* outInode) {
        while (*path == '/') path++;

        // Empty path = root directory
        uint32_t currentInode = EXT2_ROOT_INODE;
        Inode inode;
        if (!ReadInode(inst, currentInode, &inode)) return false;

        if (*path == '\0') {
            *outInodeNum = currentInode;
            *outInode = inode;
            return true;
        }

        while (*path) {
            // Current inode must be a directory
            if ((inode.i_mode & IMODE_TYPE_MASK) != IMODE_DIR) return false;

            // Extract next path component
            char component[MaxNameLen];
            int len = 0;
            while (*path && *path != '/' && len < MaxNameLen - 1) {
                component[len++] = *path++;
            }
            component[len] = '\0';
            while (*path == '/') path++;

            ParsedEntry found;
            if (!FindInDirectory(inst, inode, component, &found)) return false;

            currentInode = found.inodeNum;
            if (!ReadInode(inst, currentInode, &inode)) return false;

            if (*path == '\0') {
                *outInodeNum = currentInode;
                *outInode = inode;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // FsDriver implementation functions
    // =========================================================================

    static int OpenImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        uint32_t inodeNum;
        Inode inode;
        if (!TraversePath(self, path, &inodeNum, &inode)) return -1;

        for (int i = 0; i < MaxFilesPerInstance; i++) {
            if (!self.files[i].inUse) {
                self.files[i].inUse = true;
                self.files[i].inodeNum = inodeNum;
                self.files[i].inode = inode;
                self.files[i].isDirectory =
                    (inode.i_mode & IMODE_TYPE_MASK) == IMODE_DIR;
                return i;
            }
        }

        return -1;
    }

    static int ReadImpl(int inst, int handle, uint8_t* buffer,
                         uint64_t offset, uint64_t size) {
        if (inst < 0 || inst >= g_instanceCount) return -1;
        auto& self = g_instances[inst];
        if (handle < 0 || handle >= MaxFilesPerInstance || !self.files[handle].inUse) return -1;

        auto& file = self.files[handle];
        if (file.isDirectory) return -1;

        uint32_t fileSize = file.inode.i_size;
        if (offset >= fileSize) return 0;
        if (offset + size > fileSize) size = fileSize - offset;
        if (size == 0) return 0;

        uint32_t blockSize = self.blockSize;
        uint64_t bytesRead = 0;

        // We need a separate buffer for data reads since GetPhysicalBlock uses blockBuf
        uint8_t* dataBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dataBuf) return -1;

        while (bytesRead < size) {
            uint32_t logicalBlock = (uint32_t)((offset + bytesRead) / blockSize);
            uint32_t blockOff = (uint32_t)((offset + bytesRead) % blockSize);

            uint32_t physBlock = GetPhysicalBlock(self, file.inode, logicalBlock);
            if (physBlock == 0) break;

            if (!ReadBlock(self, physBlock, dataBuf)) break;

            uint32_t available = blockSize - blockOff;
            uint64_t toRead = size - bytesRead;
            if (toRead > available) toRead = available;

            memcpy(buffer + bytesRead, dataBuf + blockOff, toRead);
            bytesRead += toRead;
        }

        Memory::g_pfa->Free(dataBuf);
        return (int)bytesRead;
    }

    static uint64_t GetSizeImpl(int inst, int handle) {
        if (inst < 0 || inst >= g_instanceCount) return 0;
        auto& self = g_instances[inst];
        if (handle < 0 || handle >= MaxFilesPerInstance || !self.files[handle].inUse) return 0;
        return self.files[handle].inode.i_size;
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

        uint32_t inodeNum;
        Inode inode;
        if (!TraversePath(self, path, &inodeNum, &inode)) return -1;
        if ((inode.i_mode & IMODE_TYPE_MASK) != IMODE_DIR) return -1;

        ParsedEntry entries[MaxDirEntries];
        int limit = maxEntries < MaxDirEntries ? maxEntries : MaxDirEntries;
        int count = ReadDirectoryEntries(self, inode, entries, limit);

        self.dirNameCount = count;
        for (int i = 0; i < count; i++) {
            int j = 0;
            while (entries[i].name[j] && j < MaxNameLen - 2) {
                self.dirNames[i][j] = entries[i].name[j];
                j++;
            }
            // Append trailing '/' for directories so userspace can distinguish them
            if (entries[i].fileType == EXT2_FT_DIR && j < MaxNameLen - 1) {
                self.dirNames[i][j++] = '/';
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

        uint32_t blockSize = self.blockSize;
        uint32_t group = (file.inodeNum - 1) / self.inodesPerGroup;

        // Allocate a separate buffer for data I/O
        uint8_t* dataBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dataBuf) return -1;

        uint64_t bytesWritten = 0;

        while (bytesWritten < size) {
            uint32_t logicalBlock = (uint32_t)((offset + bytesWritten) / blockSize);
            uint32_t blockOff = (uint32_t)((offset + bytesWritten) % blockSize);

            uint32_t physBlock = GetPhysicalBlock(self, file.inode, logicalBlock);

            if (physBlock == 0) {
                // Need to allocate a new block
                physBlock = AllocateBlock(self, group);
                if (physBlock == 0) break;

                // Zero the new block
                memset(dataBuf, 0, blockSize);
                WriteBlock(self, physBlock, dataBuf);

                if (!SetPhysicalBlock(self, file.inode, logicalBlock, physBlock, group)) {
                    FreeBlock(self, physBlock);
                    break;
                }
                file.inode.i_blocks += blockSize / 512;
            }

            // Read existing block for partial writes
            if (!ReadBlock(self, physBlock, dataBuf)) break;

            uint32_t available = blockSize - blockOff;
            uint64_t toWrite = size - bytesWritten;
            if (toWrite > available) toWrite = available;

            memcpy(dataBuf + blockOff, buffer + bytesWritten, toWrite);
            if (!WriteBlock(self, physBlock, dataBuf)) break;

            bytesWritten += toWrite;
        }

        Memory::g_pfa->Free(dataBuf);

        // Update file size if we wrote past the end
        uint64_t endPos = offset + bytesWritten;
        if (endPos > file.inode.i_size) {
            file.inode.i_size = (uint32_t)endPos;
        }

        // Write updated inode to disk
        if (bytesWritten > 0) {
            WriteInode(self, file.inodeNum, &file.inode);
        }

        return (int)bytesWritten;
    }

    static int CreateImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        char parentPath[MaxNameLen];
        char fileName[MaxNameLen];
        SplitPath(path, parentPath, MaxNameLen, fileName, MaxNameLen);
        if (fileName[0] == '\0') return -1;

        // Traverse to parent directory
        uint32_t parentInodeNum;
        Inode parentInode;
        if (!TraversePath(self, parentPath, &parentInodeNum, &parentInode)) return -1;
        if ((parentInode.i_mode & IMODE_TYPE_MASK) != IMODE_DIR) return -1;

        // Check if file already exists
        ParsedEntry existing;
        if (FindInDirectory(self, parentInode, fileName, &existing)) {
            // If it's a directory, can't truncate
            Inode existInode;
            if (!ReadInode(self, existing.inodeNum, &existInode)) return -1;
            if ((existInode.i_mode & IMODE_TYPE_MASK) == IMODE_DIR) return -1;

            // Truncate: free all blocks and reset size
            FreeInodeBlocks(self, existInode);
            existInode.i_size = 0;
            existInode.i_blocks = 0;
            WriteInode(self, existing.inodeNum, &existInode);

            // Open a handle
            for (int i = 0; i < MaxFilesPerInstance; i++) {
                if (!self.files[i].inUse) {
                    self.files[i].inUse = true;
                    self.files[i].inodeNum = existing.inodeNum;
                    self.files[i].inode = existInode;
                    self.files[i].isDirectory = false;
                    return i;
                }
            }
            return -1;
        }

        // Allocate a new inode
        uint32_t group = (parentInodeNum - 1) / self.inodesPerGroup;
        uint32_t newInodeNum = AllocateInode(self, group);
        if (newInodeNum == 0) return -1;

        // Initialize the new inode
        Inode newInode;
        memset(&newInode, 0, sizeof(Inode));
        newInode.i_mode = IMODE_REG | 0644; // regular file, rw-r--r--
        newInode.i_links_count = 1;
        WriteInode(self, newInodeNum, &newInode);

        // Add directory entry
        if (!AddDirEntry(self, parentInodeNum, parentInode,
                         newInodeNum, fileName, EXT2_FT_REG_FILE)) {
            FreeInode(self, newInodeNum);
            return -1;
        }

        // Open a handle
        for (int i = 0; i < MaxFilesPerInstance; i++) {
            if (!self.files[i].inUse) {
                self.files[i].inUse = true;
                self.files[i].inodeNum = newInodeNum;
                self.files[i].inode = newInode;
                self.files[i].isDirectory = false;
                return i;
            }
        }

        return -1;
    }

    static int DeleteImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        char parentPath[MaxNameLen];
        char fileName[MaxNameLen];
        SplitPath(path, parentPath, MaxNameLen, fileName, MaxNameLen);
        if (fileName[0] == '\0') return -1;

        uint32_t parentInodeNum;
        Inode parentInode;
        if (!TraversePath(self, parentPath, &parentInodeNum, &parentInode)) return -1;
        if ((parentInode.i_mode & IMODE_TYPE_MASK) != IMODE_DIR) return -1;

        ParsedEntry existing;
        if (!FindInDirectory(self, parentInode, fileName, &existing)) return -1;

        Inode targetInode;
        if (!ReadInode(self, existing.inodeNum, &targetInode)) return -1;

        bool isDir = (targetInode.i_mode & IMODE_TYPE_MASK) == IMODE_DIR;
        if (isDir) {
            // Only allow deleting empty directories
            ParsedEntry children[1];
            int childCount = ReadDirectoryEntries(self, targetInode, children, 1);
            if (childCount > 0) return -1;
        }

        // Remove directory entry
        if (!RemoveDirEntry(self, parentInode, fileName)) return -1;

        // If we deleted a subdirectory, decrement parent's link count (for "..")
        if (isDir) {
            parentInode.i_links_count--;
            WriteInode(self, parentInodeNum, &parentInode);
        }

        // Decrement link count
        targetInode.i_links_count--;
        if (isDir) targetInode.i_links_count--; // account for "." self-link
        if (targetInode.i_links_count <= 0) {
            // Free all blocks and the inode
            FreeInodeBlocks(self, targetInode);
            targetInode.i_mode = 0;
            WriteInode(self, existing.inodeNum, &targetInode);
            FreeInode(self, existing.inodeNum);
        } else {
            WriteInode(self, existing.inodeNum, &targetInode);
        }

        return 0;
    }

    static int MkdirImpl(int inst, const char* path) {
        if (inst < 0 || inst >= g_instanceCount || !g_instances[inst].active) return -1;
        auto& self = g_instances[inst];

        char parentPath[MaxNameLen];
        char dirName[MaxNameLen];
        SplitPath(path, parentPath, MaxNameLen, dirName, MaxNameLen);
        if (dirName[0] == '\0') return -1;

        uint32_t parentInodeNum;
        Inode parentInode;
        if (!TraversePath(self, parentPath, &parentInodeNum, &parentInode)) return -1;
        if ((parentInode.i_mode & IMODE_TYPE_MASK) != IMODE_DIR) return -1;

        // If directory already exists, return success
        ParsedEntry existing;
        if (FindInDirectory(self, parentInode, dirName, &existing)) {
            Inode existInode;
            if (ReadInode(self, existing.inodeNum, &existInode) &&
                (existInode.i_mode & IMODE_TYPE_MASK) == IMODE_DIR) {
                return 0;
            }
            return -1; // exists as a file
        }

        // Allocate inode for new directory
        uint32_t group = (parentInodeNum - 1) / self.inodesPerGroup;
        uint32_t newInodeNum = AllocateInode(self, group);
        if (newInodeNum == 0) return -1;

        // Allocate a block for the directory data
        uint32_t dirBlock = AllocateBlock(self, group);
        if (dirBlock == 0) {
            FreeInode(self, newInodeNum);
            return -1;
        }

        // Initialize the directory block with . and .. entries
        uint32_t blockSize = self.blockSize;
        uint8_t* dirBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        if (!dirBuf) {
            FreeBlock(self, dirBlock);
            FreeInode(self, newInodeNum);
            return -1;
        }

        memset(dirBuf, 0, blockSize);

        // "." entry
        DirEntry* dot = (DirEntry*)dirBuf;
        dot->inode = newInodeNum;
        dot->rec_len = 12; // minimum size for "." (8 header + 1 name + 3 padding)
        dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR;
        dirBuf[sizeof(DirEntry)] = '.';

        // ".." entry — takes remaining space in the block
        DirEntry* dotdot = (DirEntry*)(dirBuf + 12);
        dotdot->inode = parentInodeNum;
        dotdot->rec_len = (uint16_t)(blockSize - 12);
        dotdot->name_len = 2;
        dotdot->file_type = EXT2_FT_DIR;
        dirBuf[12 + sizeof(DirEntry)] = '.';
        dirBuf[12 + sizeof(DirEntry) + 1] = '.';

        if (!WriteBlock(self, dirBlock, dirBuf)) {
            Memory::g_pfa->Free(dirBuf);
            FreeBlock(self, dirBlock);
            FreeInode(self, newInodeNum);
            return -1;
        }
        Memory::g_pfa->Free(dirBuf);

        // Initialize the new directory inode
        Inode newInode;
        memset(&newInode, 0, sizeof(Inode));
        newInode.i_mode = IMODE_DIR | 0755; // directory, rwxr-xr-x
        newInode.i_size = blockSize;
        newInode.i_blocks = blockSize / 512;
        newInode.i_links_count = 2; // . and parent's entry
        newInode.i_block[0] = dirBlock;
        WriteInode(self, newInodeNum, &newInode);

        // Add entry to parent directory
        // Re-read parent inode since AddDirEntry may modify it
        if (!ReadInode(self, parentInodeNum, &parentInode)) return -1;
        if (!AddDirEntry(self, parentInodeNum, parentInode,
                         newInodeNum, dirName, EXT2_FT_DIR)) {
            FreeBlock(self, dirBlock);
            FreeInode(self, newInodeNum);
            return -1;
        }

        // Increment parent's link count (for ".." in the new dir)
        parentInode.i_links_count++;
        WriteInode(self, parentInodeNum, &parentInode);

        // Update used_dirs_count in block group descriptor
        uint32_t newGroup = (newInodeNum - 1) / self.inodesPerGroup;
        if (newGroup < self.groupCount) {
            self.bgdt[newGroup].bg_used_dirs_count++;

            uint32_t bgdtBlock = self.firstDataBlock + 1;
            uint32_t bgdtOffset = newGroup * sizeof(BlockGroupDescriptor);
            uint32_t bgdtBlockIdx = bgdtBlock + bgdtOffset / self.blockSize;
            uint32_t bgdtOffInBlock = bgdtOffset % self.blockSize;

            uint8_t* tmpBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
            if (tmpBuf) {
                if (ReadBlock(self, bgdtBlockIdx, tmpBuf)) {
                    memcpy(tmpBuf + bgdtOffInBlock, &self.bgdt[newGroup],
                           sizeof(BlockGroupDescriptor));
                    WriteBlock(self, bgdtBlockIdx, tmpBuf);
                }
                Memory::g_pfa->Free(tmpBuf);
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
    // Superblock validation and mount
    // =========================================================================

    Vfs::FsDriver* Mount(int blockDevIndex, uint64_t startLba, uint64_t sectorCount) {
        if (g_instanceCount >= MaxInstances) return nullptr;

        auto* dev = Drivers::Storage::GetBlockDevice(blockDevIndex);
        if (!dev) return nullptr;

        // ext2 superblock is at byte offset 1024 from partition start (sector 2)
        uint8_t sbBuf[1024];
        if (!dev->ReadSectors(dev->Ctx, startLba + 2, 2, sbBuf)) return nullptr;

        Superblock* sb = (Superblock*)sbBuf;

        // Validate magic number
        if (sb->s_magic != EXT2_MAGIC) return nullptr;

        // Validate basic fields
        if (sb->s_inodes_count == 0 || sb->s_blocks_count == 0) return nullptr;
        if (sb->s_inodes_per_group == 0 || sb->s_blocks_per_group == 0) return nullptr;

        uint32_t blockSize = 1024 << sb->s_log_block_size;
        if (blockSize < 1024 || blockSize > 65536) return nullptr;

        // Determine inode size
        uint16_t inodeSize = 128; // default for rev 0
        if (sb->s_rev_level >= 1) {
            inodeSize = sb->s_inode_size;
            if (inodeSize < 128 || inodeSize > blockSize) return nullptr;
        }

        // Check for incompatible features we don't support
        // Bit 1 = compression, bit 2 = filetype (we support), bit 3 = journal needed,
        // bit 4 = meta_bg
        uint32_t incompat = sb->s_feature_incompat;
        // We support filetype (0x02). Reject anything else.
        uint32_t unsupported = incompat & ~(uint32_t)0x02;
        if (unsupported) return nullptr;

        // Compute group count
        uint32_t groupCount = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                             / sb->s_blocks_per_group;
        if (groupCount == 0) return nullptr;

        // Success — initialize instance
        int idx = g_instanceCount;
        auto& inst = g_instances[idx];

        inst.active = true;
        inst.blockDevIndex = blockDevIndex;
        inst.partStartLba = startLba;
        inst.blockSize = blockSize;
        inst.inodeSize = inodeSize;
        inst.inodesPerGroup = sb->s_inodes_per_group;
        inst.blocksPerGroup = sb->s_blocks_per_group;
        inst.totalInodes = sb->s_inodes_count;
        inst.totalBlocks = sb->s_blocks_count;
        inst.firstDataBlock = sb->s_first_data_block;
        inst.groupCount = groupCount;

        // Volume label
        memcpy(inst.volumeLabel, sb->s_volume_name, 16);
        inst.volumeLabel[16] = '\0';
        // Trim trailing nulls/spaces
        for (int i = 15; i >= 0; i--) {
            if (inst.volumeLabel[i] == '\0' || inst.volumeLabel[i] == ' ')
                inst.volumeLabel[i] = '\0';
            else break;
        }

        // Allocate block buffer
        inst.blockBufPages = ((int)blockSize + 0xFFF) / 0x1000;
        if (inst.blockBufPages == 1) {
            inst.blockBuf = (uint8_t*)Memory::g_pfa->AllocateZeroed();
        } else {
            inst.blockBuf = (uint8_t*)Memory::g_pfa->ReallocConsecutive(
                nullptr, inst.blockBufPages);
        }
        if (!inst.blockBuf) {
            inst.active = false;
            return nullptr;
        }

        // Load block group descriptor table
        // BGDT starts at the block after the superblock
        uint32_t bgdtStartBlock = inst.firstDataBlock + 1;
        uint32_t bgdtBytes = groupCount * sizeof(BlockGroupDescriptor);
        inst.bgdtPages = ((int)bgdtBytes + 0xFFF) / 0x1000;

        if (inst.bgdtPages == 1) {
            inst.bgdt = (BlockGroupDescriptor*)Memory::g_pfa->AllocateZeroed();
        } else {
            inst.bgdt = (BlockGroupDescriptor*)Memory::g_pfa->ReallocConsecutive(
                nullptr, inst.bgdtPages);
        }

        if (!inst.bgdt) {
            inst.active = false;
            return nullptr;
        }

        // Read BGDT blocks
        uint32_t bgdtBlocks = (bgdtBytes + blockSize - 1) / blockSize;
        uint8_t* dst = (uint8_t*)inst.bgdt;
        for (uint32_t b = 0; b < bgdtBlocks; b++) {
            if (!ReadBlock(inst, bgdtStartBlock + b, inst.blockBuf)) {
                inst.active = false;
                return nullptr;
            }
            uint32_t copyLen = bgdtBytes - b * blockSize;
            if (copyLen > blockSize) copyLen = blockSize;
            memcpy(dst + b * blockSize, inst.blockBuf, copyLen);
        }

        // Clear file handles
        for (int i = 0; i < MaxFilesPerInstance; i++) {
            inst.files[i].inUse = false;
        }

        g_instanceCount++;

        KernelLogStream(OK, "Ext2") << "Mounted volume \""
            << inst.volumeLabel << "\" (" << inst.totalBlocks << " blocks, "
            << blockSize << " bytes/block, " << groupCount << " groups)";

        return &g_drivers[idx];
    }

    void RegisterProbe() {
        FsProbe::Register(Mount);
    }

    // =========================================================================
    // Format
    // =========================================================================

    int Format(int blockDevIndex, uint64_t startLba, uint64_t sectorCount,
               const char* volumeLabel) {
        using namespace Drivers::Storage;
        auto* dev = GetBlockDevice(blockDevIndex);
        if (!dev) return -1;

        if (sectorCount < 8192) {
            KernelLogStream(ERROR, "Ext2") << "Partition too small for ext2";
            return -1;
        }

        constexpr uint32_t blockSize = 4096;
        constexpr uint32_t logBlockSize = 2;          // 1024 << 2 = 4096
        constexpr uint32_t sectorsPerBlock = blockSize / 512;
        constexpr uint32_t inodeSize = 128;
        constexpr uint32_t inodesPerBlock = blockSize / inodeSize;  // 32
        constexpr uint32_t blocksPerGroup = 8 * blockSize;          // 32768
        constexpr uint32_t reservedInodeCount = 10;

        uint32_t totalBlocks = (uint32_t)(sectorCount / sectorsPerBlock);

        uint32_t groupCount = (totalBlocks + blocksPerGroup - 1) / blocksPerGroup;
        if (groupCount == 0) groupCount = 1;

        // Inodes per group: ~1 per 16K, rounded to fill inode table blocks
        uint32_t inodesPerGroup = blocksPerGroup / 4;  // 8192
        inodesPerGroup = (inodesPerGroup / inodesPerBlock) * inodesPerBlock;
        if (inodesPerGroup < inodesPerBlock) inodesPerGroup = inodesPerBlock;

        // Cap for small partitions
        uint32_t maxInodes = totalBlocks / 4;
        if (maxInodes < reservedInodeCount + inodesPerBlock)
            maxInodes = reservedInodeCount + inodesPerBlock;
        uint32_t maxIpg = ((maxInodes + groupCount - 1) / groupCount);
        maxIpg = ((maxIpg + inodesPerBlock - 1) / inodesPerBlock) * inodesPerBlock;
        if (inodesPerGroup > maxIpg) inodesPerGroup = maxIpg;

        uint32_t inodeTableBlocks = (inodesPerGroup * inodeSize) / blockSize;
        uint32_t totalInodes = inodesPerGroup * groupCount;

        // BGDT
        uint32_t bgdtBytes = groupCount * sizeof(BlockGroupDescriptor);
        uint32_t bgdtBlocks = (bgdtBytes + blockSize - 1) / blockSize;

        // Allocate a block-sized buffer (1 page = 4K = blockSize)
        uint8_t* buf = (uint8_t*)Memory::g_pfa->ReallocConsecutive(nullptr, 1);
        if (!buf) return -1;

        auto writeBlock = [&](uint32_t blockNum) -> bool {
            uint64_t sector = startLba + (uint64_t)blockNum * sectorsPerBlock;
            for (uint32_t s = 0; s < sectorsPerBlock; s++) {
                if (!dev->WriteSectors(dev->Ctx, sector + s, 1, buf + s * 512))
                    return false;
            }
            return true;
        };

        // ---- Superblock (block 0, at byte offset 1024) ----
        memset(buf, 0, blockSize);

        Superblock* sb = (Superblock*)(buf + 1024);
        sb->s_inodes_count = totalInodes;
        sb->s_blocks_count = totalBlocks;
        sb->s_r_blocks_count = totalBlocks / 20;

        // Calculate total used blocks
        uint32_t usedBlocks = 0;
        for (uint32_t g = 0; g < groupCount; g++) {
            uint32_t overhead = 2 + inodeTableBlocks; // bitmaps + itable
            if (g == 0) overhead += 1 + bgdtBlocks + 1; // sb + bgdt + root data
            usedBlocks += overhead;
        }
        sb->s_free_blocks_count = totalBlocks - usedBlocks;
        sb->s_free_inodes_count = totalInodes - reservedInodeCount;

        sb->s_first_data_block = 0; // 4K blocks
        sb->s_log_block_size = logBlockSize;
        sb->s_log_frag_size = logBlockSize;
        sb->s_blocks_per_group = blocksPerGroup;
        sb->s_frags_per_group = blocksPerGroup;
        sb->s_inodes_per_group = inodesPerGroup;
        sb->s_max_mnt_count = 20;
        sb->s_magic = EXT2_MAGIC;
        sb->s_state = 1;   // clean
        sb->s_errors = 1;  // continue
        sb->s_rev_level = 1;
        sb->s_first_ino = 11;
        sb->s_inode_size = inodeSize;
        sb->s_block_group_nr = 0;
        sb->s_feature_incompat = 0x0002; // FILETYPE

        // Generate UUID from RDTSC
        uint32_t lo, hi;
        asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint32_t seed = lo ^ hi;
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245 + 12345;
            sb->s_uuid[i] = (uint8_t)(seed >> 16);
        }

        memset(sb->s_volume_name, 0, 16);
        if (volumeLabel) {
            for (int i = 0; i < 16 && volumeLabel[i]; i++)
                sb->s_volume_name[i] = volumeLabel[i];
        }

        if (!writeBlock(0)) { Memory::g_pfa->Free(buf, 1); return -1; }

        // ---- BGDT (block 1..) ----
        for (uint32_t b = 0; b < bgdtBlocks; b++) {
            memset(buf, 0, blockSize);

            uint32_t entriesPerBlock = blockSize / sizeof(BlockGroupDescriptor);
            uint32_t startEntry = b * entriesPerBlock;

            for (uint32_t e = 0; e < entriesPerBlock && (startEntry + e) < groupCount; e++) {
                uint32_t g = startEntry + e;
                BlockGroupDescriptor* bgd = (BlockGroupDescriptor*)(buf + e * sizeof(BlockGroupDescriptor));

                uint32_t groupBase = g * blocksPerGroup;
                uint32_t metaStart = (g == 0) ? groupBase + 1 + bgdtBlocks : groupBase;

                bgd->bg_block_bitmap = metaStart;
                bgd->bg_inode_bitmap = metaStart + 1;
                bgd->bg_inode_table  = metaStart + 2;

                uint32_t groupBlocks = (g < groupCount - 1) ? blocksPerGroup
                    : (totalBlocks - g * blocksPerGroup);
                uint32_t overhead = 2 + inodeTableBlocks;
                if (g == 0) overhead += 1 + bgdtBlocks + 1; // sb+bgdt + root data
                bgd->bg_free_blocks_count = (uint16_t)(groupBlocks - overhead);

                if (g == 0) {
                    bgd->bg_free_inodes_count = (uint16_t)(inodesPerGroup - reservedInodeCount);
                    bgd->bg_used_dirs_count = 1;
                } else {
                    bgd->bg_free_inodes_count = (uint16_t)inodesPerGroup;
                    bgd->bg_used_dirs_count = 0;
                }
            }

            if (!writeBlock(1 + b)) { Memory::g_pfa->Free(buf, 1); return -1; }
        }

        // ---- Per-group bitmaps and inode tables ----
        for (uint32_t g = 0; g < groupCount; g++) {
            uint32_t groupBase = g * blocksPerGroup;
            uint32_t metaStart = (g == 0) ? groupBase + 1 + bgdtBlocks : groupBase;
            uint32_t groupBlocks = (g < groupCount - 1) ? blocksPerGroup
                : (totalBlocks - g * blocksPerGroup);

            // Block bitmap
            memset(buf, 0, blockSize);
            uint32_t overhead = 2 + inodeTableBlocks;
            if (g == 0) overhead += 1 + bgdtBlocks;
            for (uint32_t bit = 0; bit < overhead; bit++)
                buf[bit / 8] |= (1 << (bit % 8));
            if (g == 0) {
                // Root dir data block
                buf[overhead / 8] |= (1 << (overhead % 8));
            }
            // Mark blocks beyond group end as used (last group)
            for (uint32_t bit = groupBlocks; bit < blocksPerGroup; bit++)
                buf[bit / 8] |= (1 << (bit % 8));
            if (!writeBlock(metaStart)) { Memory::g_pfa->Free(buf, 1); return -1; }

            // Inode bitmap
            memset(buf, 0, blockSize);
            if (g == 0) {
                for (uint32_t bit = 0; bit < reservedInodeCount; bit++)
                    buf[bit / 8] |= (1 << (bit % 8));
            }
            if (!writeBlock(metaStart + 1)) { Memory::g_pfa->Free(buf, 1); return -1; }

            // Inode table
            for (uint32_t tb = 0; tb < inodeTableBlocks; tb++) {
                memset(buf, 0, blockSize);

                if (g == 0 && tb == 0) {
                    // Root directory inode (inode 2 = index 1)
                    uint32_t rootDataBlock = 1 + bgdtBlocks + 2 + inodeTableBlocks;
                    Inode* ri = (Inode*)(buf + 1 * inodeSize);
                    ri->i_mode = IMODE_DIR | 0x01FF;
                    ri->i_size = blockSize;
                    ri->i_links_count = 2;
                    ri->i_blocks = blockSize / 512;
                    ri->i_block[0] = rootDataBlock;
                }

                if (!writeBlock(metaStart + 2 + tb)) { Memory::g_pfa->Free(buf, 1); return -1; }
            }

            // Root directory data block (group 0 only)
            if (g == 0) {
                uint32_t rootDataBlock = 1 + bgdtBlocks + 2 + inodeTableBlocks;
                memset(buf, 0, blockSize);

                // "."
                DirEntry* dot = (DirEntry*)buf;
                dot->inode = EXT2_ROOT_INODE;
                dot->rec_len = 12;
                dot->name_len = 1;
                dot->file_type = EXT2_FT_DIR;
                buf[sizeof(DirEntry)] = '.';

                // ".."
                DirEntry* dotdot = (DirEntry*)(buf + 12);
                dotdot->inode = EXT2_ROOT_INODE;
                dotdot->rec_len = blockSize - 12;
                dotdot->name_len = 2;
                dotdot->file_type = EXT2_FT_DIR;
                buf[12 + sizeof(DirEntry)] = '.';
                buf[12 + sizeof(DirEntry) + 1] = '.';

                if (!writeBlock(rootDataBlock)) { Memory::g_pfa->Free(buf, 1); return -1; }
            }
        }

        Memory::g_pfa->Free(buf, 1);

        KernelLogStream(OK, "Ext2") << "Formatted: " << (uint64_t)totalBlocks
            << " blocks (4K), " << (uint64_t)groupCount
            << " groups, " << (uint64_t)totalInodes << " inodes";

        return 0;
    }

};
