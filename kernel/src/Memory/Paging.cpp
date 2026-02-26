#include "Paging.hpp"
#include <Memory/PageFrameAllocator.hpp>
#include <Common/Panic.hpp>
#include <Memory/HHDM.hpp>

namespace Memory::VMM {
    Paging* g_paging = nullptr;

    extern "C" uint64_t KernelStartSymbol;

    std::uint64_t GetPhysKernelAddress(std::uint64_t virtualAddress) {
        return Paging::GetPhysAddr(GetCR3(), (std::uint64_t)virtualAddress, true);
    }

    Paging::Paging() {
        PML4 = (PageTable*)SubHHDM((PageTable*)Memory::g_pfa->AllocateZeroed());
    }

    void Paging::Init(std::uint64_t kernelBaseVirt, std::uint64_t kernelSize, limine_memmap_response* memMap) {
        // Map kernel
        Kt::KernelLogStream(Kt::DEBUG, "VMM") << "Paging::Init called with kernelBaseVirt as 0x" << base::hex << kernelBaseVirt;

        for (std::uint64_t pageAddr = kernelBaseVirt; pageAddr < (kernelBaseVirt + kernelSize); pageAddr += 0x1000) {
            Map(GetPhysKernelAddress(pageAddr), pageAddr);
        }

        // Map HHDM memMap entries

        for (size_t i = 0; i < memMap->entry_count; i++) {
            auto entry = memMap->entries[i];

            for (size_t pageAddr = entry->base; pageAddr < (entry->base + entry->length); pageAddr += 0x1000) {
                Map(pageAddr, HHDM(pageAddr));
            }
        }

        LoadCR3(PML4);
        Kt::KernelLogStream(Kt::OK, "VMM") << "Switched CR3";
    }

    PageTable* Paging::HandleLevel(VirtualAddress virtualAddress, PageTable* table, const size_t level) {
        PageTableEntry* entry = (PageTableEntry*)Memory::HHDM(&table->entries[virtualAddress.GetIndex(level)]);

        if (!entry->Present) {
            entry->Present = true;
            entry->Writable = true;

            uint64_t downLevelAddr = Memory::SubHHDM((uint64_t)Memory::g_pfa->AllocateZeroed());

            entry->Address = downLevelAddr >> 12;

            return (PageTable*)downLevelAddr;
        } else {
            return (PageTable*)(entry->Address << 12);
        }
    }

    PageTable* Paging::HandleLevelUser(VirtualAddress virtualAddress, PageTable* table, const size_t level) {
        PageTableEntry* entry = (PageTableEntry*)Memory::HHDM(&table->entries[virtualAddress.GetIndex(level)]);

        if (!entry->Present) {
            entry->Present = true;
            entry->Writable = true;
            entry->Supervisor = 1;  // User-accessible

            uint64_t downLevelAddr = Memory::SubHHDM((uint64_t)Memory::g_pfa->AllocateZeroed());

            entry->Address = downLevelAddr >> 12;

            return (PageTable*)downLevelAddr;
        } else {
            // Ensure User bit is set on existing entries in the user path
            entry->Supervisor = 1;
            return (PageTable*)(entry->Address << 12);
        }
    }

    void Paging::Map(std::uint64_t physicalAddress, std::uint64_t virtualAddress) {
        if (virtualAddress % 0x1000 != 0 || physicalAddress % 0x1000 != 0) {
            Panic("Value that isn't page-aligned passed as address to Paging::Map!", nullptr);
        }

        VirtualAddress virtualAddressObj(virtualAddress);

        auto PML3 = HandleLevel(virtualAddressObj, PML4, 4);
        auto PML2 = HandleLevel(virtualAddressObj, PML3, 3);
        auto PML1 = HandleLevel(virtualAddressObj, PML2, 2);

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&PML1->entries[virtualAddressObj.GetPageIndex()]);

        pageEntry->Present = true;
        pageEntry->Writable = true;

