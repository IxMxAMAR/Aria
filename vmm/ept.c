#define EPT_MAX_TABLE_PAGES 256u

// vmm/ept.c — EPT runtime management
#include "ept.h"
#include "vmcs.h"
#include "../efi/serial.h"

// Split page tables have to live in memory the OS preserves. A static array here
// would sit in the image's BSS, which the guest frees at ExitBootServices, and
// every page table the EPT points at would be handed over to the guest.
static uint8_t* ept_pool = NULL;
static uint64_t ept_pool_size = 0;
static uint64_t ept_pool_offset = 0;

void ept_set_pool(void* base, uint64_t size) {
    ept_pool = (uint8_t*)base;
    ept_pool_size = size;
    ept_pool_offset = 0;
}

static void* ept_runtime_alloc_page(void) {
    if (ept_pool == NULL) {
        serial_print("[!] EPT pool not configured\n");
        return NULL;
    }
    if (ept_pool_offset + PAGE_SIZE > ept_pool_size) {
        serial_print("[!] EPT pool exhausted\n");
        return NULL;
    }
    void* p = &ept_pool[ept_pool_offset];
    ept_pool_offset += PAGE_SIZE;
    vmm_memset(p, 0, PAGE_SIZE);
    return p;
}

static uint64_t va_to_pa(void* va) {
    return (uint64_t)va;
}

static EPT_ENTRY* ept_walk(VMM_STATE* state, uint64_t gpa, int* level) {
    uint64_t pml4_idx = (gpa >> 39) & 0x1FF;
    uint64_t pdpt_idx = (gpa >> 30) & 0x1FF;
    uint64_t pd_idx   = (gpa >> 21) & 0x1FF;
    uint64_t pt_idx   = (gpa >> 12) & 0x1FF;

    EPT_ENTRY* pml4 = state->ept_pml4;
    if (!pml4[pml4_idx].read) { *level = 4; return &pml4[pml4_idx]; }

    EPT_ENTRY* pdpt = (EPT_ENTRY*)((uint64_t)(pml4[pml4_idx].pfn) << 12);
    if (!pdpt[pdpt_idx].read) { *level = 3; return &pdpt[pdpt_idx]; }
    if (pdpt[pdpt_idx].large_page) { *level = 3; return &pdpt[pdpt_idx]; }

    EPT_ENTRY* pd = (EPT_ENTRY*)((uint64_t)(pdpt[pdpt_idx].pfn) << 12);
    if (!pd[pd_idx].read) { *level = 2; return &pd[pd_idx]; }
    if (pd[pd_idx].large_page) { *level = 2; return &pd[pd_idx]; }

    EPT_ENTRY* pt = (EPT_ENTRY*)((uint64_t)(pd[pd_idx].pfn) << 12);
    *level = 1;
    return &pt[pt_idx];
}

int ept_split_2mb_to_4kb(VMM_STATE* state, uint64_t gpa) {
    int level;
    EPT_ENTRY* entry = ept_walk(state, gpa, &level);

    if (level != 2 || !entry->large_page) {
        return (level == 1) ? 0 : -1;
    }

    EPT_ENTRY* new_pt = (EPT_ENTRY*)ept_runtime_alloc_page();
    if (!new_pt) return -1;

    uint64_t base_pfn = entry->pfn;
    uint64_t base_pa = (uint64_t)base_pfn << 12;
    // Preserve the parent's memory type. Hardcoding WB here would silently make
    // an uncached region cacheable the moment one page in it was split.
    uint64_t parent_mt = entry->memory_type;

    for (int i = 0; i < 512; i++) {
        new_pt[i].value = 0;
        new_pt[i].read = 1;
        new_pt[i].write = 1;
        new_pt[i].execute = 1;
        new_pt[i].memory_type = parent_mt;
        new_pt[i].pfn = (base_pa + i * PAGE_SIZE) >> 12;
    }

    entry->large_page = 0;
    entry->memory_type = 0;
    entry->pfn = va_to_pa(new_pt) >> 12;

    ept_invept_global();
    return 0;
}

EPT_ENTRY* ept_get_pte_4kb(VMM_STATE* state, uint64_t gpa) {
    ept_split_2mb_to_4kb(state, gpa);
    int level;
    EPT_ENTRY* entry = ept_walk(state, gpa, &level);
    if (level != 1) return NULL;
    return entry;
}

