// vmm/nested/vmcs_shadow.h — Shadow VMCS page + VMREAD/VMWRITE bitmap management
// Spec §3.1: Hardware VMCS Shadowing
#pragma once

#include "nested_vmx.h"

// Initialize shadow VMCS page: set revision_id | (1u << 31)
// CRITICAL: bit 31 MUST be set per Intel SDM §24.10
int vmcs_shadow_init(NESTED_STATE* ns, uint32_t revision_id);

// Allocate VMREAD/VMWRITE bitmap pages (shared across all CPUs)
int vmcs_shadow_alloc_bitmaps(NESTED_STATE* ns);

// Configure VMCS01 to enable VMCS shadowing:
// - Set VMCS_LINK_POINTER to shadow VMCS PA
// - Enable secondary proc-based bit 14
// - Set VMREAD bitmap (all 0s = hardware handles reads)
// - Set VMWRITE bitmap (all 1s = intercept all writes)
void vmcs_shadow_setup_bitmaps(NESTED_STATE* ns);

// Write a value back to the shadow VMCS page so L1's VMREAD sees it.
// Used after exit reflection to update guest state visible to L1.
// Temporarily disables VMCS shadowing, VMPTRLD shadow, VMWRITE, restore.
void vmcs_shadow_writeback(NESTED_STATE* ns, uint32_t encoding, uint64_t value);

// Bulk sync all dirty VMCS12 fields to the shadow VMCS page.
// Must be called AFTER VMCS01 is restored as current VMCS.
void vmcs_shadow_sync_to_hardware(NESTED_STATE* ns);
