/*
    * AcpiEvents.cpp
    * ACPI fixed event handling (SCI interrupt, power button, etc.)
    * Copyright (c) 2026 Daniel Hammer
*/

#include "AcpiEvents.hpp"
#include <ACPI/AcpiSleep.hpp>
#include <Io/IoPort.hpp>
#include <Hal/Apic/Apic.hpp>
#include <Hal/Apic/IoApic.hpp>
#include <Hal/Apic/Interrupts.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Hal {
    namespace AcpiEvents {

        // ============================================================================
        // State
        // ============================================================================
        static uint32_t g_pm1aEventBlock  = 0;
        static uint32_t g_pm1bEventBlock  = 0;
        static uint8_t  g_pm1EventLength  = 0;
        static uint16_t g_sciIrq          = 0;
        static bool     g_initialized     = false;

        // ============================================================================
        // PM1 register helpers
        // ============================================================================
        static uint16_t ReadPM1Status() {
            uint16_t sts = 0;
            if (g_pm1aEventBlock != 0)
                sts |= Io::In16((uint16_t)g_pm1aEventBlock);
            if (g_pm1bEventBlock != 0)
                sts |= Io::In16((uint16_t)g_pm1bEventBlock);
            return sts;
        }

        static void ClearPM1StatusBits(uint16_t bits) {
            // Write-1-to-clear semantics
            if (g_pm1aEventBlock != 0)
                Io::Out16(bits, (uint16_t)g_pm1aEventBlock);
            if (g_pm1bEventBlock != 0)
                Io::Out16(bits, (uint16_t)g_pm1bEventBlock);
        }

        static void SetPM1Events(uint16_t mask) {
            uint16_t enableOffset = g_pm1EventLength / 2;
            if (enableOffset == 0) return;

            if (g_pm1aEventBlock != 0) {
                Io::Out16(mask, (uint16_t)(g_pm1aEventBlock + enableOffset));
            }
            if (g_pm1bEventBlock != 0) {
                Io::Out16(mask, (uint16_t)(g_pm1bEventBlock + enableOffset));
            }
        }

        // ============================================================================
        // SCI Interrupt Handler
        // ============================================================================
        static void SciHandler(uint8_t irq) {
            uint16_t sts = ReadPM1Status();

            if (sts & AcpiSleep::PM1_PWRBTN_STS) {
                ClearPM1StatusBits(AcpiSleep::PM1_PWRBTN_STS);
                KernelLogStream(INFO, "ACPI") << "Power button pressed";
            }

            if (sts & AcpiSleep::PM1_SLPBTN_STS) {
                ClearPM1StatusBits(AcpiSleep::PM1_SLPBTN_STS);
                KernelLogStream(INFO, "ACPI") << "Sleep button pressed";
            }

            // Note: EOI is sent by HalIrqDispatch after this handler returns.
        }

        // ============================================================================

        // Initialize

        // ============================================================================
        void Initialize(ACPI::CommonSDTHeader* xsdt) {
            FADT::ParsedFADT fadt{};
            if (!FADT::Parse(xsdt, fadt) || !fadt.Valid) {
                KernelLogStream(ERROR, "ACPI") << "Failed to parse FADT - ACPI events unavailable";
                return;
            }

            g_pm1aEventBlock = fadt.PM1aEventBlock;
            g_pm1bEventBlock = fadt.PM1bEventBlock;
            g_pm1EventLength = fadt.PM1EventLength;
            g_sciIrq         = fadt.SCI_Interrupt;

            if (g_pm1aEventBlock == 0) {
                KernelLogStream(ERROR, "ACPI") << "No PM1a event block - ACPI events unavailable";
                return;
            }

            // Clear any pending status bits
            ClearPM1StatusBits(
                AcpiSleep::PM1_PWRBTN_STS | AcpiSleep::PM1_SLPBTN_STS |
                AcpiSleep::PM1_WAK_STS | AcpiSleep::PM1_TMR_STS |
                AcpiSleep::PM1_GBL_STS | AcpiSleep::PM1_RTC_STS |
                AcpiSleep::PM1_BM_STS);

            // Enable power button event
            SetPM1Events(AcpiSleep::PM1_PWRBTN_EN);

            // Route SCI to an IRQ vector. The SCI is level-triggered,
            // active-low (ACPI spec requirement). The MADT may have an
            // Interrupt Source Override for this IRQ that already sets
            // these flags; RouteIrq applies overrides automatically.
            uint8_t sciVector = IRQ_VECTOR_BASE + (uint8_t)g_sciIrq;
            uint8_t bspApicId = (uint8_t)LocalApic::GetId();

            // Register the handler before routing to avoid missing events
            RegisterIrqHandler((uint8_t)g_sciIrq, SciHandler);

            // Route via IoApic (applies MADT overrides for polarity/trigger)
            IoApic::RouteIrq((uint8_t)g_sciIrq, sciVector, bspApicId);

            g_initialized = true;

            KernelLogStream(OK, "ACPI") << "ACPI events initialized (SCI IRQ "
                << base::dec << (uint64_t)g_sciIrq << ", vector "
                << (uint64_t)sciVector << ")";
        }

        void Reinitialize() {
            if (!g_initialized) return;

            // Resume may leave WAK/RTC/GBL bits pending. Because the SCI is
            // level-triggered, re-enabling interrupts without clearing them can
            // immediately retrigger the SCI in a tight loop.
            ClearPM1StatusBits(
                AcpiSleep::PM1_PWRBTN_STS | AcpiSleep::PM1_SLPBTN_STS |
                AcpiSleep::PM1_WAK_STS | AcpiSleep::PM1_TMR_STS |
                AcpiSleep::PM1_GBL_STS | AcpiSleep::PM1_RTC_STS |
                AcpiSleep::PM1_BM_STS);

            // Drop the suspend-time RTC wake enable and restore only the
            // fixed events we actually want during normal runtime.
            SetPM1Events(AcpiSleep::PM1_PWRBTN_EN);
        }

    };
};
