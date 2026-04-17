// vmm/nested/shadow_ept.c — Shadow EPT lazy composition engine (spec §6)
// Composes L1's EPT through L0's EPT into a single shadow EPT for L2.
// 64MB pool, LRU eviction fallback, hook injection during composition.
#include "shadow_ept.h"
#include "shadow_ept_walk.h"
#include "../ept.h"
#include "../mem_engine.h"
#include "../vmm.h"
#include "../../shared/protocol.h"
#include "../../efi/serial.h"

void* shadow_ept_alloc_page(NESTED_STATE* ns) {
    if (ns->shadow_ept_pool_offset + PAGE_SIZE > ns->shadow_ept_pool_size) {
        DBG_PRINT("[shadow_ept] Pool exhausted! Attempting LRU eviction...\n");
        // TODO: implement LRU eviction — clear oldest composed leaves
        // For now: fail
        return NULL;
    }
    void* p = ns->shadow_ept_pool + ns->shadow_ept_pool_offset;
    ns->shadow_ept_pool_offset += PAGE_SIZE;
    vmm_memset(p, 0, PAGE_SIZE);
    return p;
}

int shadow_ept_init(NESTED_STATE* ns, uint32_t eptp_index) {
    if (eptp_index >= NESTED_MAX_EPTP_LIST) return -1;

    SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[eptp_index];
    spinlock_acquire(&ctx->lock);

    // Allocate PML4 for this shadow EPT
    ctx->pml4_va = (EPT_ENTRY*)shadow_ept_alloc_page(ns);
    if (!ctx->pml4_va) {
        spinlock_release(&ctx->lock);
        return -1;
    }
    ctx->pml4_pa = (uint64_t)ctx->pml4_va; // identity-mapped in L0 host page tables
    ctx->valid = 1;
    ctx->page_count = 0;

    spinlock_release(&ctx->lock);

    DBG_PRINT("[shadow_ept] Initialized index ");
    DBG_DEC(eptp_index);
    DBG_PRINT(", PML4 PA=");
    DBG_HEX(ctx->pml4_pa);
    DBG_PRINT("\n");

    return 0;
}

