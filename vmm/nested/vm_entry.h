// vmm/nested/vm_entry.h — L1 → L2 entry (spec §4)
#pragma once

#include "nested_vmx.h"

// Full L2 entry sequence:
// 1. VMCS12 already populated via intercepted VMWRITEs
// 2. Validate VMCS12 per SDM §26.2-26.3
// 3. Cache L0 fields from VMCS01
// 4. XSAVE L1's FPU/SSE/AVX state
// 5. VMCLEAR VMCS02 (always — spec §4.1 step 9)
// 6. VMPTRLD VMCS02
// 7. Write all merged fields via merge table
// 8. VMLAUNCH into L2 (always VMLAUNCH, never VMRESUME after VMCLEAR)
//
// Returns 0 on success (L2 is running), nonzero = VMX error code
int nested_enter_l2(NESTED_STATE* ns);
