// vmm/nested/exit_reflect.c — L2 → L1 state transfer (spec §5.2)
// Saves ALL L2 guest state into VMCS12 so L1 can inspect it.
// This is the most field-count-intensive function — 40+ VMCS fields.
// Reference: Intel SDM §27.2 "Information Saved on VM Exits"
#include "exit_reflect.h"
#include "nested_util.h"
#include "vmcs_shadow.h"
#include "../vmcs.h"
#include "../../efi/serial.h"

extern uint8_t __vmx_vmptrld(uint64_t* pa);
extern uint64_t __rdtsc(void);
extern void __xsave(void* area, uint64_t mask);
extern void __xrstor(void* area, uint64_t mask);

// Save a VMCS02 field into the software VMCS12 and the shadow VMCS page
static void save_field(NESTED_STATE* ns, uint32_t encoding) {
    uint64_t value = vmcs_read64(encoding);
    vmcs12_write(&ns->vmcs12, encoding, value);
    vmcs_shadow_writeback(ns, encoding, value);
}

void nested_reflect_to_l1(NESTED_STATE* ns, GUEST_REGS* regs,
                          uint32_t exit_reason, uint64_t exit_qual)
{
    (void)regs;

    // ================================================================
    // Step 1: Save L2 guest state from VMCS02 → VMCS12
    // SDM §27.2: "Information Saved on VM Exits"
    // ================================================================

    // Guest GP state
    save_field(ns, VMCS_GUEST_RIP);
    save_field(ns, VMCS_GUEST_RSP);
    save_field(ns, VMCS_GUEST_RFLAGS);

    // Guest control registers
    save_field(ns, VMCS_GUEST_CR0);
    save_field(ns, VMCS_GUEST_CR3);
    save_field(ns, VMCS_GUEST_CR4);

    // Guest segment registers (all 8 segments × 4 fields)
    save_field(ns, VMCS_GUEST_ES_SEL);
    save_field(ns, VMCS_GUEST_ES_BASE);
    save_field(ns, VMCS_GUEST_ES_LIMIT);
    save_field(ns, VMCS_GUEST_ES_AR);
    save_field(ns, VMCS_GUEST_CS_SEL);
    save_field(ns, VMCS_GUEST_CS_BASE);
    save_field(ns, VMCS_GUEST_CS_LIMIT);
    save_field(ns, VMCS_GUEST_CS_AR);
    save_field(ns, VMCS_GUEST_SS_SEL);
    save_field(ns, VMCS_GUEST_SS_BASE);
    save_field(ns, VMCS_GUEST_SS_LIMIT);
    save_field(ns, VMCS_GUEST_SS_AR);
    save_field(ns, VMCS_GUEST_DS_SEL);
    save_field(ns, VMCS_GUEST_DS_BASE);
    save_field(ns, VMCS_GUEST_DS_LIMIT);
    save_field(ns, VMCS_GUEST_DS_AR);
    save_field(ns, VMCS_GUEST_FS_SEL);
    save_field(ns, VMCS_GUEST_FS_BASE);
    save_field(ns, VMCS_GUEST_FS_LIMIT);
    save_field(ns, VMCS_GUEST_FS_AR);
    save_field(ns, VMCS_GUEST_GS_SEL);
    save_field(ns, VMCS_GUEST_GS_BASE);
    save_field(ns, VMCS_GUEST_GS_LIMIT);
    save_field(ns, VMCS_GUEST_GS_AR);
    save_field(ns, VMCS_GUEST_LDTR_SEL);
    save_field(ns, VMCS_GUEST_LDTR_BASE);
    save_field(ns, VMCS_GUEST_LDTR_LIMIT);
    save_field(ns, VMCS_GUEST_LDTR_AR);
    save_field(ns, VMCS_GUEST_TR_SEL);
    save_field(ns, VMCS_GUEST_TR_BASE);
    save_field(ns, VMCS_GUEST_TR_LIMIT);
    save_field(ns, VMCS_GUEST_TR_AR);

    // Guest descriptor tables
    save_field(ns, VMCS_GUEST_GDTR_BASE);
    save_field(ns, VMCS_GUEST_GDTR_LIMIT);
    save_field(ns, VMCS_GUEST_IDTR_BASE);
    save_field(ns, VMCS_GUEST_IDTR_LIMIT);

    // Guest DR7
    save_field(ns, VMCS_GUEST_DR7);

    // Guest MSRs
    save_field(ns, VMCS_GUEST_IA32_DEBUGCTL);
    save_field(ns, VMCS_GUEST_IA32_PAT);
    save_field(ns, VMCS_GUEST_IA32_EFER);
    save_field(ns, VMCS_GUEST_SYSENTER_CS);
    save_field(ns, VMCS_GUEST_SYSENTER_ESP);
    save_field(ns, VMCS_GUEST_SYSENTER_EIP);

    // CET fields (Alder Lake)
    save_field(ns, VMCS_GUEST_IA32_S_CET);
    save_field(ns, VMCS_GUEST_SSP);
    save_field(ns, VMCS_GUEST_IA32_INTERRUPT_SSP_TABLE);

    // Guest interruptibility and activity — CRITICAL for NMI/interrupt correctness
    save_field(ns, VMCS_GUEST_INTERRUPTIBILITY);
    save_field(ns, VMCS_GUEST_ACTIVITY);
    save_field(ns, VMCS_GUEST_PENDING_DBG);

    // ================================================================
    // Step 2: Write exit information to VMCS12
    // ================================================================

    vmcs12_write(&ns->vmcs12, VMCS_EXIT_REASON, exit_reason);
    vmcs_shadow_writeback(ns, VMCS_EXIT_REASON, exit_reason);

    vmcs12_write(&ns->vmcs12, VMCS_EXIT_QUALIFICATION, exit_qual);
    vmcs_shadow_writeback(ns, VMCS_EXIT_QUALIFICATION, exit_qual);

    // Exit interrupt info
    uint32_t exit_intr_info = vmcs_read32(VMCS_VM_EXIT_INTR_INFO);
    vmcs12_write(&ns->vmcs12, VMCS_VM_EXIT_INTR_INFO, exit_intr_info);
    vmcs_shadow_writeback(ns, VMCS_VM_EXIT_INTR_INFO, exit_intr_info);

    uint32_t exit_intr_ec = vmcs_read32(VMCS_VM_EXIT_INTR_EC);
    vmcs12_write(&ns->vmcs12, VMCS_VM_EXIT_INTR_EC, exit_intr_ec);
    vmcs_shadow_writeback(ns, VMCS_VM_EXIT_INTR_EC, exit_intr_ec);

    // IDT vectoring info
    uint32_t idt_info = vmcs_read32(VMCS_IDT_VECTORING_INFO);
    vmcs12_write(&ns->vmcs12, VMCS_IDT_VECTORING_INFO, idt_info);
    vmcs_shadow_writeback(ns, VMCS_IDT_VECTORING_INFO, idt_info);

    uint32_t idt_ec = vmcs_read32(VMCS_IDT_VECTORING_EC);
    vmcs12_write(&ns->vmcs12, VMCS_IDT_VECTORING_EC, idt_ec);
    vmcs_shadow_writeback(ns, VMCS_IDT_VECTORING_EC, idt_ec);

    // Instruction length and info
    uint32_t insn_len = vmcs_read32(VMCS_VM_EXIT_INSN_LEN);
    vmcs12_write(&ns->vmcs12, VMCS_VM_EXIT_INSN_LEN, insn_len);
    vmcs_shadow_writeback(ns, VMCS_VM_EXIT_INSN_LEN, insn_len);

    uint32_t insn_info = vmcs_read32(VMCS_VM_EXIT_INSN_INFO);
    vmcs12_write(&ns->vmcs12, VMCS_VM_EXIT_INSN_INFO, insn_info);
    vmcs_shadow_writeback(ns, VMCS_VM_EXIT_INSN_INFO, insn_info);

    // Guest linear and physical addresses
    uint64_t guest_linear = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
    vmcs12_write(&ns->vmcs12, VMCS_GUEST_LINEAR_ADDR, guest_linear);
    vmcs_shadow_writeback(ns, VMCS_GUEST_LINEAR_ADDR, guest_linear);

    uint64_t guest_phys = vmcs_read64(VMCS_GUEST_PHYS_ADDR);
    vmcs12_write(&ns->vmcs12, VMCS_GUEST_PHYS_ADDR, guest_phys);
    vmcs_shadow_writeback(ns, VMCS_GUEST_PHYS_ADDR, guest_phys);

    // ================================================================
    // Step 3: Preemption timer accounting
    // ================================================================
    if (ns->l1_preempt_value > 0) {
        uint64_t now = __rdtsc();
        // Timer shift: read from IA32_VMX_MISC bits 4:0
        uint64_t vmx_misc = __readmsr(0x485);
        uint32_t shift = (uint32_t)(vmx_misc & 0x1F);
        uint64_t ticks_elapsed = (now - ns->l2_entry_tsc) >> shift;

        uint32_t remaining = 0;
        if (ticks_elapsed < ns->l1_preempt_value) {
            remaining = ns->l1_preempt_value - (uint32_t)ticks_elapsed;
        }
        vmcs12_write(&ns->vmcs12, VMCS_GUEST_PREEMPT_TIMER, remaining);
        vmcs_shadow_writeback(ns, VMCS_GUEST_PREEMPT_TIMER, remaining);
    }

    // ================================================================
    // Step 4: XSAVE L2 / XRSTOR L1 FPU state
    // ================================================================
    __xsave(ns->l2_xsave_area, 0x7);
    __xrstor(ns->l1_xsave_area, 0x7);

    // ================================================================
    // Step 5: Switch from VMCS02 back to VMCS01
    // ================================================================
    ns->l2_running = 0;
    __vmx_vmptrld(&ns->vmcs01_pa);

    // VMCS01 is now current — bulk sync dirty VMCS12 fields to shadow page
    vmcs_shadow_sync_to_hardware(ns);

    // L1 will resume and handle the reflected exit.
    // The caller (vmm_handle_exit in vmm.c) will VMRESUME into L1.

    DBG_PRINT("[nested] Reflected exit to L1: reason=");
    DBG_DEC(exit_reason);
    DBG_PRINT("\n");
}
