// vmm/ept.h — EPT runtime management
#pragma once

#include "vmm.h"

void ept_set_pool(void* base, uint64_t size);
int ept_split_2mb_to_4kb(VMM_STATE* state, uint64_t guest_physical);
int ept_install_split(VMM_STATE* state, uint64_t guest_physical,
                      void* clean_page, void* dirty_page);
void ept_set_split_execute(VMM_STATE* state, uint32_t split_idx);
void ept_set_split_normal(VMM_STATE* state, uint32_t split_idx);
int ept_hide_range(VMM_STATE* state, uint64_t phys_start, uint64_t size);
EPT_ENTRY* ept_get_pte_4kb(VMM_STATE* state, uint64_t guest_physical);
void ept_invept_single(VMM_STATE* state, uint64_t addr);
void ept_invept_global(void);

void ept_protect_own_tables(VMM_STATE* state);