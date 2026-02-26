#pragma once
#include <limine.h>
#include <cstdint>
#include <Terminal/Terminal.hpp>

namespace Memory::VMM {
    struct PageTableEntry {
        std::uint8_t Present : 1;
        std::uint8_t Writable : 1;
        std::uint8_t Supervisor : 1;
        std::uint8_t WriteThrough : 1;
        std::uint8_t CacheDisabled : 1;
        std::uint8_t Accessed : 1;
        std::uint8_t Ignore : 1;
        std::uint8_t LargerPages : 1;
        std::uint8_t PageSize : 1;
        std::uint8_t Available : 3;
        std::uint64_t Address : 52;
    };

    struct PageTableEntry40Bit {
        std::uint8_t Present : 1;
        std::uint8_t Writable : 1;
        std::uint8_t Supervisor : 1;
        std::uint8_t WriteThrough : 1;
        std::uint8_t CacheDisabled : 1;
        std::uint8_t Accessed : 1;
        std::uint8_t Ignore : 1;
        std::uint8_t LargerPages : 1;
        std::uint8_t PageSize : 1;
        std::uint8_t Available : 3;
        std::uint64_t Address : 40;
        std::uint8_t AvailableHigh : 7;
        std::uint8_t PK : 4;
        std::uint8_t NX : 1;
    };


    struct PageTable {
        PageTableEntry entries[512];
    } __attribute__((packed)) __attribute__((aligned(0x1000)));

    struct VirtualAddress {
        std::uint64_t address;

        VirtualAddress(std::uint64_t newAddress) {
            if (newAddress % 0x1000 != 0) {
                Kt::KernelLogStream(Kt::WARNING, "VMM") << "VirtualAddress object created with non-aligned value.";
            }

            address = newAddress;
        }

        uint64_t GetL4Index() {
            return (address >> 39) & 0x1ff;
        }

        uint64_t GetL3Index() {
            return (address >> 30) & 0x1ff;
        }

        uint64_t GetL2Index() {
            return (address >> 21) & 0x1ff;
        }

        uint64_t GetPageIndex() {
            return (address >> 12) & 0x1ff;
        }

        uint64_t GetIndex(size_t level) {
            if (level == 4)
                return GetL4Index();

            else if (level == 3)
                return GetL3Index();

            else if (level == 2)
                return GetL2Index();

            else if (level == 1)
                return GetPageIndex();

            return 0;
        }
    };

    class Paging {
        PageTable* HandleLevel(VirtualAddress virtualAddress, PageTable* table, size_t level);
        PageTable* HandleLevelUser(VirtualAddress virtualAddress, PageTable* table, size_t level);
public:
        PageTable* PML4{};

        Paging();
        void Init(std::uint64_t kernelBaseVirt, std::uint64_t kernelSize, limine_memmap_response* memMap);
        void Map(std::uint64_t physicalAddress, std::uint64_t virtualAddress);
        void MapMMIO(std::uint64_t physicalAddress, std::uint64_t virtualAddress);
        void MapWC(std::uint64_t physicalAddress, std::uint64_t virtualAddress);
        void MapUser(std::uint64_t physicalAddress, std::uint64_t virtualAddress);
        static std::uint64_t GetPhysAddr(std::uint64_t PML4, std::uint64_t virtualAddress, bool use40BitL1 = false);
        std::uint64_t GetPhysAddr(std::uint64_t virtualAddress);

        // Create a new PML4 with kernel-half (entries 256-511) copied from g_paging.
        // Returns the physical address of the new PML4.
        static std::uint64_t CreateUserPML4();

        // Map a page into an arbitrary PML4 (specified by physical address) with User bit set.
        static void MapUserIn(std::uint64_t pml4Phys, std::uint64_t physicalAddress, std::uint64_t virtualAddress);

        // Map a page into an arbitrary PML4 with User + Write-Combining attributes.
        static void MapUserInWC(std::uint64_t pml4Phys, std::uint64_t physicalAddress, std::uint64_t virtualAddress);

        // Unmap a single page from an arbitrary PML4 (clears PTE + invalidates TLB).
        static void UnmapUserIn(std::uint64_t pml4Phys, std::uint64_t virtualAddress);

        // Identity-map EFI runtime service regions so firmware code can
        // reference its own data at physical addresses.
        void MapEfiRuntime(limine_efi_memmap_response* efiMemmap);
    };

    extern Paging* g_paging;

    extern "C" uint64_t GetCR3();
    extern "C" void LoadCR3(PageTable* PML4);

    inline void FlushTLB() {
        asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    }
};
