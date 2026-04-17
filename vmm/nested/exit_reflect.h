// vmm/nested/exit_reflect.h — L2 → L1 exit reflection (spec §5.2)
#pragma once

#include "nested_vmx.h"

// Reflect a VM exit from L2 to L1:
// 1. Save all 40+ L2 VMCS guest state fields to VMCS12
// 2. Write exit information (reason, qualification, interrupt info)
// 3. Account preemption timer
// 4. XSAVE L2 / XRSTOR L1 FPU state
// 5. VMPTRLD VMCS01
// 6. VMRESUME L1
void nested_reflect_to_l1(NESTED_STATE* ns, GUEST_REGS* regs,
                          uint32_t exit_reason, uint64_t exit_qual);
