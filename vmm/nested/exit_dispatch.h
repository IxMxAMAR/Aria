// vmm/nested/exit_dispatch.h — L2 exit classification (spec §5.1)
#pragma once

#include "nested_vmx.h"

// Classify an L2 VM exit. Returns:
//   0 = L0 handles (VMRESUME L2 after handling)
//   1 = reflect to L1 (call nested_reflect_to_l1)
int nested_classify_exit(NESTED_STATE* ns, GUEST_REGS* regs,
                         uint32_t exit_reason, uint64_t exit_qual);