int ept_install_split(VMM_STATE* state, uint64_t gpa,
                      void* clean_page, void* dirty_page)
{
    if (state->split_page_count >= MAX_SPLIT_PAGES) return -1;

    EPT_ENTRY* pte = ept_get_pte_4kb(state, gpa);
    if (!pte) return -1;

    uint32_t idx = state->split_page_count++;
    EPT_SPLIT_PAGE* sp = &state->split_pages[idx];
    sp->guest_physical = gpa & PAGE_MASK;
    sp->clean_physical = va_to_pa(clean_page);
    sp->dirty_physical = va_to_pa(dirty_page);
    sp->active = 1;

    pte->read = 1;
    pte->write = 1;
    pte->execute = 0;
    pte->pfn = sp->clean_physical >> 12;

    ept_invept_global();

    serial_print("[+] Split page installed at GPA ");
    serial_print_hex(gpa);
    serial_print("\n");

    return idx;
}

void ept_set_split_execute(VMM_STATE* state, uint32_t idx) {
    EPT_SPLIT_PAGE* sp = &state->split_pages[idx];
    EPT_ENTRY* pte = ept_get_pte_4kb(state, sp->guest_physical);
    if (!pte) return;

    pte->pfn = sp->dirty_physical >> 12;
    pte->read = 0;
    pte->write = 0;
    pte->execute = 1;
}

void ept_set_split_normal(VMM_STATE* state, uint32_t idx) {
    EPT_SPLIT_PAGE* sp = &state->split_pages[idx];
    EPT_ENTRY* pte = ept_get_pte_4kb(state, sp->guest_physical);
    if (!pte) return;

    pte->pfn = sp->clean_physical >> 12;
    pte->read = 1;
    pte->write = 1;
    pte->execute = 0;
}

int ept_hide_range(VMM_STATE* state, uint64_t phys_start, uint64_t size) {
    for (uint64_t pa = phys_start; pa < phys_start + size; pa += PAGE_SIZE) {
        EPT_ENTRY* pte = ept_get_pte_4kb(state, pa);
        if (pte) {
            pte->read = 0;
            pte->write = 0;
            pte->execute = 0;
        }
    }
    ept_invept_global();
    return 0;
}

extern void __invept(uint64_t type, void* descriptor);

void ept_invept_single(VMM_STATE* state, uint64_t addr) {
    struct { uint64_t eptp; uint64_t reserved; } desc;
    desc.eptp = state->eptp.value;
    desc.reserved = addr;
    __invept(1, &desc);
}

void ept_invept_global(void) {
    struct { uint64_t eptp; uint64_t reserved; } desc = {0, 0};
    __invept(2, &desc);
}

// Hide the EPT's own page tables and the split pool from the guest. This runs
// last, because hiding a page can itself split a 2MB entry and allocate a page
// table from the pool.
//
// Hiding a leaf entry for a GPA does not disturb the entries used to walk to any
// other GPA, and the CPU walks the EPT through physical addresses rather than
// through the EPT, so an active page table can have its own GPA hidden without
// affecting the walk.
void ept_protect_own_tables(VMM_STATE* state) {
    uint64_t pages[EPT_MAX_TABLE_PAGES];
    uint32_t n = 0;

    pages[n++] = state->ept_pml4_pa;
    EPT_ENTRY* pml4 = state->ept_pml4;

    for (int i = 0; i < 512; i++) {
        if (!pml4[i].read) continue;
        if (n >= EPT_MAX_TABLE_PAGES) break;
        uint64_t pdpt_pa = (uint64_t)pml4[i].pfn << 12;
        pages[n++] = pdpt_pa;

        EPT_ENTRY* pdpt = (EPT_ENTRY*)pdpt_pa;
        for (int j = 0; j < 512; j++) {
            if (!pdpt[j].read || pdpt[j].large_page) continue;
            if (n >= EPT_MAX_TABLE_PAGES) break;
            uint64_t pd_pa = (uint64_t)pdpt[j].pfn << 12;
            pages[n++] = pd_pa;

            EPT_ENTRY* pd = (EPT_ENTRY*)pd_pa;
            for (int k = 0; k < 512; k++) {
                if (!pd[k].read || pd[k].large_page) continue;
                if (n >= EPT_MAX_TABLE_PAGES) break;
                pages[n++] = (uint64_t)pd[k].pfn << 12;   // a split page table
            }
        }
    }

    for (uint32_t k = 0; k < n; k++) {
        ept_hide_range(state, pages[k], PAGE_SIZE);
    }

    // The whole pool, not just the used prefix: hiding it can allocate from it.
    if (ept_pool != NULL && ept_pool_size != 0) {
        ept_hide_range(state, (uint64_t)(UINTN)ept_pool, ept_pool_size);
    }

    serial_print("[+] EPT own tables protected: ");
    serial_print_dec(n);
    serial_print(" table pages, pool ");
    serial_print_dec((uint32_t)(ept_pool_size >> 12));
    serial_print(" pages\n");
}