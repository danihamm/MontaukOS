/*
    * ElfLoader.cpp
    * ELF64 binary loader for user-mode processes
    * Copyright (c) 2025 Daniel Hammer
*/

#include "ElfLoader.hpp"
#include <Fs/Vfs.hpp>
#include <Memory/Heap.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/Paging.hpp>
#include <Memory/HHDM.hpp>
#include <Libraries/Memory.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

namespace Sched {

    static bool ValidateElfHeader(const Elf64Header* hdr) {
        // Check ELF magic: 0x7f 'E' 'L' 'F'
        if (hdr->e_ident[0] != 0x7f ||
            hdr->e_ident[1] != 'E'  ||
            hdr->e_ident[2] != 'L'  ||
            hdr->e_ident[3] != 'F') {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Invalid ELF magic";
            return false;
        }

        // Class must be ELFCLASS64 (2)
        if (hdr->e_ident[4] != 2) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not a 64-bit ELF";
            return false;
        }

        // Data encoding must be ELFDATA2LSB (1) - little endian
        if (hdr->e_ident[5] != 1) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not little-endian";
            return false;
        }

        if (hdr->e_type != ET_EXEC) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not an executable (type=" << (uint64_t)hdr->e_type << ")";
            return false;
        }

        if (hdr->e_machine != EM_X86_64) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not x86_64 (machine=" << (uint64_t)hdr->e_machine << ")";
            return false;
        }

        return true;
    }

    uint64_t ElfLoad(const char* vfsPath, uint64_t pml4Phys) {
        Fs::Vfs::BackendFile file = {-1, -1};
        if (Fs::Vfs::OpenBackendFile(vfsPath, file) < 0) {
            return 0;
        }

        uint64_t fileSize = Fs::Vfs::GetBackendFileSize(file);
        if (fileSize < sizeof(Elf64Header)) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "File too small (" << fileSize << " bytes)";
            Fs::Vfs::CloseBackendFile(file);
            return 0;
        }

        // Read entire file into a heap buffer
        uint8_t* fileData = (uint8_t*)Memory::g_heap->Request(fileSize);
        if (fileData == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Failed to allocate " << fileSize << " bytes for file";
            Fs::Vfs::CloseBackendFile(file);
            return 0;
        }

        Fs::Vfs::ReadBackendFile(file, fileData, 0, fileSize);
        Fs::Vfs::CloseBackendFile(file);

        // Prevent the optimizer from reordering the VfsRead store past the
        // header validation reads that follow.
        asm volatile("" ::: "memory");

        // Validate ELF header
        Elf64Header* hdr = (Elf64Header*)fileData;
        if (!ValidateElfHeader(hdr)) {
            Memory::g_heap->Free(fileData);
            return 0;
        }

        // Process program headers
        for (uint16_t i = 0; i < hdr->e_phnum; i++) {
            Elf64ProgramHeader* phdr = (Elf64ProgramHeader*)(fileData + hdr->e_phoff + i * hdr->e_phentsize);

            if (phdr->p_type != PT_LOAD) {
                continue;
            }

            if (phdr->p_memsz == 0) {
                continue;
            }

            // Allocate pages and map them in the process PML4 with User bit
            uint64_t segBase = phdr->p_vaddr & ~0xFFFULL;
            uint64_t segEnd = (phdr->p_vaddr + phdr->p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t numPages = (segEnd - segBase) / 0x1000;

            for (uint64_t p = 0; p < numPages; p++) {
                void* page = Memory::g_pfa->AllocateZeroed();
                if (page == nullptr) {
                    Kt::KernelLogStream(Kt::ERROR, "ELF") << "Out of physical pages";
                    Memory::g_heap->Free(fileData);
                    return 0;
                }

                uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
                uint64_t virtAddr = segBase + p * 0x1000;

                // Map into the process's PML4 with User bit set
                if (!Memory::VMM::Paging::MapUserIn(pml4Phys, physAddr, virtAddr)) {
                    Kt::KernelLogStream(Kt::ERROR, "ELF") << "Failed to map page";
                    Memory::g_heap->Free(fileData);
                    return 0;
                }

                // Copy file data that overlaps this page (via HHDM)
                uint64_t pageStart = virtAddr;
                uint64_t pageEnd = virtAddr + 0x1000;

                uint64_t segFileStart = phdr->p_vaddr;
                uint64_t segFileEnd = phdr->p_vaddr + phdr->p_filesz;

                uint64_t copyStart = (pageStart > segFileStart) ? pageStart : segFileStart;
                uint64_t copyEnd = (pageEnd < segFileEnd) ? pageEnd : segFileEnd;

                if (copyStart < copyEnd) {
                    uint64_t dstOffset = copyStart - pageStart;
                    uint64_t srcOffset = copyStart - phdr->p_vaddr + phdr->p_offset;
                    uint64_t copySize = copyEnd - copyStart;

                    uint8_t* dst = (uint8_t*)Memory::HHDM(physAddr) + dstOffset;
                    uint8_t* src = fileData + srcOffset;
                    memcpy(dst, src, copySize);
                }
            }
        }

        uint64_t entryPoint = hdr->e_entry;
        Memory::g_heap->Free(fileData);

        return entryPoint;
    }

    uint64_t ElfLoadLib(const char* vfsPath, uint64_t pml4Phys, int slot) {
        Fs::Vfs::BackendFile file = {-1, -1};
        if (Fs::Vfs::OpenBackendFile(vfsPath, file) < 0) {
            return 0;
        }

        uint64_t fileSize = Fs::Vfs::GetBackendFileSize(file);
        if (fileSize < sizeof(Elf64Header)) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "File too small (" << fileSize << " bytes)";
            Fs::Vfs::CloseBackendFile(file);
            return 0;
        }

        // Read entire file into a heap buffer
        uint8_t* fileData = (uint8_t*)Memory::g_heap->Request(fileSize);
        if (fileData == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Failed to allocate " << fileSize << " bytes for file";
            Fs::Vfs::CloseBackendFile(file);
            return 0;
        }

        Fs::Vfs::ReadBackendFile(file, fileData, 0, fileSize);
        Fs::Vfs::CloseBackendFile(file);

        asm volatile("" ::: "memory");

        Elf64Header* hdr = (Elf64Header*)fileData;

        // Validate ELF header
        if (hdr->e_ident[0] != 0x7f ||
            hdr->e_ident[1] != 'E'  ||
            hdr->e_ident[2] != 'L'  ||
            hdr->e_ident[3] != 'F') {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Invalid ELF magic";
            Memory::g_heap->Free(fileData);
            return 0;
        }

        if (hdr->e_ident[4] != 2) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not a 64-bit ELF";
            Memory::g_heap->Free(fileData);
            return 0;
        }

        if (hdr->e_ident[5] != 1) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not little-endian";
            Memory::g_heap->Free(fileData);
            return 0;
        }

        // Libraries are built as ET_EXEC at a fixed base (0x60000000)
        if (hdr->e_type != ET_EXEC) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Library not ET_EXEC (type=" << (uint64_t)hdr->e_type << ")";
            Memory::g_heap->Free(fileData);
            return 0;
        }

        if (hdr->e_machine != EM_X86_64) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Not x86_64 (machine=" << (uint64_t)hdr->e_machine << ")";
            Memory::g_heap->Free(fileData);
            return 0;
        }

        // Calculate library base for this slot (each slot gets LIB_MAX_SIZE region)
        uint64_t libBase = LIB_BASE + (uint64_t(slot) * LIB_MAX_SIZE);

        // Process program headers
        for (uint16_t i = 0; i < hdr->e_phnum; i++) {
            Elf64ProgramHeader* phdr = (Elf64ProgramHeader*)(fileData + hdr->e_phoff + i * hdr->e_phentsize);

            if (phdr->p_type != PT_LOAD) {
                continue;
            }

            if (phdr->p_memsz == 0) {
                continue;
            }

            // The library is linked at 0x400000. We need to relocate it to libBase.
            // target_vaddr = libBase + (file_vaddr - 0x400000)
            uint64_t targetSegStart = libBase + (phdr->p_vaddr - 0x400000ULL);
            uint64_t targetSegFileEnd = targetSegStart + phdr->p_filesz;  // only file data
            uint64_t targetSegMemEnd = targetSegStart + phdr->p_memsz;    // includes bss

            // Align segment start down, segment end up to page boundaries
            uint64_t pageSegStart = targetSegStart & ~0xFFFULL;
            uint64_t pageSegEnd = (targetSegMemEnd + 0xFFFULL) & ~0xFFFULL;
            uint64_t numPages = (pageSegEnd - pageSegStart) / 0x1000;

            for (uint64_t p = 0; p < numPages; p++) {
                void* page = Memory::g_pfa->AllocateZeroed();
                if (page == nullptr) {
                    Kt::KernelLogStream(Kt::ERROR, "ELF") << "Out of physical pages";
                    Memory::g_heap->Free(fileData);
                    return 0;
                }

                uint64_t physAddr = Memory::SubHHDM((uint64_t)page);
                uint64_t pageStart = pageSegStart + p * 0x1000;
                uint64_t pageEnd = pageStart + 0x1000;

                // Check that we're within the library region
                if (pageStart < libBase || pageEnd > libBase + LIB_MAX_SIZE) {
                    Kt::KernelLogStream(Kt::ERROR, "ELF") << "Segment outside library region";
                    Memory::g_heap->Free(fileData);
                    return 0;
                }

                if (!Memory::VMM::Paging::MapUserIn(pml4Phys, physAddr, pageStart)) {
                    Kt::KernelLogStream(Kt::ERROR, "ELF") << "Failed to map page";
                    Memory::g_heap->Free(fileData);
                    return 0;
                }

                // Calculate overlap between this page and the file-data portion of the segment
                uint64_t copyStart = (pageStart > targetSegStart) ? pageStart : targetSegStart;
                uint64_t copyEnd = (pageEnd < targetSegFileEnd) ? pageEnd : targetSegFileEnd;

                if (copyStart < copyEnd) {
                    // Source: file data at (copyStart - targetSegStart) offset from p_offset
                    uint64_t srcOffset = phdr->p_offset + (copyStart - targetSegStart);
                    // Dest: page at (copyStart - targetSegStart) offset, then adjusted for this page
                    uint64_t pageDataOffset = copyStart - targetSegStart;
                    uint64_t copySize = copyEnd - copyStart;

                    uint8_t* dst = (uint8_t*)Memory::HHDM(physAddr) + pageDataOffset;
                    uint8_t* src = fileData + srcOffset;
                    memcpy(dst, src, copySize);
                }
            }
        }

        Memory::g_heap->Free(fileData);

        // Return the library base address for this slot
        return libBase;
    }

}