        pageEntry->Address = physicalAddress >> 12;
    }

    void Paging::MapWC(std::uint64_t physicalAddress, std::uint64_t virtualAddress) {
        if (virtualAddress % 0x1000 != 0 || physicalAddress % 0x1000 != 0) {
            Panic("Value that isn't page-aligned passed as address to Paging::MapWC!", nullptr);
        }

        VirtualAddress virtualAddressObj(virtualAddress);

        auto PML3 = HandleLevel(virtualAddressObj, PML4, 4);
        auto PML2 = HandleLevel(virtualAddressObj, PML3, 3);
        auto PML1 = HandleLevel(virtualAddressObj, PML2, 2);

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&PML1->entries[virtualAddressObj.GetPageIndex()]);

        pageEntry->Present = true;
        pageEntry->Writable = true;
        pageEntry->WriteThrough = true;   // PWT=1, PCD=0 → PAT entry 1 = WC

        pageEntry->Address = physicalAddress >> 12;
    }

    void Paging::MapMMIO(std::uint64_t physicalAddress, std::uint64_t virtualAddress) {
        if (virtualAddress % 0x1000 != 0 || physicalAddress % 0x1000 != 0) {
            Panic("Value that isn't page-aligned passed as address to Paging::MapMMIO!", nullptr);
        }

        VirtualAddress virtualAddressObj(virtualAddress);

        auto PML3 = HandleLevel(virtualAddressObj, PML4, 4);
        auto PML2 = HandleLevel(virtualAddressObj, PML3, 3);
        auto PML1 = HandleLevel(virtualAddressObj, PML2, 2);

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&PML1->entries[virtualAddressObj.GetPageIndex()]);

        pageEntry->Present = true;
        pageEntry->Writable = true;
        pageEntry->CacheDisabled = true;
        pageEntry->WriteThrough = true;

        pageEntry->Address = physicalAddress >> 12;
    }

    void Paging::MapUser(std::uint64_t physicalAddress, std::uint64_t virtualAddress) {
        if (virtualAddress % 0x1000 != 0 || physicalAddress % 0x1000 != 0) {
            Panic("Value that isn't page-aligned passed as address to Paging::MapUser!", nullptr);
        }

        VirtualAddress virtualAddressObj(virtualAddress);

        auto PML3 = HandleLevelUser(virtualAddressObj, PML4, 4);
        auto PML2 = HandleLevelUser(virtualAddressObj, PML3, 3);
        auto PML1 = HandleLevelUser(virtualAddressObj, PML2, 2);

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&PML1->entries[virtualAddressObj.GetPageIndex()]);

        pageEntry->Present = true;
        pageEntry->Writable = true;
        pageEntry->Supervisor = 1;  // User-accessible

        pageEntry->Address = physicalAddress >> 12;
    }

    std::uint64_t Paging::CreateUserPML4() {
        // Allocate a new PML4
        void* newPage = Memory::g_pfa->AllocateZeroed();
        uint64_t newPml4Phys = Memory::SubHHDM((uint64_t)newPage);
        PageTable* newPml4 = (PageTable*)newPage;  // HHDM virtual address

        // Copy kernel-half entries (256-511) from the global PML4
        PageTable* kernelPml4 = (PageTable*)Memory::HHDM((uint64_t)g_paging->PML4);
        for (int i = 256; i < 512; i++) {
            newPml4->entries[i] = kernelPml4->entries[i];
        }

        return newPml4Phys;
    }

    void Paging::MapUserIn(std::uint64_t pml4Phys, std::uint64_t physicalAddress, std::uint64_t virtualAddress) {
        if (virtualAddress % 0x1000 != 0 || physicalAddress % 0x1000 != 0) {
            Panic("Non-aligned address in Paging::MapUserIn!", nullptr);
        }

        VirtualAddress va(virtualAddress);

        // Walk/create page tables from the given PML4, setting User bit at each level
        auto walkLevel = [](PageTable* table, uint64_t index) -> PageTable* {
            PageTableEntry* entry = (PageTableEntry*)Memory::HHDM(&table->entries[index]);
            if (!entry->Present) {
                entry->Present = true;
                entry->Writable = true;
                entry->Supervisor = 1;  // User-accessible
                uint64_t newPhys = Memory::SubHHDM((uint64_t)Memory::g_pfa->AllocateZeroed());
                entry->Address = newPhys >> 12;
                return (PageTable*)newPhys;
            } else {
                entry->Supervisor = 1;
                return (PageTable*)(entry->Address << 12);
            }
        };

        PageTable* pml4 = (PageTable*)pml4Phys;
        auto pml3 = walkLevel(pml4, va.GetL4Index());
        auto pml2 = walkLevel(pml3, va.GetL3Index());
        auto pml1 = walkLevel(pml2, va.GetL2Index());

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&pml1->entries[va.GetPageIndex()]);
        pageEntry->Present = true;
        pageEntry->Writable = true;
        pageEntry->Supervisor = 1;
        pageEntry->Address = physicalAddress >> 12;
    }

    void Paging::MapUserInWC(std::uint64_t pml4Phys, std::uint64_t physicalAddress, std::uint64_t virtualAddress) {
        if (virtualAddress % 0x1000 != 0 || physicalAddress % 0x1000 != 0) {
            Panic("Non-aligned address in Paging::MapUserInWC!", nullptr);
        }

        VirtualAddress va(virtualAddress);

        auto walkLevel = [](PageTable* table, uint64_t index) -> PageTable* {
            PageTableEntry* entry = (PageTableEntry*)Memory::HHDM(&table->entries[index]);
            if (!entry->Present) {
                entry->Present = true;
                entry->Writable = true;
                entry->Supervisor = 1;
                uint64_t newPhys = Memory::SubHHDM((uint64_t)Memory::g_pfa->AllocateZeroed());
                entry->Address = newPhys >> 12;
                return (PageTable*)newPhys;
            } else {
                entry->Supervisor = 1;
                return (PageTable*)(entry->Address << 12);
            }
        };

        PageTable* pml4 = (PageTable*)pml4Phys;
        auto pml3 = walkLevel(pml4, va.GetL4Index());
        auto pml2 = walkLevel(pml3, va.GetL3Index());
        auto pml1 = walkLevel(pml2, va.GetL2Index());

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&pml1->entries[va.GetPageIndex()]);
        pageEntry->Present = true;
        pageEntry->Writable = true;
        pageEntry->Supervisor = 1;
        pageEntry->WriteThrough = true;   // PWT=1, PCD=0 → PAT entry 1 = WC
        pageEntry->Address = physicalAddress >> 12;
    }

    void Paging::UnmapUserIn(std::uint64_t pml4Phys, std::uint64_t virtualAddress) {
        VirtualAddress va(virtualAddress);

        // Walk without allocating — if any level is absent, nothing is mapped.
        auto walkRead = [](PageTable* table, uint64_t index) -> PageTable* {
            PageTableEntry* entry = (PageTableEntry*)Memory::HHDM(&table->entries[index]);
            if (!entry->Present) return nullptr;
            return (PageTable*)(entry->Address << 12);
        };

        PageTable* pml4 = (PageTable*)pml4Phys;
        auto pml3 = walkRead(pml4, va.GetL4Index());
        if (!pml3) return;
        auto pml2 = walkRead(pml3, va.GetL3Index());
        if (!pml2) return;
        auto pml1 = walkRead(pml2, va.GetL2Index());
        if (!pml1) return;

        PageTableEntry* pageEntry = (PageTableEntry*)Memory::HHDM(&pml1->entries[va.GetPageIndex()]);
        if (!pageEntry->Present) return;

        // Clear the entire 8-byte PTE
        *(uint64_t*)pageEntry = 0;

        // Invalidate TLB for this virtual address
        asm volatile("invlpg (%0)" :: "r"(virtualAddress) : "memory");
    }

    std::uint64_t Paging::GetPhysAddr(std::uint64_t pml4, std::uint64_t virtualAddress, bool use40BitL1) {
        VirtualAddress virtualAddressObj(virtualAddress);

        PageTable* pml4Virt = (PageTable*)HHDM(pml4);

        PageTableEntry* pml4_entry = &pml4Virt->entries[virtualAddressObj.GetL4Index()];

        PageTable* pml3 = (PageTable*)HHDM(pml4_entry->Address << 12);
        PageTableEntry* pml3_entry = &pml3->entries[virtualAddressObj.GetL3Index()];

        PageTable* pml2 = (PageTable*)HHDM(pml3_entry->Address << 12);
        PageTableEntry* pml2_entry = &pml2->entries[virtualAddressObj.GetL2Index()];

        PageTable* pml1 = (PageTable*)HHDM(pml2_entry->Address << 12);

        if (use40BitL1 == true) {
            PageTableEntry40Bit* pml1_entry = (PageTableEntry40Bit*)&pml1->entries[virtualAddressObj.GetPageIndex()];
            return (uint64_t)pml1_entry->Address << 12;
        }

        PageTableEntry* pml1_entry = &pml1->entries[virtualAddressObj.GetPageIndex()];
        return (uint64_t)pml1_entry->Address << 12;
    }

    std::uint64_t Paging::GetPhysAddr(std::uint64_t virtualAddress) {
        return GetPhysAddr((std::uint64_t)PML4, virtualAddress, false);
    }

    void Paging::MapEfiRuntime(limine_efi_memmap_response* efiMemmap) {
        if (!efiMemmap) return;

        auto* base = (uint8_t*)efiMemmap->memmap;
        uint64_t descSize = efiMemmap->desc_size;
        uint64_t count = efiMemmap->memmap_size / descSize;

        struct EfiMemDesc {
            uint32_t Type;
            uint64_t PhysicalStart;
            uint64_t VirtualStart;
            uint64_t NumberOfPages;
            uint64_t Attribute;
        };

        static constexpr uint64_t EFI_MEMORY_RUNTIME = 0x8000000000000000ULL;

        for (uint64_t i = 0; i < count; i++) {
            auto* desc = (EfiMemDesc*)(base + i * descSize);
            if (desc->Attribute & EFI_MEMORY_RUNTIME) {
                for (uint64_t p = 0; p < desc->NumberOfPages; p++) {
                    uint64_t addr = desc->PhysicalStart + p * 0x1000;
                    Map(addr, addr);
                }
            }
        }

        FlushTLB();
    }
};
