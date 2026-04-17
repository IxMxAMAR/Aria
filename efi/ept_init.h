// efi/ept_init.h — EPT identity map builder (runs in EFI context)
#pragma once

#include "../vmm/vmm.h"

int ept_build_identity_map(VMM_STATE* state, uint64_t max_physical_address);
int ept_setup_shared_page(VMM_STATE* state);
int ept_hide_vmm_memory(VMM_STATE* state);
