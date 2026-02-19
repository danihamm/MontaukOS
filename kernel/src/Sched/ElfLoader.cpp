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
        int handle = Fs::Vfs::VfsOpen(vfsPath);
        if (handle < 0) {
            return 0;
        }

        uint64_t fileSize = Fs::Vfs::VfsGetSize(handle);
        if (fileSize < sizeof(Elf64Header)) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "File too small (" << fileSize << " bytes)";
            Fs::Vfs::VfsClose(handle);
            return 0;
        }

        // Read entire file into a heap buffer
        uint8_t* fileData = (uint8_t*)Memory::g_heap->Request(fileSize);
        if (fileData == nullptr) {
            Kt::KernelLogStream(Kt::ERROR, "ELF") << "Failed to allocate " << fileSize << " bytes for file";
            Fs::Vfs::VfsClose(handle);
            return 0;
        }

        Fs::Vfs::VfsRead(handle, fileData, 0, fileSize);
        Fs::Vfs::VfsClose(handle);

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
                Memory::VMM::Paging::MapUserIn(pml4Phys, physAddr, virtAddr);

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

}
