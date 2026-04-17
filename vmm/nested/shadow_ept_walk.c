// vmm/nested/shadow_ept_walk.c — Software walk of L1's EPT (spec §6.3)
// Translates L2 GPA through L1's EPT to get L1 GPA.
// L1's EPT pages live at L1 GPAs which we access via physical memory
// (L0's host page tables identity-map all RAM).
#include "shadow_ept_walk.h"
#include "nested_util.h"
#include "../mem_engine.h"
#include "../../efi/serial.h"

// EPT entry flags (same as in vmm.h but we define locally for clarity)
#define EPT_READ    (1ULL << 0)
#define EPT_WRITE   (1ULL << 1)
#define EPT_EXEC    (1ULL << 2)
#define EPT_LARGE   (1ULL << 7)
#define EPT_PFN_MASK 0x000FFFFFFFFFF000ULL

int shadow_ept_walk_l1(NESTED_STATE* ns, uint32_t eptp_index,
                       uint64_t l2_gpa, uint64_t* out_l1_gpa)
{
    SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[eptp_index];
    uint64_t l1_eptp = ctx->l1_eptp;

    // L1's EPT PML4 physical address (from L1's EPTP)
    uint64_t l1_pml4_pa = l1_eptp & EPT_PFN_MASK;

    // Walk L1's 4-level EPT structure
    // L1's EPT pages are at L1 GPAs. Since L0's EPT identity-maps physical RAM,
    // L1 GPA == HPA for normal RAM. We read them as physical memory.

    uint64_t indices[4] = {
        (l2_gpa >> 39) & 0x1FF,  // PML4 index
        (l2_gpa >> 30) & 0x1FF,  // PDPT index
        (l2_gpa >> 21) & 0x1FF,  // PD index
        (l2_gpa >> 12) & 0x1FF   // PT index
    };

    uint64_t table_pa = l1_pml4_pa;

    for (int level = 0; level < 4; level++) {
        uint64_t entry_pa = table_pa + indices[level] * 8;
        uint64_t entry = 0;

        // Read the EPT entry from physical memory
        mem_read_physical(entry_pa, &entry, 8);

        // Check present bits (R/W/X — any bit set means present)
        if (!(entry & (EPT_READ | EPT_WRITE | EPT_EXEC))) {
            // Not present at this level — L1 doesn't map this GPA
            return SHADOW_EPT_L1_NOT_PRESENT;
        }

        // Check for large page (2MB at level 2, 1GB at level 1)
        if ((entry & EPT_LARGE) && (level == 1 || level == 2)) {
            uint64_t page_size = (level == 1) ? (1ULL << 30) : (1ULL << 21);
            uint64_t offset_mask = page_size - 1;
            *out_l1_gpa = (entry & EPT_PFN_MASK & ~offset_mask) | (l2_gpa & offset_mask);
            return 0;
        }

        // Not a large page — descend to next level
        table_pa = entry & EPT_PFN_MASK;
    }

    // Reached PT level — extract the 4KB page mapping
    // (The loop already read the PT entry on level 3)
    // Actually, the loop read the PT entry and checked present. If we're here,
    // the entry at level 3 was present but not a large page (correct for 4KB).
    // We need to re-read the PT entry to get the PFN.
    uint64_t pt_entry_pa = table_pa; (void)pt_entry_pa; // table_pa = PT entry's PFN
    // Wait — the loop set table_pa = entry & EPT_PFN_MASK after the last iteration.
    // That's the PFN of the mapped physical page, not the next table.
    // The 4KB leaf case: entry at level 3 has PFN of the final physical page.
    // table_pa after level 3 = entry & EPT_PFN_MASK = L1 GPA of the mapped page.

    *out_l1_gpa = table_pa | (l2_gpa & 0xFFF);
    return 0;
}
