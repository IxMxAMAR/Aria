// efi/ept_init.c — Build initial EPT tables
#include "ept_init.h"
#include "serial.h"
#include "../shared/protocol.h"

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

static void* ept_alloc_page(uint64_t* phys_out) {
    EFI_PHYSICAL_ADDRESS pa;
    EFI_STATUS status = gBS->AllocatePages(
        AllocateAnyPages, EfiRuntimeServicesData, 1, &pa);
    if (EFI_ERROR(status)) return NULL;

    void* va = (void*)pa;
    ZeroMem(va, PAGE_SIZE);
    if (phys_out) *phys_out = pa;
    return va;
}

// Determine EPT memory type for a 2MB page using MTRR registers.
// MMIO regions must be UC(0), RAM should be WB(6).
// Quick approach: read variable MTRRs to find UC/WC ranges,
// default to WB for everything else (like SimpleVisor does).
static uint8_t ept_mtrr_types[512]; // cache: type per 2MB in first 1GB
static uint64_t mtrr_phys_base[10], mtrr_phys_mask[10];
static int mtrr_count = 0;
static uint8_t mtrr_default_type = 6; // WB

static void ept_read_mtrrs(void) {
    extern uint64_t __readmsr(uint32_t);
    uint64_t def = __readmsr(0x2FF); // IA32_MTRR_DEF_TYPE
    mtrr_default_type = (uint8_t)(def & 0xFF);

    // Read variable MTRRs (IA32_MTRRCAP tells how many)
    uint64_t cap = __readmsr(0xFE); // IA32_MTRRCAP
    int vcnt = (int)(cap & 0xFF);
    if (vcnt > 10) vcnt = 10;
    mtrr_count = vcnt;
    for (int i = 0; i < vcnt; i++) {
        mtrr_phys_base[i] = __readmsr(0x200 + i * 2);
        mtrr_phys_mask[i] = __readmsr(0x201 + i * 2);
    }
}