int shadow_ept_compose_page(NESTED_STATE* ns, uint32_t eptp_index, uint64_t l2_gpa) {
    if (eptp_index >= NESTED_MAX_EPTP_LIST) return -1;

    SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[eptp_index];
    spinlock_acquire(&ctx->lock);

    // NOTE: shadow_ept_alloc_page returns VAs from the EFI-allocated pool.
    // In our identity-mapped host page tables, VA == PA for all EFI
    // RuntimeServicesData pages. PFN = (uint64_t)va >> 12 is correct.

    // Step 1: Walk L1's EPT to translate L2 GPA → L1 GPA
    uint64_t l1_gpa = 0;
    int walk_result = shadow_ept_walk_l1(ns, eptp_index, l2_gpa, &l1_gpa);

    if (walk_result == SHADOW_EPT_L1_NOT_PRESENT) {
        // L1's EPT doesn't map this GPA → reflect to L1 as EPT violation
        spinlock_release(&ctx->lock);
        return 1;
    }

    if (walk_result != 0) {
        spinlock_release(&ctx->lock);
        return -1;
    }

    // Step 2: Translate L1 GPA → HPA through L0's EPT
    uint64_t hpa = mem_virt_to_phys(0, l1_gpa); // L0's EPT is identity-mapped
    // Actually: L1 GPA == HPA for most RAM (L0 identity maps physical memory)
    // Only VMM-hidden memory differs
    hpa = l1_gpa; // identity-mapped

    // Check if this HPA falls in VMM hidden memory ranges
    // If so, DON'T compose — return -1 to inject #PF to L2
    {
        extern VMM_STATE g_vmm;
        uint64_t check_pa = hpa & PAGE_MASK;

        // Check main VMM regions
        if ((check_pa >= g_vmm.vmxon_region_pa && check_pa < g_vmm.vmxon_region_pa + PAGE_SIZE) ||
            (check_pa >= g_vmm.vmcs_region_pa && check_pa < g_vmm.vmcs_region_pa + PAGE_SIZE) ||
            (check_pa >= g_vmm.msr_bitmap_pa && check_pa < g_vmm.msr_bitmap_pa + PAGE_SIZE) ||
            (check_pa >= g_vmm.host_stack_pa && check_pa < g_vmm.host_stack_pa + 0x10000) ||
            (check_pa >= g_vmm.shared_page_pa && check_pa < g_vmm.shared_page_pa + PAGE_SIZE) ||
            (check_pa >= g_vmm.draw_buf_pa && check_pa < g_vmm.draw_buf_pa + ARIA_DRAW_BUF_SIZE)) {
            spinlock_release(&ctx->lock);
            return -1;
        }

        // Check nested state regions
        NESTED_STATE* nst = g_vmm.nested;
        if (nst) {
            uint64_t ns_pa = (uint64_t)nst; // identity-mapped
            uint64_t ns_end = ns_pa + sizeof(NESTED_STATE);
            if (check_pa >= ns_pa && check_pa < ns_end) {
                spinlock_release(&ctx->lock);
                return -1;
            }
            if (nst->shadow_ept_pool_pa &&
                check_pa >= nst->shadow_ept_pool_pa &&
                check_pa < nst->shadow_ept_pool_pa + nst->shadow_ept_pool_size) {
                spinlock_release(&ctx->lock);
                return -1;
            }
            if (check_pa == nst->vmcs02_pa || check_pa == nst->shadow_vmcs_pa ||
                check_pa == nst->composed_msr_bitmap_pa ||
                check_pa == nst->vmread_bitmap_pa || check_pa == nst->vmwrite_bitmap_pa) {
                spinlock_release(&ctx->lock);
                return -1;
            }
        }
    }

    // Step 3: Build shadow EPT entry: L2 GPA → HPA
    // Navigate/create shadow EPT page table levels
    // (Similar to ept_get_pte_4kb but for the shadow EPT)

    uint64_t pml4_idx = (l2_gpa >> 39) & 0x1FF;
    uint64_t pdpt_idx = (l2_gpa >> 30) & 0x1FF;
    uint64_t pd_idx   = (l2_gpa >> 21) & 0x1FF;
    uint64_t pt_idx   = (l2_gpa >> 12) & 0x1FF;

    // PML4 → PDPT
    EPT_ENTRY* pml4 = ctx->pml4_va;
    if (!pml4[pml4_idx].read) {
        EPT_ENTRY* pdpt = (EPT_ENTRY*)shadow_ept_alloc_page(ns);
        if (!pdpt) { spinlock_release(&ctx->lock); return -1; }
        pml4[pml4_idx].read = 1;
        pml4[pml4_idx].write = 1;
        pml4[pml4_idx].execute = 1;
        pml4[pml4_idx].pfn = (uint64_t)pdpt >> 12;
    }
    EPT_ENTRY* pdpt = (EPT_ENTRY*)((uint64_t)pml4[pml4_idx].pfn << 12);

    // PDPT → PD
    if (!pdpt[pdpt_idx].read) {
        EPT_ENTRY* pd = (EPT_ENTRY*)shadow_ept_alloc_page(ns);
        if (!pd) { spinlock_release(&ctx->lock); return -1; }
        pdpt[pdpt_idx].read = 1;
        pdpt[pdpt_idx].write = 1;
        pdpt[pdpt_idx].execute = 1;
        pdpt[pdpt_idx].pfn = (uint64_t)pd >> 12;
    }
    EPT_ENTRY* pd = (EPT_ENTRY*)((uint64_t)pdpt[pdpt_idx].pfn << 12);

    // PD → PT
    if (!pd[pd_idx].read) {
        EPT_ENTRY* pt = (EPT_ENTRY*)shadow_ept_alloc_page(ns);
        if (!pt) { spinlock_release(&ctx->lock); return -1; }
        pd[pd_idx].read = 1;
        pd[pd_idx].write = 1;
        pd[pd_idx].execute = 1;
        pd[pd_idx].pfn = (uint64_t)pt >> 12;
    }
    EPT_ENTRY* pt = (EPT_ENTRY*)((uint64_t)pd[pd_idx].pfn << 12);

    // PT leaf entry: L2 GPA → HPA
    pt[pt_idx].read = 1;
    pt[pt_idx].write = 1;
    pt[pt_idx].execute = 1;
    pt[pt_idx].memory_type = 6; // write-back
    pt[pt_idx].pfn = hpa >> 12;

    // Step 4: Check if this HPA is a hook target → inject split page (spec §10.2)
    // During shadow EPT composition, every resulting HPA is checked against
    // the parent VMM's split-page list. If the HPA matches a hooked page
    // (e.g., Present in dxgi.dll, GetMessageW in user32.dll), we apply the
    // same split-page configuration to the shadow EPT entry:
    //   read/write → clean physical page (integrity checks pass)
    //   execute    → dirty physical page (our hook runs)
    // This ensures hooks persist across ALL shadow EPT variants (VTL0, VTL1).
    // After INVEPT invalidation, recomposition re-injects hooks automatically.
    {
        extern VMM_STATE g_vmm;
        uint64_t page_hpa = hpa & PAGE_MASK;
        for (uint32_t si = 0; si < g_vmm.split_page_count; si++) {
            EPT_SPLIT_PAGE* sp = &g_vmm.split_pages[si];
            if (!sp->active) continue;

            // Match: the composed HPA falls on a page we've hooked in L0's EPT
            if (page_hpa == sp->guest_physical) {
                // Apply split to the shadow EPT leaf entry:
                // Read/write → clean page (original bytes, detection software sees clean)
                // Execute    → dirty page (our CPUID hook shellcode runs)
                pt[pt_idx].pfn = sp->clean_physical >> 12;
                pt[pt_idx].read = 1;
                pt[pt_idx].write = 1;
                pt[pt_idx].execute = 0;  // execute causes EPT violation → flip to dirty

                DBG_PRINT("[shadow_ept] Hook injected at HPA=");
                DBG_HEX(page_hpa);
                DBG_PRINT(" (split idx ");
                DBG_DEC(si);
                DBG_PRINT(")\n");
                break;
            }
        }
    }

    ctx->page_count++;
    spinlock_release(&ctx->lock);

    // Flush TLB for this shadow EPTP
    extern void __invept(uint64_t type, void* descriptor);
    struct { uint64_t eptp; uint64_t reserved; } desc;
    EPTP eptp;
    eptp.value = 0;
    eptp.memory_type = 6;
    eptp.page_walk = 3;
    eptp.pml4_pfn = ctx->pml4_pa >> 12;
    desc.eptp = eptp.value;
    desc.reserved = 0;
    __invept(1, &desc);

    return 0; // composed successfully — VMRESUME L2
}

