// vmm/exit_handlers/cr_access.c — control-register access handling
//
// An exit is produced only for the bits set in CR0/CR4_GUEST_HOST_MASK. This
// build masks CR4.VMXE only, so the reachable cases are a guest MOV to CR4 that
// would change VMXE, and any guest read of CR4.
#include "../vmm.h"
#include "../vmcs.h"
#include "../../efi/vmx_init.h"

// Exit qualification layout for control-register accesses (SDM Table 27-3):
//   bits 3:0   control register number (4 = CR4)
//   bits 5:4   access type: 0 = MOV to CR, 1 = MOV from CR, 2 = CLTS, 3 = LMSW
//   bits 11:8  general-purpose register
//   bits 15:12 source data (LMSW only)
#define CR_ACCESS_MOV_TO_CR   0
#define CR_ACCESS_MOV_FROM_CR 1

// Register encoding 4 is RSP. The exit stub does not capture RSP into
// GUEST_REGS (that slot holds host stack contents), so RSP must be read from
// and written to the VMCS instead of the register table.
#define GPR_ENCODING_RSP 4

void handle_cr_access(GUEST_REGS* regs) {
    uint64_t qualification = vmcs_read64(VMCS_EXIT_QUALIFICATION);

    uint32_t cr_num = (uint32_t)(qualification & 0xF);
    uint32_t access = (uint32_t)((qualification >> 4) & 0x3);
    uint32_t reg    = (uint32_t)((qualification >> 8) & 0xF);

    // Only CR4 is masked in this build; nothing else can reach here.
    if (cr_num != 4) return;
    if (reg > 15) return;

    uint64_t* gpr_table[16] = {
        &regs->rax, &regs->rcx, &regs->rdx, &regs->rbx,
        NULL,       &regs->rbp, &regs->rsi, &regs->rdi,
        &regs->r8,  &regs->r9,  &regs->r10, &regs->r11,
        &regs->r12, &regs->r13, &regs->r14, &regs->r15
    };
    uint64_t* gpr = (reg == GPR_ENCODING_RSP) ? NULL : gpr_table[reg];

    if (access == CR_ACCESS_MOV_TO_CR) {
        uint64_t requested = (gpr == NULL) ? vmcs_read64(VMCS_GUEST_RSP) : *gpr;

        // Reject bits the CPU does not implement, then force the bits it
        // requires. Skipping this check lets a guest write a CR4 that makes the
        // next VM entry fail, which would halt the host.
        uint64_t fixed0 = __readmsr(IA32_VMX_CR4_FIXED0);
        uint64_t fixed1 = __readmsr(IA32_VMX_CR4_FIXED1);
        uint64_t real_cr4 = (requested | fixed0) & fixed1;

        // VMXE stays set in the real CR4 (fixed0 guarantees it) but is hidden
        // from the guest's view.
        vmcs_write64(VMCS_GUEST_CR4, real_cr4);
        vmcs_write64(VMCS_CR4_READ_SHADOW, real_cr4 & ~((uint64_t)CR4_VMXE));
        return;
    }

    if (access == CR_ACCESS_MOV_FROM_CR) {
        // When a control-register read exits, the CPU does not write the
        // destination register; the handler must supply the value.
        uint64_t value = vmcs_read64(VMCS_CR4_READ_SHADOW);
        if (gpr == NULL) {
            vmcs_write64(VMCS_GUEST_RSP, value);
        } else {
            *gpr = value;
        }
        return;
    }

    // CLTS (2) and LMSW (3) do not apply to CR4.
}
