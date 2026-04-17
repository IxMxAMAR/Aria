// vmm/nested/eptp_switch.c — VMFUNC EPTP list pre-composition (spec §7)
// When L1 VMWRITEs an EPTP-list address, we:
// 1. Read all 512 entries
// 2. Pre-compose a shadow EPT for each valid entry
// 3. Build shadow EPTP switching list with composed EPTPs
// 4. Write-protect L1's EPTP list page to catch modifications
#include "eptp_switch.h"
#include "shadow_ept.h"
#include "../mem_engine.h"
#include "../ept.h"
#include "../../efi/serial.h"

int eptp_switch_setup(NESTED_STATE* ns) {
    if (!ns->eptp_list_gpa) return -1;

    // Read L1's EPTP list (512 × 8 bytes = 4KB)
    uint64_t eptp_list[NESTED_MAX_EPTP_LIST];
    uint64_t guest_cr3 = 0; // L1 GPA — identity mapped
    mem_read_physical(ns->eptp_list_gpa, eptp_list, sizeof(eptp_list));

    uint32_t valid_count = 0;

    for (uint32_t i = 0; i < NESTED_MAX_EPTP_LIST; i++) {
        if (eptp_list[i] == 0) continue;

        // Initialize a shadow EPT for this EPTP index
        ns->shadow_epts[i].l1_eptp = eptp_list[i];
        shadow_ept_init(ns, i);
        valid_count++;
    }

    // Build shadow EPTP switching list
    if (ns->shadow_eptp_list_va) {
        uint64_t* shadow_list = (uint64_t*)ns->shadow_eptp_list_va;
        for (uint32_t i = 0; i < NESTED_MAX_EPTP_LIST; i++) {
            if (ns->shadow_epts[i].valid && ns->shadow_epts[i].pml4_pa) {
                EPTP eptp;
                eptp.value = 0;
                eptp.memory_type = 6;
                eptp.page_walk = 3;
                eptp.pml4_pfn = ns->shadow_epts[i].pml4_pa >> 12;
                shadow_list[i] = eptp.value;
            } else {
                shadow_list[i] = 0;
            }
        }
    }

    // Write-protect L1's EPTP list page in L0's EPT
    // Any write to it triggers an EPT violation → we catch it and rebuild
    // TODO: set L0 EPT entry for eptp_list_gpa to read-only
    // ept_set_readonly(ns->eptp_list_gpa);

    DBG_PRINT("[eptp] Setup complete: ");
    DBG_DEC(valid_count);
    DBG_PRINT(" valid EPTP entries\n");

    (void)guest_cr3;
    return 0;
}

void eptp_switch_rebuild(NESTED_STATE* ns) {
    // Called when L1 modifies the EPTP list page (detected via EPT write-protection)
    // Re-read the list and rebuild any changed entries
    eptp_switch_setup(ns); // full rebuild for simplicity
}
