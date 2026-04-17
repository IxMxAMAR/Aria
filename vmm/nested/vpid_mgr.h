// vmm/nested/vpid_mgr.h — VPID allocation and translation (spec §9)
#pragma once

#include "nested_vmx.h"

// Allocate a physical VPID for L1's logical VPID
// Returns L0's physical VPID = VPID_BASE + l1_vpid
uint16_t vpid_allocate(NESTED_STATE* ns, uint16_t l1_vpid);

// Free a previously allocated VPID translation
void vpid_free(NESTED_STATE* ns, uint16_t l1_vpid);

// Translate L1's logical VPID → L0's physical VPID
// Auto-allocates if not yet mapped
uint16_t vpid_translate(NESTED_STATE* ns, uint16_t l1_vpid);