static uint8_t ept_get_memory_type(uint64_t phys) {
    // Check each variable MTRR
    uint8_t result_type = 0xFF; // unset
    for (int i = 0; i < mtrr_count; i++) {
        if (!(mtrr_phys_mask[i] & (1ULL << 11))) continue; // not valid
        uint64_t mask = mtrr_phys_mask[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t base = mtrr_phys_base[i] & 0x000FFFFFFFFFF000ULL;
        uint8_t type  = (uint8_t)(mtrr_phys_base[i] & 0xFF);
        if ((phys & mask) == (base & mask)) {
            // MTRR hit — UC always wins, otherwise use lowest type
            if (type == 0) return 0; // UC trumps everything
            if (result_type == 0xFF || type < result_type)
                result_type = type;
        }
    }
    return (result_type != 0xFF) ? result_type : mtrr_default_type;
}

// MTRR-derived memory typing. A variable MTRR with type WB yields WB; a UC (or
// any other) variable MTRR yields UC; and an address covered by no variable
// MTRR takes the MTRR default type. The default is the important case: firmware
// normally sets it to WB and leaves system RAM uncovered, using the variable
// registers only to mark MMIO holes UC. Treating "uncovered" as UC marks every
// byte of RAM uncached.
static int mtrr_is_wb(uint64_t phys) {
    // Check if this address is in a variable MTRR with WB type
    for (int i = 0; i < mtrr_count; i++) {
        if (!(mtrr_phys_mask[i] & (1ULL << 11))) continue; // not valid
        uint64_t mask = mtrr_phys_mask[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t base = mtrr_phys_base[i] & 0x000FFFFFFFFFF000ULL;
        uint8_t type  = (uint8_t)(mtrr_phys_base[i] & 0xFF);
        if ((phys & mask) == (base & mask)) {
            if (type == 0) return 0; // UC MTRR — definitely not WB
            if (type == 6) return 1; // WB MTRR — safe for WB
            return 0; // any other type (WC, WT, WP) — use UC to be safe
        }
    }
    // No variable MTRR covers this address, so the MTRR default type applies
    // (SDM Vol 3 11.11.2.1). On real firmware the default is almost always WB,
    // and system RAM is deliberately *not* covered by a variable MTRR: the
    // variable registers exist to carve out UC MMIO holes. Returning UC here
    // marked every byte of RAM uncached, which is why the guest crawled and why
    // UEFI services (USB, GOP) appeared to hang.
    return (mtrr_default_type == 6) ? 1 : 0;
}

int ept_build_identity_map_minimal(VMM_STATE* state) {
    state->ept_pml4 = (EPT_ENTRY*)ept_alloc_page(&state->ept_pml4_pa);
    if (!state->ept_pml4) return -1;

    uint64_t pdpt_pa;
    uint64_t* pdpt = (uint64_t*)ept_alloc_page(&pdpt_pa);
    if (!pdpt) return -1;

    // PML4[0] → PDPT
    ((uint64_t*)state->ept_pml4)[0] = 0x7ULL | (pdpt_pa & 0x000FFFFFFFFFF000ULL);

    // Read MTRRs for whitelist checking
    ept_read_mtrrs();
    serial_print("[*] MTRR default=");
    serial_print_dec(mtrr_default_type);
    serial_print(" vars=");
    serial_print_dec(mtrr_count);
    serial_print("\n");

    // 0-4GB: 2MB pages — MTRR whitelist (WB only where MTRR says WB)
    // Count the resulting types so a hardware run can confirm the MTRR default
    // was actually honoured: if system RAM is still coming out uncached, the WB
    // count prints as 0 and the typing is still wrong.
    uint32_t wb_pages = 0, uc_pages = 0;

    for (int gi = 0; gi < 4; gi++) {
        uint64_t pd_pa;
        uint64_t* pd = (uint64_t*)ept_alloc_page(&pd_pa);
        if (!pd) return -1;
        pdpt[gi] = 0x7ULL | (pd_pa & 0x000FFFFFFFFFF000ULL);

        for (int pi = 0; pi < 512; pi++) {
            uint64_t phys = ((uint64_t)gi << 30) | ((uint64_t)pi << 21);
            uint64_t mt = 0; // UC default — SAFE
            // Only WB if BOTH start and end of 2MB range confirmed WB by MTRR
            if (phys >= 0x200000 && mtrr_is_wb(phys) && mtrr_is_wb(phys + 0x1FFFFF))
                mt = 6;
            if (mt == 6) wb_pages++; else uc_pages++;

            pd[pi] = 0x7ULL | (mt << 3) | (1ULL << 7)
                   | (phys & 0xFFFFFFFFFFE00000ULL);
        }
    }

    // 4-512GB: 1GB pages — MTRR whitelist
    for (int i = 4; i < 512; i++) {
        uint64_t phys = (uint64_t)i << 30;
        uint64_t mt = 0; // UC default
        if (mtrr_is_wb(phys) && mtrr_is_wb(phys + 0x3FFFFFFF))
            mt = 6;
        if (mt == 6) wb_pages++; else uc_pages++;
        pdpt[i] = 0x7ULL | (mt << 3) | (1ULL << 7)
                 | (phys & 0xFFFFFFFFC0000000ULL);
    }

    // EPTP: WB for structure access + 4-level walk
    state->eptp.value = 6ULL | (3ULL << 3)
                      | (state->ept_pml4_pa & 0x000FFFFFFFFFF000ULL);

    state->ept_wb_pages = wb_pages;
    state->ept_uc_pages = uc_pages;
    serial_print("[+] EPT typing: ");
    serial_print_dec(wb_pages);
    serial_print(" WB pages, ");
    serial_print_dec(uc_pages);
    serial_print(" UC pages\n");
    serial_print("[+] EPTP = ");
    serial_print_hex(state->eptp.value);
    serial_print("\n");
    return 0;
}

int ept_build_identity_map(VMM_STATE* state, uint64_t max_phys) {
    serial_print("[*] Building EPT identity map for ");
    serial_print_hex(max_phys);
    serial_print(" bytes\n");

    // Read MTRRs to determine correct memory type per region
    ept_read_mtrrs();
    serial_print("[*] MTRR default type=");
    serial_print_dec(mtrr_default_type);
    serial_print(" variable count=");
    serial_print_dec(mtrr_count);
    serial_print("\n");

    state->ept_pml4 = (EPT_ENTRY*)ept_alloc_page(&state->ept_pml4_pa);
    if (!state->ept_pml4) return -1;

    uint64_t phys = 0;
    uint64_t pml4_idx = 0;

    while (phys < max_phys && pml4_idx < 512) {
        uint64_t pdpt_pa;
        EPT_ENTRY* pdpt = (EPT_ENTRY*)ept_alloc_page(&pdpt_pa);
        if (!pdpt) return -1;

        state->ept_pml4[pml4_idx].value = 0;
        state->ept_pml4[pml4_idx].read = 1;
        state->ept_pml4[pml4_idx].write = 1;
        state->ept_pml4[pml4_idx].execute = 1;
        state->ept_pml4[pml4_idx].pfn = pdpt_pa >> 12;

        for (uint64_t pdpt_idx = 0; pdpt_idx < 512 && phys < max_phys; pdpt_idx++) {
            uint64_t pd_pa;
            EPT_ENTRY* pd = (EPT_ENTRY*)ept_alloc_page(&pd_pa);
            if (!pd) return -1;

            pdpt[pdpt_idx].value = 0;
            pdpt[pdpt_idx].read = 1;
            pdpt[pdpt_idx].write = 1;
            pdpt[pdpt_idx].execute = 1;
            pdpt[pdpt_idx].pfn = pd_pa >> 12;

            for (uint64_t pd_idx = 0; pd_idx < 512 && phys < max_phys; pd_idx++) {
                // Get correct memory type from MTRR for this 2MB page.
                // Check at page start — if any subrange differs, use UC (safe).
                uint8_t mt = ept_get_memory_type(phys);
                // For 2MB pages: if the range straddles an MTRR boundary,
                // the whole page must use UC to be safe.
                uint8_t mt_end = ept_get_memory_type(phys + PAGE_2MB - 1);
                if (mt != mt_end) mt = 0; // UC if types differ within the page

                pd[pd_idx].value = 0;
                pd[pd_idx].read = 1;
                pd[pd_idx].write = 1;
                pd[pd_idx].execute = 1;
                pd[pd_idx].large_page = 1;
                pd[pd_idx].memory_type = mt;
                pd[pd_idx].pfn = phys >> 12;
                phys += PAGE_2MB;
            }
        }
        pml4_idx++;
    }

    // Build EPTP manually — don't trust bitfield layout
    state->eptp.value = 6ULL                    // memory_type = WB (bits 2:0)
                      | (3ULL << 3)             // page_walk = 4-level (bits 5:3)
                      | (state->ept_pml4_pa & 0x000FFFFFFFFFF000ULL); // PML4 PFN

    serial_print("[+] EPT built: ");
    serial_print_dec((uint32_t)(phys / PAGE_2MB));
    serial_print(" 2MB pages mapped\n");
    serial_print("[+] EPTP = ");
    serial_print_hex(state->eptp.value);
    serial_print("\n");
    return 0;
}

int ept_setup_shared_page(VMM_STATE* state) {
    state->shared_page_va = ept_alloc_page(&state->shared_page_pa);
    if (!state->shared_page_va) return -1;

    EFI_PHYSICAL_ADDRESS draw_pa;
    EFI_STATUS status = gBS->AllocatePages(
        AllocateAnyPages, EfiRuntimeServicesData, 16, &draw_pa);
    if (EFI_ERROR(status)) return -1;
    state->draw_buf_va = (void*)draw_pa;
    state->draw_buf_pa = draw_pa;
    ZeroMem(state->draw_buf_va, ARIA_DRAW_BUF_SIZE);

    serial_print("[+] Shared page PA: ");
    serial_print_hex(state->shared_page_pa);
    serial_print("\n[+] Draw buffer PA: ");
    serial_print_hex(state->draw_buf_pa);
    serial_print("\n");
    return 0;
}

int ept_hide_vmm_memory(VMM_STATE* state) {
    serial_print("[*] VMM memory hiding deferred to post-launch\n");
    return 0;
}
