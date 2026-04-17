// vmm/nested/nested_vmx.c — Nested VMX lifecycle management
// Handles all VMX instructions from L1 (Hyper-V): VMXON, VMXOFF,
// VMPTRLD, VMCLEAR, VMWRITE, VMREAD, VMLAUNCH, VMRESUME, INVEPT, INVVPID.
// Spec §2.2, §3, §4, §9
#include "nested_vmx.h"
#include "nested_util.h"
#include "vmcs_shadow.h"
#include "vpid_mgr.h"
#include "vm_entry.h"
#include "../vmcs.h"
#include "../mem_engine.h"
#include "../../efi/serial.h"

// ================================================================
// RFLAGS manipulation for VMX success/failure
// ================================================================

// VMX success: clear CF, PF, AF, ZF, SF, OF
static void vmx_succeed(void) {
    uint64_t rflags = vmcs_read64(VMCS_GUEST_RFLAGS);
    rflags &= ~(1ULL | (1ULL<<2) | (1ULL<<4) | (1ULL<<6) | (1ULL<<7) | (1ULL<<11));
    vmcs_write64(VMCS_GUEST_RFLAGS, rflags);
}

// VMX failure with valid VMCS: set ZF=1, CF=0, write error code
static void vmx_fail_valid(NESTED_STATE* ns, uint32_t error_code) {
    uint64_t rflags = vmcs_read64(VMCS_GUEST_RFLAGS);
    rflags &= ~(1ULL | (1ULL << 2) | (1ULL << 4) | (1ULL << 7) | (1ULL << 11));
    rflags |= (1ULL << 6); // ZF=1
    vmcs_write64(VMCS_GUEST_RFLAGS, rflags);
    // Write error code to software VMCS12 (served via VMREAD intercept)
    vmcs12_write(&ns->vmcs12, 0x4400, error_code);
}

// VMX failure without valid VMCS: set CF=1, ZF=0
static void vmx_fail_invalid(void) {
    uint64_t rflags = vmcs_read64(VMCS_GUEST_RFLAGS);
    rflags |= 1ULL;         // CF = 1
    rflags &= ~(1ULL<<6);   // ZF = 0
    vmcs_write64(VMCS_GUEST_RFLAGS, rflags);
}

// ================================================================
// GPR table for operand decoding
// ================================================================

static uint64_t* get_gpr(GUEST_REGS* regs, uint32_t index) {
    static uint64_t rsp_storage = 0;
    switch (index) {
    case 0:  return &regs->rax;
    case 1:  return &regs->rcx;
    case 2:  return &regs->rdx;
    case 3:  return &regs->rbx;
    case 4:
        // RSP comes from VMCS guest state, not GUEST_REGS
        // NOTE: writes to this pointer won't auto-update VMCS_GUEST_RSP
        rsp_storage = vmcs_read64(VMCS_GUEST_RSP);
        return &rsp_storage;
    case 5:  return &regs->rbp;
    case 6:  return &regs->rsi;
    case 7:  return &regs->rdi;
    case 8:  return &regs->r8;
    case 9:  return &regs->r9;
    case 10: return &regs->r10;
    case 11: return &regs->r11;
    case 12: return &regs->r12;
    case 13: return &regs->r13;
    case 14: return &regs->r14;
    case 15: return &regs->r15;
    default: return &rsp_storage;
    }
}

// ================================================================
// Lifecycle
// ================================================================

void nested_init(NESTED_STATE* ns) {
    vmm_memset(ns, 0, sizeof(NESTED_STATE));
    DBG_PRINT("[nested] State initialized\n");
}

void nested_teardown(NESTED_STATE* ns) {
    if (ns->l0_vpid_translated) {
        vpid_free(ns, ns->l1_vpid);
    }

    // Disable VMCS shadowing if it was enabled
    if (ns->vmx_enabled) {
        uint32_t procbased2 = vmcs_read32(VMCS_PROC_BASED_EXEC_CTRL2);
        procbased2 &= ~(1u << 14);
        vmcs_write32(VMCS_PROC_BASED_EXEC_CTRL2, procbased2);
        vmcs_write64(VMCS_VMCS_LINK_PTR, 0xFFFFFFFFFFFFFFFFULL);
    }

    vmm_memset(ns, 0, sizeof(NESTED_STATE));
    DBG_PRINT("[nested] State torn down\n");
}

// ================================================================
// VMX instruction handlers
// ================================================================

