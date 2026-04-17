// vmm/nested/shadow_ept_walk.h — L1 EPT software walk + disambiguation (spec §6.3)
#pragma once
#include "nested_vmx.h"

#define SHADOW_EPT_L1_NOT_PRESENT  1
#define SHADOW_EPT_L0_HIDDEN       2

// Walk L1's EPT to translate L2 GPA → L1 GPA.
// Returns 0 on success, SHADOW_EPT_L1_NOT_PRESENT if L1 doesn't map it,
// SHADOW_EPT_L0_HIDDEN if L1 maps to VMM hidden memory.
int shadow_ept_walk_l1(NESTED_STATE* ns, uint32_t eptp_index,
                       uint64_t l2_gpa, uint64_t* out_l1_gpa);
