// vmm/exit_handlers/cr_access.c — CR0/CR4 access handling
#include "../vmm.h"
#include "../vmcs.h"
#include "../../efi/vmx_init.h"

void handle_cr_access(GUEST_REGS* regs) {
    uint64_t qualification = vmcs_read64(VMCS_EXIT_QUALIFICATION);

    uint32_t cr_num = qualification & 0xF;
    uint32_t access = (qualification >> 4) & 0x3;
    uint32_t reg    = (qualification >> 8) & 0xF;

    uint64_t* gpr_table[] = {
        &regs->rax, &regs->rcx, &regs->rdx, &regs->rbx,
        NULL,       &regs->rbp, &regs->rsi, &regs->rdi,
        &regs->r8,  &regs->r9,  &regs->r10, &regs->r11,
        &regs->r12, &regs->r13, &regs->r14, &regs->r15
    };

    if (cr_num == 4) {
        if (access == 0) {
            uint64_t new_cr4 = (reg == 4) ? vmcs_read64(VMCS_GUEST_RSP) : *gpr_table[reg];
            uint64_t real_cr4 = new_cr4 | CR4_VMXE;
            uint64_t shadow_cr4 = new_cr4;

            vmcs_write64(VMCS_GUEST_CR4, real_cr4);
            vmcs_write64(VMCS_CR4_READ_SHADOW, shadow_cr4);
        }
    }
}
