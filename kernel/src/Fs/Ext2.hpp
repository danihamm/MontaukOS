/*
    * Ext2.hpp
    * ext2 filesystem driver
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Fs/Vfs.hpp>

namespace Fs::Ext2 {

    // Try to mount an ext2 filesystem at the given partition range.
    // Returns a FsDriver* on success, nullptr if not a valid ext2 volume.
    Vfs::FsDriver* Mount(int blockDevIndex, uint64_t startLba, uint64_t sectorCount);

    // Register Ext2::Mount as a filesystem probe with FsProbe.
    void RegisterProbe();

    // Format a partition as ext2.
    // Returns 0 on success, -1 on error.
    int Format(int blockDevIndex, uint64_t startLba, uint64_t sectorCount,
               const char* volumeLabel);

};
