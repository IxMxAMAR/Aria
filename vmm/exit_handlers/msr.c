// vmm/exit_handlers/msr.c — MSR read/write interception
#include "../vmm.h"
#include "../vmcs.h"
#include "../../efi/vmx_init.h"
#include "../../vmm/nested/hv_interface.h"
#include "../../vmm/nested/nested_vmx.h"

void handle_msr_read(GUEST_REGS* regs) {
    uint32_t msr = (uint32_t)regs->rcx;
    uint64_t val;

    // Check Hyper-V synthetic MSRs first
    if (g_vmm.nested) {
        uint64_t hv_val;
        if (hv_handle_msr_read(g_vmm.nested, msr, &hv_val)) {
            regs->rax = hv_val & 0xFFFFFFFF;
            regs->rdx = hv_val >> 32;
            return;
        }
    }

    switch (msr) {
    case IA32_FEATURE_CONTROL:
        val = __readmsr(IA32_FEATURE_CONTROL);
        val |= FEATURE_CONTROL_VMXON | FEATURE_CONTROL_LOCK;
        break;

    default:
        val = __readmsr(msr);
        break;
    }

    regs->rax = val & 0xFFFFFFFF;
    regs->rdx = val >> 32;
}

void handle_msr_write(GUEST_REGS* regs) {
    uint32_t msr = (uint32_t)regs->rcx;
    uint64_t val = ((uint64_t)(uint32_t)regs->rdx << 32) | ((uint32_t)regs->rax);

    // Check Hyper-V synthetic MSRs
    if (g_vmm.nested) {
        if (hv_handle_msr_write(g_vmm.nested, msr, val))
            return;
    }

    switch (msr) {
    case IA32_FEATURE_CONTROL:
        break; // swallow

    default:
        __writemsr(msr, val);
        break;
    }
}
