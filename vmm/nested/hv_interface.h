// vmm/nested/hv_interface.h — Hyper-V synthetic MSR/CPUID (spec §8)
#pragma once
#include "nested_vmx.h"

// Handle Hyper-V synthetic MSR read from L1. Returns 1 if handled, 0 if not ours.
int hv_handle_msr_read(NESTED_STATE* ns, uint32_t msr, uint64_t* value);

// Handle Hyper-V synthetic MSR write from L1. Returns 1 if handled, 0 if not ours.
int hv_handle_msr_write(NESTED_STATE* ns, uint32_t msr, uint64_t value);

// Impersonate Microsoft Hyper-V on CPUID leaves 0x40000000-0x40000006 (spec §8.2)
void hv_cpuid_impersonate(uint32_t leaf, uint32_t subleaf, GUEST_REGS* regs);
