// vmm/nested/interrupt_nested.c — Nested interrupt/NMI/interrupt-window (spec §5.3, §5.6, §5.7)
#include "interrupt_nested.h"
#include "exit_reflect.h"
#include "nested_util.h"
#include "../vmcs.h"
#include "../../efi/serial.h"

void nested_handle_external_interrupt(NESTED_STATE* ns, GUEST_REGS* regs, uint32_t vector) {
    // Spec §5.3: External interrupt while L2 runs
    // 1. L0 acknowledges and handles the interrupt (re-inject to host IDT)
    // 2. After handling: check if L1 wants external-interrupt exiting
    // 3. If yes: reflect to L1 (L1 decides whether to inject to L2 or handle at VTL1)
    // 4. If no: L0 injects directly to L2

    int l1_wants_ext_int = vmcs12_control_bit_set(
        &ns->vmcs12, VMCS_PIN_BASED_EXEC_CTRL, 0);

    if (l1_wants_ext_int) {
        uint64_t qual = vmcs_read64(VMCS_EXIT_QUALIFICATION);
        nested_reflect_to_l1(ns, regs, EXIT_REASON_EXTERNAL_INT, qual);
    }
    // If L1 doesn't want them: the caller in vmm.c re-injects to L2
    (void)vector;
}

void nested_handle_nmi(NESTED_STATE* ns, GUEST_REGS* regs) {
    // Spec §5.6: NMI while L2 runs
    // NMIs use separate controls from external interrupts.

    int l1_wants_nmi = vmcs12_control_bit_set(
        &ns->vmcs12, VMCS_PIN_BASED_EXEC_CTRL, 3);

    if (l1_wants_nmi) {
        // CRITICAL: Set NMI-blocking bit in reflected interruptibility state.
        // After NMI delivery, NMIs are blocked until IRET (or virtual NMI unblock).
        uint32_t interruptibility = vmcs_read32(VMCS_GUEST_INTERRUPTIBILITY);
        interruptibility |= (1u << 3); // bit 3 = blocking by NMI
        vmcs_write32(VMCS_GUEST_INTERRUPTIBILITY, interruptibility);

        // Check if L1 uses virtual NMIs (pin-based bit 5)
        // If yes: NMI blocking is controlled by interruptibility bit 3, not IRET
        // The bit is already set above — correct for both real and virtual NMIs.

        uint64_t qual = vmcs_read64(VMCS_EXIT_QUALIFICATION);
        nested_reflect_to_l1(ns, regs, EXIT_REASON_EXCEPTION_NMI, qual);
    }
    // If L1 doesn't want NMI exiting: re-inject NMI to L2 via VM-entry interrupt info
}

void nested_handle_interrupt_window(NESTED_STATE* ns, GUEST_REGS* regs) {
    // Spec §5.7: Interrupt-window exit from L2
    // L1 uses this for virtual interrupt delivery.
    // Reflect to L1, then track the bit to prevent livelock.

    int l1_wants_intwin = vmcs12_control_bit_set(
        &ns->vmcs12, VMCS_PROC_BASED_EXEC_CTRL, 2);

    if (l1_wants_intwin) {
        uint64_t qual = vmcs_read64(VMCS_EXIT_QUALIFICATION);
        nested_reflect_to_l1(ns, regs, 7, qual); // exit reason 7 = interrupt window

        // After reflecting: on VMCS02 rebuild, only set interrupt-window bit
        // if L1's VMCS12 STILL has it set AND L2's RFLAGS.IF = 1.
        // This is handled in vmcs_merge_build_vmcs02 by checking VMCS12 controls.
        // The UNION_OR merge naturally handles this — if L1 clears the bit
        // in VMCS12 after injecting the interrupt, the merged VMCS02 won't have it.
    }
}
