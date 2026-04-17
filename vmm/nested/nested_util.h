// vmm/nested/nested_util.h — VMCS12 field access + SDM validation helpers
#pragma once

#include "nested_vmx.h"

// VMCS12 field access (software copy — single source of truth)
void     vmcs12_write(SOFTWARE_VMCS12* v, uint32_t encoding, uint64_t value);
uint64_t vmcs12_read(SOFTWARE_VMCS12* v, uint32_t encoding);
int      vmcs12_has_field(SOFTWARE_VMCS12* v, uint32_t encoding);
void     vmcs12_clear(SOFTWARE_VMCS12* v);
uint32_t vmcs12_field_hash(uint32_t encoding);
uint64_t vmcs12_read_or_default(SOFTWARE_VMCS12* v, uint32_t encoding, uint64_t def);
int      vmcs12_control_bit_set(SOFTWARE_VMCS12* v, uint32_t control_field, uint32_t bit);

// SDM §26.2-26.3 validation of VMCS12 before building VMCS02
// Returns 0 on success, nonzero = VMX instruction error code
int vmcs12_validate_controls(SOFTWARE_VMCS12* v);
int vmcs12_validate_guest_state(SOFTWARE_VMCS12* v);
int vmcs12_validate_host_state(SOFTWARE_VMCS12* v);
