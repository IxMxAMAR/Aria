// vmm/nested/exit_dispatch.c — L2 exit classification (spec §5.1)
// Decision tree:
// 1. Our hook (CPUID magic, EPT split) → L0 handles (return 0)
// 2. L1 requested (check VMCS12 controls) → reflect (return 1)
// 3. Neither → L0 handles transparently (return 0)
#include "exit_dispatch.h"
#include "nested_util.h"
#include "../vmcs.h"
#include "../../shared/protocol.h"
#include "../../efi/serial.h"

int nested_classify_exit(NESTED_STATE* ns, GUEST_REGS* regs,
                         uint32_t exit_reason, uint64_t exit_qual)
{
    (void)exit_qual;

    // ================================================================
    // Priority 1: Our L0 hooks
    // ================================================================

    if (exit_reason == EXIT_REASON_CPUID) {
        uint32_t leaf = (uint32_t)regs->rax;
        if (leaf == ARIA_CPUID_MAGIC) {
            return 0; // L0 handles our covert channel
        }
    }

    if (exit_reason == EXIT_REASON_EPT_VIOLATION) {
        // L0 handles: shadow EPT composition and hook flip are done
        // in vmm_handle_exit's L2 block BEFORE classify is called.
        // If we reach here, this EPT violation wasn't handled by L0.
        // Check if L1 wants it reflected (which it usually does for its own EPT violations).
        // Fall through to Priority 2 which returns 1 (reflect).
    }

    if (exit_reason == EXIT_REASON_MTF) {
        // MTF from our EPT split single-step pattern
        return 0; // L0 handles
    }

    // EPT misconfiguration — check for cross-core INVEPT race (spec §14.2)
    if (exit_reason == EXIT_REASON_EPT_MISCONFIG) {
        uint64_t gpa = vmcs_read64(VMCS_GUEST_PHYS_ADDR);
        // Check if this GPA is in a shadow EPT that's being invalidated
        // If yes: recompose (return 0). If no: genuine misconfig (return 1).
        (void)gpa;
        // TODO: implement race detection
        return 1; // default: reflect to L1
    }

    // ================================================================
    // Priority 2: L1 requested this exit (check VMCS12 controls)
    // ================================================================

    switch (exit_reason) {

    case EXIT_REASON_CPUID:
        // CPUID always causes VM exit — no control bit to check
        // Always reflect to L1 (unless it's our magic, checked by L0 first)
        return 1;

    case EXIT_REASON_EXTERNAL_INT:
        if (vmcs12_control_bit_set(&ns->vmcs12, VMCS_PIN_BASED_EXEC_CTRL, 0))
            return 1;
        break;

    case EXIT_REASON_EXCEPTION_NMI: {
        // Check exception bitmap for the specific vector
        uint32_t intr_info = vmcs_read32(VMCS_VM_EXIT_INTR_INFO);
        uint32_t vector = intr_info & 0xFF;
        uint32_t type = (intr_info >> 8) & 0x7;

        if (type == 2) {
            // NMI — check pin-based NMI exiting
            if (vmcs12_control_bit_set(&ns->vmcs12, VMCS_PIN_BASED_EXEC_CTRL, 3))
                return 1;
        } else {
            // Exception — check exception bitmap
            uint32_t bitmap = (uint32_t)vmcs12_read(&ns->vmcs12, VMCS_EXCEPTION_BITMAP);
            if (bitmap & (1u << vector))
                return 1;
        }
        break;
    }

    case EXIT_REASON_HLT:
        if (vmcs12_control_bit_set(&ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 7))
            return 1;
        break;

    case EXIT_REASON_VMCALL:
        // Always reflect VMCALL to L1 (Hyper-V hypercalls)
        return 1;

    case 22: // EXIT_REASON_VMPTRST
        return 1; // reflect to L1

    case EXIT_REASON_CR_ACCESS: {
        // Check if L1 requested this CR intercept
        uint64_t qual = vmcs_read64(VMCS_EXIT_QUALIFICATION);
        uint32_t cr_num = qual & 0xF;
        uint32_t access_type = (qual >> 4) & 0x3;

        if (cr_num == 3 && access_type == 0) {
            // MOV to CR3 — reflect if L1 set CR3-load exiting (bit 15)
            if (vmcs12_control_bit_set(&ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 15))
                return 1;
        } else if (cr_num == 3 && access_type == 1) {
            // MOV from CR3 — reflect if L1 set CR3-store exiting (bit 16)
            if (vmcs12_control_bit_set(&ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 16))
                return 1;
        } else if (cr_num == 0 || cr_num == 4) {
            // CR0/CR4 — check L1's guest-host mask for the modified bit
            uint64_t l1_mask = (cr_num == 0) ?
                vmcs12_read(&ns->vmcs12, VMCS_CR0_GUEST_HOST_MASK) :
                vmcs12_read(&ns->vmcs12, VMCS_CR4_GUEST_HOST_MASK);
            uint32_t reg_idx = (qual >> 8) & 0xF;
            (void)reg_idx;
            // If L1 masks any bit, conservatively reflect
            if (l1_mask != 0) return 1;
        } else if (cr_num == 8) {
            // CR8/TPR — reflect if L1 set CR8-load/store exiting
            if (access_type == 0 && vmcs12_control_bit_set(&ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 19))
                return 1;
            if (access_type == 1 && vmcs12_control_bit_set(&ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 20))
                return 1;
        }
        return 0; // L0 handles transparently
    }

    case EXIT_REASON_MSR_READ:
    case EXIT_REASON_MSR_WRITE:
        // Check L1's MSR bitmap for the specific MSR
        // TODO: check composed MSR bitmap
        return 1; // conservative: reflect all MSR exits

    case EXIT_REASON_PREEMPT_TIMER:
        return 1; // always reflect to L1 (it uses timer for scheduling)

    case EXIT_REASON_EPT_VIOLATION:
        // If we didn't handle it as our hook above, reflect to L1
        return 1;

    case EXIT_REASON_RDTSCP:
        // RDTSCP exits when primary RDTSC exiting (bit 12) is set
        if (vmcs12_control_bit_set(&ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 12))
            return 1;
        break;

    case EXIT_REASON_XSETBV:
        return 0; // L0 handles XSETBV transparently

    default:
        break;
    }

    // ================================================================
    // Priority 3: L0 handles transparently
    // ================================================================
    return 0;
}