int nested_handle_vmxon(NESTED_STATE* ns, GUEST_REGS* regs) {
    (void)regs;

    if (ns->vmx_enabled) {
        vmx_fail_valid(ns, 15); // VMXON in VMX root
        return 0;
    }

    // VMXON operand is a memory address — read from guest linear address
    uint64_t linear_addr = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
    uint64_t vmxon_pa = 0;
    mem_read_virtual(vmcs_read64(VMCS_GUEST_CR3), linear_addr, &vmxon_pa, 8);
    ns->vmxon_region_gpa = vmxon_pa;
    ns->vmx_enabled = 1;

    // Initialize shadow VMCS
    uint64_t vmx_basic = __readmsr(0x480);
    uint32_t revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFF);
    vmcs_shadow_init(ns, revision_id);
    vmcs_shadow_alloc_bitmaps(ns);
    vmcs_shadow_setup_bitmaps(ns);

    vmx_succeed();
    DBG_PRINT("[nested] VMXON: region GPA=");
    DBG_HEX(vmxon_pa);
    DBG_PRINT("\n");
    return 0;
}

int nested_handle_vmxoff(NESTED_STATE* ns, GUEST_REGS* regs) {
    (void)regs;

    if (!ns->vmx_enabled) {
        vmx_fail_invalid();
        return 0;
    }

    // Zero entire state including VMCS shadowing cleanup
    nested_teardown(ns);
    vmx_succeed();
    DBG_PRINT("[nested] VMXOFF\n");
    return 0;
}

int nested_handle_vmptrld(NESTED_STATE* ns, GUEST_REGS* regs) {
    (void)regs;
    if (!ns->vmx_enabled) { vmx_fail_invalid(); return 0; }

    // VMPTRLD operand: 64-bit memory location containing VMCS physical address
    // Guest linear address from VMCS_GUEST_LINEAR_ADDR (not exit qualification)
    uint64_t linear_addr = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
    uint64_t guest_cr3 = vmcs_read64(VMCS_GUEST_CR3);

    uint64_t vmcs_pa = 0;
    if (mem_read_virtual(guest_cr3, linear_addr, &vmcs_pa, 8) != 0) {
        vmx_fail_valid(ns, 2); // VMPTRLD with invalid physical address
        return 0;
    }

    // If loading a different VMCS than current, clear the old one
    if (ns->vmcs_active && vmcs_pa != ns->current_vmcs_gpa) {
        vmcs12_clear(&ns->vmcs12);
    }

    ns->current_vmcs_gpa = vmcs_pa;
    ns->vmcs_active = 1;

    vmx_succeed();
    DBG_PRINT("[nested] VMPTRLD: VMCS GPA=");
    DBG_HEX(vmcs_pa);
    DBG_PRINT("\n");
    return 0;
}

int nested_handle_vmclear(NESTED_STATE* ns, GUEST_REGS* regs) {
    (void)regs;
    if (!ns->vmx_enabled) { vmx_fail_invalid(); return 0; }

    uint64_t linear_addr = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
    uint64_t guest_cr3 = vmcs_read64(VMCS_GUEST_CR3);
    uint64_t vmcs_pa = 0;
    mem_read_virtual(guest_cr3, linear_addr, &vmcs_pa, 8);

    if (vmcs_pa == ns->current_vmcs_gpa) {
        ns->vmcs_active = 0;
        ns->current_vmcs_gpa = 0;
        vmcs12_clear(&ns->vmcs12);
        ns->l2_running = 0;
    }

    vmx_succeed();
    DBG_PRINT("[nested] VMCLEAR: GPA=");
    DBG_HEX(vmcs_pa);
    DBG_PRINT("\n");
    return 0;
}

int nested_handle_vmwrite(NESTED_STATE* ns, GUEST_REGS* regs) {
    if (!ns->vmx_enabled || !ns->vmcs_active) {
        vmx_fail_invalid();
        return 0;
    }

    // Decode operands from VM-exit instruction information (VMCS field 0x440E)
    uint32_t insn_info = vmcs_read32(VMCS_VM_EXIT_INSN_INFO);

    // Reg2 (bits 31:28): register containing the VMCS field encoding
    uint32_t reg2_idx = (insn_info >> 28) & 0xF;
    uint32_t field_encoding = (uint32_t)*get_gpr(regs, reg2_idx);

    // Source: register (bit 10 = 1) or memory (bit 10 = 0)
    uint64_t value;
    int is_reg = (insn_info >> 10) & 1;

    if (is_reg) {
        // Reg1 (bits 6:3): register containing the value
        uint32_t reg1_idx = (insn_info >> 3) & 0xF;
        value = *get_gpr(regs, reg1_idx);
    } else {
        // Memory operand — effective address from guest linear address
        uint64_t linear_addr = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
        uint64_t guest_cr3 = vmcs_read64(VMCS_GUEST_CR3);
        mem_read_virtual(guest_cr3, linear_addr, &value, 8);
    }

    // Store in software VMCS12 copy (single source of truth)
    vmcs12_write(&ns->vmcs12, field_encoding, value);

    // Also write to shadow VMCS page (so L1's VMREAD sees the value)
    vmcs_shadow_writeback(ns, field_encoding, value);

    vmx_succeed();
    return 0;
}

