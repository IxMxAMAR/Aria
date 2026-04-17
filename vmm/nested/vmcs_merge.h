// vmm/nested/vmcs_merge.h — Table-driven VMCS02 merge (spec §3.3)
// The most critical file in the nested VMX engine.
// Every VMCS field must be in the merge table. Missing fields = L2 crash.
#pragma once

#include "nested_vmx.h"

// Build VMCS02 from cached L0 fields + VMCS12 software copy.
// VMCS02 must be current (VMPTRLD'd) before calling.
// L0 field cache must be populated (call vmcs_merge_cache_l0 first).
// Returns 0 on success, nonzero = SDM error code.
int vmcs_merge_build_vmcs02(NESTED_STATE* ns);

// Cache L0's host/control fields from VMCS01 (must be current).
// Call BEFORE switching to VMCS02.
void vmcs_merge_cache_l0(NESTED_STATE* ns);

// Validate merged VMCS02 per SDM §26.2-26.3.
int vmcs_merge_validate(NESTED_STATE* ns);

// Get the merge table for inspection.
const VMCS_MERGE_ENTRY* vmcs_merge_get_table(uint32_t* out_count);
