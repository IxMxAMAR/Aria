// vmm/ept.c — EPT runtime management
#include "ept.h"
#include "vmcs.h"
#include "../efi/serial.h"

static ARIA_ALIGN(4096) uint8_t ept_pool[4 * 1024 * 1024];
static uint64_t ept_pool_offset = 0;

static void* ept_runtime_alloc_page(void) {
    if (ept_pool_offset + PAGE_SIZE > sizeof(ept_pool)) {
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

    for (int i = 0; i < 512; i++) {
        new_pt[i].value = 0;
        new_pt[i].read = 1;
        new_pt[i].write = 1;
        new_pt[i].execute = 1;
        new_pt[i].memory_type = 6;
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