int nested_handle_vmread(NESTED_STATE* ns, GUEST_REGS* regs) {
    // This should rarely fire — VMREAD bitmap is all 0s (hardware handles).
    // If we get here, the bitmap may not be configured yet, or this is
    // an unusual field. Handle gracefully from our software copy.

    if (!ns->vmx_enabled || !ns->vmcs_active) {
        vmx_fail_invalid();
        return 0;
    }

    uint32_t insn_info = vmcs_read32(VMCS_VM_EXIT_INSN_INFO);

    // Reg2 (bits 31:28): register with field encoding
    uint32_t reg2_idx = (insn_info >> 28) & 0xF;
    uint32_t field_encoding = (uint32_t)*get_gpr(regs, reg2_idx);

    // Read from software copy
    uint64_t value = vmcs12_read(&ns->vmcs12, field_encoding);

    // Write result to destination
    int is_reg = (insn_info >> 10) & 1;
    if (is_reg) {
        uint32_t reg1_idx = (insn_info >> 3) & 0xF;
        *get_gpr(regs, reg1_idx) = value;
        if (reg1_idx == 4)
            vmcs_write64(VMCS_GUEST_RSP, value);
    } else {
        uint64_t linear_addr = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);
        uint64_t guest_cr3 = vmcs_read64(VMCS_GUEST_CR3);
        uint64_t pa = mem_virt_to_phys(guest_cr3, linear_addr);
        if (pa) {
            // Direct physical write (identity-mapped in L0 host page tables)
            *(uint64_t*)pa = value;
        }
    }

    vmx_succeed();
    return 0;
}

int nested_handle_vmlaunch(NESTED_STATE* ns, GUEST_REGS* regs) {
    (void)regs;
    if (!ns->vmx_enabled || !ns->vmcs_active) {
        vmx_fail_invalid();
        return 0;
    }

    DBG_PRINT("[nested] VMLAUNCH from L1\n");

    int result = nested_enter_l2(ns);
    if (result != 0) {
        vmx_fail_valid(ns, result);
        DBG_PRINT("[nested] VMLAUNCH failed: err=");
        DBG_DEC(result);
        DBG_PRINT("\n");
        return 0; // advance RIP, L1 checks RFLAGS
    }

    // L2 is now running — don't advance RIP (we switched VMCS context)
    return -1;
}

int nested_handle_vmresume(NESTED_STATE* ns, GUEST_REGS* regs) {
    // We VMCLEAR+VMLAUNCH every time (spec §4.1 step 9-10),
    // so VMRESUME is handled identically to VMLAUNCH.
    return nested_handle_vmlaunch(ns, regs);
}

int nested_handle_invept(NESTED_STATE* ns, GUEST_REGS* regs) {
    if (!ns->vmx_enabled) { vmx_fail_invalid(); return 0; }

    // INVEPT type from register operand (bits 6:3 of insn_info)
    uint32_t insn_info = vmcs_read32(VMCS_VM_EXIT_INSN_INFO);
    uint32_t reg_idx = (insn_info >> 3) & 0xF;
    uint64_t type = *get_gpr(regs, reg_idx);

    if (type == 1) {
        // Single-context: flush shadow EPT for active EPTP index
        shadow_ept_invalidate(ns, ns->active_eptp_index);
    } else if (type == 2) {
        // Global: flush all shadow EPTs
        shadow_ept_invalidate_all(ns);
    }

    // Execute real INVEPT on our shadow EPTP (this core)
    extern void __invept(uint64_t type, void* descriptor);
    struct { uint64_t eptp; uint64_t reserved; } desc = {0, 0};
    __invept(2, &desc); // global on this core

    // Cross-core shootdown via IPI — set flags for other cores
    // (Full IPI mechanism implemented in M4 when multi-core exit handling is done)

    vmx_succeed();
    return 0;
}

int nested_handle_invvpid(NESTED_STATE* ns, GUEST_REGS* regs) {
    if (!ns->vmx_enabled) { vmx_fail_invalid(); return 0; }

    // Translate L1's VPID and execute real INVVPID
    uint16_t translated = vpid_translate(ns, ns->l1_vpid);

    extern void __invvpid(uint64_t type, void* descriptor);
    struct {
        uint16_t vpid;
        uint16_t pad[3];
        uint64_t linear;
    } desc;
    vmm_memset(&desc, 0, sizeof(desc));
    desc.vpid = translated;

    __invvpid(2, &desc); // global INVVPID with translated VPID

    vmx_succeed();
    return 0;
}
