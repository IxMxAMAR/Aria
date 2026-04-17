// vmm/vmm.c — VM exit dispatcher
#include "vmm.h"
#include "vmcs.h"
#include "nested/nested_vmx.h"
#include "nested/shadow_ept.h"
#include "nested/exit_reflect.h"
#include "nested/exit_dispatch.h"
#include "../efi/serial.h"
#include "../shared/protocol.h"

VMM_STATE g_vmm = {0};

// MSVC floating-point support marker — referenced by any code using float/double.
// In freestanding builds without CRT, we provide the symbol ourselves.
int _fltused = 0;

// MSVC generates implicit memset/memcpy calls for large zero-initializations.
// Provide real symbols since we're freestanding (no CRT).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4164)
#pragma function(memset)
#pragma warning(pop)
#endif
void* memset(void* dst, int val, size_t count) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < count; i++) p[i] = (uint8_t)val;
    return dst;
}
void* memcpy(void* dst, const void* src, size_t count) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < count; i++) d[i] = s[i];
    return dst;
}

// Forward declarations for exit handlers
void handle_cpuid_exit(GUEST_REGS* regs);
void handle_ept_violation(GUEST_REGS* regs);
void handle_msr_read(GUEST_REGS* regs);
void handle_msr_write(GUEST_REGS* regs);
// Nested VMX dispatcher — routes VMX instruction exits to the nested engine
// Returns 0 = advance RIP, 1 = don't advance (entered L2)
static int handle_nested_vmx(GUEST_REGS* regs, uint32_t reason) {
    NESTED_STATE* ns = g_vmm.nested;
    if (!ns) {
        // No nested state — return VMfailInvalid to guest
        uint64_t rflags = vmcs_read64(VMCS_GUEST_RFLAGS);
        rflags |= 1ULL; // CF=1
        vmcs_write64(VMCS_GUEST_RFLAGS, rflags);
        return 0;
    }

    switch (reason) {
    case EXIT_REASON_VMXON:
        nested_handle_vmxon(ns, regs);
        return 0;
    case EXIT_REASON_VMXOFF:
        nested_handle_vmxoff(ns, regs);
        return 0;
    case EXIT_REASON_VMPTRLD:
        nested_handle_vmptrld(ns, regs);
        return 0;
    case EXIT_REASON_VMCLEAR:
        nested_handle_vmclear(ns, regs);
        return 0;
    case EXIT_REASON_VMWRITE:
        nested_handle_vmwrite(ns, regs);
        return 0;
    case EXIT_REASON_VMREAD:
        nested_handle_vmread(ns, regs);
        return 0;
    case EXIT_REASON_VMLAUNCH: {
        int r = nested_handle_vmlaunch(ns, regs);
        return (r == -1) ? 1 : 0;  // -1 = entered L2, don't advance
    }
    case EXIT_REASON_VMRESUME: {
        int r = nested_handle_vmresume(ns, regs);
        return (r == -1) ? 1 : 0;
    }
    case EXIT_REASON_VMPTRST: {
        uint64_t linear_addr = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
        uint64_t cr3 = vmcs_read64(VMCS_GUEST_CR3);
        uint64_t dest_pa = mem_virt_to_phys(cr3, linear_addr);
        if (dest_pa) {
            *(uint64_t*)dest_pa = ns->current_vmcs_gpa;
        }
        return 0;
    }
    case EXIT_REASON_INVEPT:
        nested_handle_invept(ns, regs);
        return 0;
    case EXIT_REASON_INVVPID:
        nested_handle_invvpid(ns, regs);
        return 0;
    case EXIT_REASON_VMCALL: {
        // Check for Hyper-V hypercall
        if ((uint32_t)regs->rax == ARIA_CPUID_MAGIC) {
            regs->rax = ARIA_ERR_BAD_CMD;
            return 0;
        }
        // Hyper-V hypercall — return not supported
        regs->rax = 0x0002; // HV_STATUS_INVALID_HYPERCALL_CODE
        return 0;
    }
    default:
        // Unknown VMX exit — succeed silently
        {
            uint64_t rflags = vmcs_read64(VMCS_GUEST_RFLAGS);
            rflags &= ~(1ULL | (1ULL << 6)); // clear CF, ZF
            vmcs_write64(VMCS_GUEST_RFLAGS, rflags);
        }
        return 0;
    }
}
void handle_cr_access(GUEST_REGS* regs);

static void advance_guest_rip(void) {
    uint64_t rip = vmcs_read64(VMCS_GUEST_RIP);
    uint32_t len = vmcs_read32(VMCS_VM_EXIT_INSN_LEN);
    vmcs_write64(VMCS_GUEST_RIP, rip + len);
}

// Global exit counter — readable from EFI main after VMLAUNCH
volatile uint32_t aria_exit_count = 0;
// Track first 16 exit reasons for debugging
volatile uint32_t aria_exit_log[16] = {0};