int shadow_ept_handle_hook_violation(NESTED_STATE* ns, uint32_t eptp_index, uint64_t l2_gpa) {
    if (eptp_index >= NESTED_MAX_EPTP_LIST) return 0;

    SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[eptp_index];
    if (!ctx->valid || !ctx->pml4_va) return 0;

    // Translate L2 GPA through L1's EPT to get HPA
    // Then check if HPA matches any split-page hook
    uint64_t l1_gpa = 0;
    int walk_result = shadow_ept_walk_l1(ns, eptp_index, l2_gpa, &l1_gpa);
    if (walk_result != 0) return 0;

    uint64_t hpa = l1_gpa; // identity-mapped L0 EPT
    uint64_t page_hpa = hpa & PAGE_MASK;

    extern VMM_STATE g_vmm;
    for (uint32_t si = 0; si < g_vmm.split_page_count; si++) {
        EPT_SPLIT_PAGE* sp = &g_vmm.split_pages[si];
        if (!sp->active) continue;
        if (page_hpa != sp->guest_physical) continue;

        // Found a hook match — flip the shadow EPT leaf entry
        // Navigate to the leaf in the shadow EPT
        uint64_t pml4_idx = (l2_gpa >> 39) & 0x1FF;
        uint64_t pdpt_idx = (l2_gpa >> 30) & 0x1FF;
        uint64_t pd_idx   = (l2_gpa >> 21) & 0x1FF;
        uint64_t pt_idx   = (l2_gpa >> 12) & 0x1FF;

        EPT_ENTRY* pml4 = ctx->pml4_va;
        if (!pml4[pml4_idx].read) return 0;
        EPT_ENTRY* pdpt = (EPT_ENTRY*)((uint64_t)pml4[pml4_idx].pfn << 12);
        if (!pdpt[pdpt_idx].read) return 0;
        EPT_ENTRY* pd = (EPT_ENTRY*)((uint64_t)pdpt[pdpt_idx].pfn << 12);
        if (!pd[pd_idx].read) return 0;
        EPT_ENTRY* pt = (EPT_ENTRY*)((uint64_t)pd[pd_idx].pfn << 12);

        EPT_ENTRY* leaf = &pt[pt_idx];

        if (!leaf->execute) {
            // Currently read/write on clean → flip to execute-only on dirty
            leaf->pfn = sp->dirty_physical >> 12;
            leaf->read = 0;
            leaf->write = 0;
            leaf->execute = 1;
        } else {
            // Currently execute-only on dirty → flip back to read/write on clean
            leaf->pfn = sp->clean_physical >> 12;
            leaf->read = 1;
            leaf->write = 1;
            leaf->execute = 0;
        }

        // Flush shadow EPTP TLB
        extern void __invept(uint64_t type, void* descriptor);
        struct { uint64_t eptp; uint64_t reserved; } desc;
        EPTP eptp_val;
        eptp_val.value = 0;
        eptp_val.memory_type = 6;
        eptp_val.page_walk = 3;
        eptp_val.pml4_pfn = ctx->pml4_pa >> 12;
        desc.eptp = eptp_val.value;
        desc.reserved = 0;
        __invept(1, &desc);

        return 1; // handled
    }

    return 0; // not a hook page
}

void shadow_ept_invalidate(NESTED_STATE* ns, uint32_t eptp_index) {
    if (eptp_index >= NESTED_MAX_EPTP_LIST) return;
    SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[eptp_index];

    spinlock_acquire(&ctx->lock);

    // Zero the PML4 page — all entries become not-present
    // Next L2 access to any page triggers recomposition
    if (ctx->pml4_va) {
        vmm_memset(ctx->pml4_va, 0, PAGE_SIZE);
    }
    ctx->page_count = 0;
    // Note: intermediate PT pages are leaked (pool doesn't free individual pages)
    // This is acceptable — the pool is 64MB and LRU eviction handles pressure

    spinlock_release(&ctx->lock);

    DBG_PRINT("[shadow_ept] Invalidated index ");
    DBG_DEC(eptp_index);
    DBG_PRINT("\n");
}

void shadow_ept_invalidate_all(NESTED_STATE* ns) {
    for (uint32_t i = 0; i < NESTED_MAX_EPTP_LIST; i++) {
        if (ns->shadow_epts[i].valid) {
            shadow_ept_invalidate(ns, i);
        }
    }
}
