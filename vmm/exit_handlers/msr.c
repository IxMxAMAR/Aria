// vmm/exit_handlers/msr.c — MSR write interception
//
// Only writes are intercepted (see vmm/msr_bitmap.c). Reads execute natively
// and return the real MSR, which this file keeps identical to the VMCS.
//
// The handler must validate before touching the hardware: it runs in *host*
// context, so a value that would raise #GP on WRMSR faults the hypervisor
// instead of the guest. Anything the guest is not allowed to write has to be
// rejected here and reflected back into the guest as #GP.

#include "../vmm.h"
#include "../vmcs.h"

#include <Library/BaseLib.h>

// IA32_EFER bits (SDM Vol 4, Table 2-2)
#define EFER_SCE   (1ULL << 0)
#define EFER_LME   (1ULL << 8)
#define EFER_LMA   (1ULL << 10)
#define EFER_NXE   (1ULL << 11)

// Inject #GP into the guest at the instruction that caused the exit.
static void inject_gp(void) {
    vmcs_write32(VMCS_VM_ENTRY_INTR_INFO, 0x80000000u | (3u << 8) | 13u);
    vmcs_write32(VMCS_VM_ENTRY_EXCEPTION_EC, 0);
}

// Returns 1 if the guest RIP should be advanced past the WRMSR, 0 if an
// exception was injected and the instruction must be re-executed.
int handle_msr_write(GUEST_REGS* regs) {
    uint32_t msr = (uint32_t)regs->rcx;
    uint64_t val = ((uint64_t)(uint32_t)regs->rdx << 32) | (uint32_t)regs->rax;

    switch (msr) {

    case 0xC0000080: { // IA32_EFER
        uint64_t cur = AsmReadMsr64(msr);
        // LMA is read-only: the processor sets it when long mode is entered, and
        // a WRMSR that attempts to change it raises #GP. Reserved bits do too.
        // The guest may only touch SCE / LME / NXE on top of what is already set.
        uint64_t allowed = cur | EFER_SCE | EFER_LME | EFER_NXE;
        if ((val & ~allowed) != 0 || (val & EFER_LMA) != (cur & EFER_LMA)) {
            inject_gp();
            return 0;
        }
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_IA32_EFER, val);
        return 1;
    }

    case 0xC0000100: // IA32_FS_BASE
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_FS_BASE, val);
        return 1;

    case 0xC0000101: // IA32_GS_BASE
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_GS_BASE, val);
        return 1;

    case 0x00000277: // IA32_PAT
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_IA32_PAT, val);
        return 1;

    case 0x00000174: // IA32_SYSENTER_CS
        AsmWriteMsr64(msr, val);
        vmcs_write32(VMCS_GUEST_SYSENTER_CS, (uint32_t)val);
        return 1;

    case 0x00000175: // IA32_SYSENTER_ESP
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_SYSENTER_ESP, val);
        return 1;

    case 0x00000176: // IA32_SYSENTER_EIP
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_SYSENTER_EIP, val);
        return 1;

    case 0x000001D9: // IA32_DEBUGCTL
        AsmWriteMsr64(msr, val);
        vmcs_write64(VMCS_GUEST_IA32_DEBUGCTL, val);
        return 1;

    default:
        // Not in the intercepted set, so this is only reachable if the bitmap
        // and this switch ever disagree. Pass through rather than drop the write.
        AsmWriteMsr64(msr, val);
        return 1;
    }
}
