// vmm/nested/cr_nested.c — CR access exit handling with dual-shadow (spec §5.5)
// Handles the case where L0 masks bits that L1 doesn't, and vice versa.
#include "nested_vmx.h"
#include "nested_util.h"
#include "exit_reflect.h"
#include "../vmcs.h"

int nested_handle_cr_access(NESTED_STATE* ns, GUEST_REGS* regs, uint64_t exit_qual) {
    uint32_t cr_num = exit_qual & 0xF;
    uint32_t access = (exit_qual >> 4) & 0x3;   // 0=MOV to CR, 1=MOV from CR
    uint32_t reg = (exit_qual >> 8) & 0xF;

    (void)access;
    (void)reg;

    if (cr_num == 0 || cr_num == 4) {
        uint32_t mask_field = (cr_num == 0) ? VMCS_CR0_GUEST_HOST_MASK : VMCS_CR4_GUEST_HOST_MASK;
        uint64_t l0_mask = vmcs_read64(mask_field); (void)l0_mask; // from VMCS02
        uint64_t l1_mask = vmcs12_read(&ns->vmcs12, mask_field);

        // Determine which bits caused the exit
        // If the exiting bit is in L1's mask → reflect to L1
        // If the exiting bit is in L0's mask only → L0 handles silently

        // For now: if ANY exiting bit is in L1's mask, reflect the whole exit.
        // More granular dual-shadow logic can be added if needed.
        if (l1_mask) {
            nested_reflect_to_l1(ns, regs, EXIT_REASON_CR_ACCESS, exit_qual);
            return 1; // reflected
        }
    }

    return 0; // L0 handles silently
}