void vmm_handle_exit(GUEST_REGS* regs) {
    uint32_t exit_reason = vmcs_read32(VMCS_EXIT_REASON) & 0xFFFF;

    // Log exit reason
    uint32_t idx = aria_exit_count;
    if (idx < 16) aria_exit_log[idx] = exit_reason;
    aria_exit_count++;

    // ============ MINIMAL TEST HANDLER ============
    // If this works, the full handler has a bug.
    switch (exit_reason) {
    case EXIT_REASON_EXTERNAL_INT:  // 1
    case EXIT_REASON_EXCEPTION_NMI: // 0
        // Re-inject the NMI/exception if needed
        if (exit_reason == 0) {
            uint32_t intr_info = vmcs_read32(VMCS_VM_EXIT_INTR_INFO);
            vmcs_write32(VMCS_VM_ENTRY_INTR_INFO, intr_info);
            if (intr_info & (1 << 11))
                vmcs_write32(VMCS_VM_ENTRY_EXCEPTION_EC, vmcs_read32(VMCS_VM_EXIT_INTR_EC));
            vmcs_write32(VMCS_VM_ENTRY_INSN_LEN, vmcs_read32(VMCS_VM_EXIT_INSN_LEN));
        }
        break; // external interrupt: just return, guest IDT handles it

    case EXIT_REASON_CPUID: { // 10
        int leaf = (int)regs->rax;
        int sub  = (int)regs->rcx;
        int info[4];
        extern void __cpuidex(int info[4], int leaf, int sub);
        __cpuidex(info, leaf, sub);

        // Hide hypervisor + VMX from guest
        if (leaf == 1) {
            info[2] &= ~(1 << 31); // clear hypervisor bit
            info[2] &= ~(1 << 5);  // clear VMX bit — prevents guest VMXON
        }
        // Block Hyper-V CPUID leaves (0x40000000-0x4000000F)
        if ((uint32_t)leaf >= 0x40000000 && (uint32_t)leaf <= 0x4000000F) {
            info[0] = 0; info[1] = 0; info[2] = 0; info[3] = 0;
        }

        regs->rax = info[0]; regs->rbx = info[1];
        regs->rcx = info[2]; regs->rdx = info[3];
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_MSR_READ: { // 31
        uint32_t msr = (uint32_t)regs->rcx;
        uint64_t val = 0;
        // Hyper-V synthetic MSRs (0x40000000+) don't exist on bare metal.
        // Windows probes these during boot — return 0 instead of GP faulting.
        if (msr >= 0x40000000 && msr <= 0x400000FF) {
            val = 0; // not present
        } else {
            extern uint64_t __readmsr(uint32_t);
            val = __readmsr(msr);
        }
        regs->rax = val & 0xFFFFFFFF;
        regs->rdx = val >> 32;
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_MSR_WRITE: { // 32
        uint32_t msr = (uint32_t)regs->rcx;
        // Ignore writes to Hyper-V synthetic MSRs
        if (msr >= 0x40000000 && msr <= 0x400000FF) {
            // silently ignore
        } else {
            extern void __writemsr(uint32_t, uint64_t);
            __writemsr(msr, (regs->rdx << 32) | (regs->rax & 0xFFFFFFFF));
        }
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_EPT_VIOLATION: // 48
        // For now: just log and halt — shouldn't happen with full identity map
        serial_print("[!] EPT violation at GPA ");
        serial_print_hex(vmcs_read64(VMCS_GUEST_PHYS_ADDR));
        serial_print(" RIP=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_RIP));
        serial_print("\n");
        for (;;) { extern void _mm_pause(void); _mm_pause(); }
        break;

    case EXIT_REASON_EPT_MISCONFIG: // 49
        serial_print("[!] EPT MISCONFIG at GPA ");
        serial_print_hex(vmcs_read64(VMCS_GUEST_PHYS_ADDR));
        serial_print("\n");
        for (;;) { extern void _mm_pause(void); _mm_pause(); }
        break;

    case EXIT_REASON_XSETBV: { // 55
        extern void __xsetbv(uint32_t idx, uint64_t val);
        __xsetbv((uint32_t)regs->rcx, ((uint64_t)regs->rdx << 32) | (regs->rax & 0xFFFFFFFF));
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_CR_ACCESS: // 28
        handle_cr_access(regs);
        advance_guest_rip();
        break;

    case EXIT_REASON_TRIPLE_FAULT: // 2
        serial_print("[!] TRIPLE FAULT in guest at RIP=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_RIP));
        serial_print("\n");
        for (;;) { extern void _mm_pause(void); _mm_pause(); }
        break;

    case EXIT_REASON_INVD: // 13
        // INVD invalidates caches — just advance RIP (safe for our purposes)
        advance_guest_rip();
        break;

    default:
        // Log unexpected exit reasons to serial
        serial_print("[?] Exit reason ");
        serial_print_dec(exit_reason);
        serial_print(" at RIP=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_RIP));
        serial_print("\n");
        advance_guest_rip();
        break;
    }
    return;
}
