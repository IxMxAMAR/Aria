// vmm/nested/hv_interface.c — Hyper-V synthetic MSR + CPUID impersonation (spec §8)
// Handles the 25 synthetic MSRs needed for VBS boot.
// L1 (Hyper-V) writes these during initialization to set up:
// - Hypercall page (VMCALL trampoline)
// - Reference TSC page
// - SynIC (synthetic interrupt controller)
// - Synthetic timers
#include "hv_interface.h"
#include "../mem_engine.h"
#include "../../efi/serial.h"

// ================================================================
// CPUID impersonation — present as Microsoft Hyper-V (spec §8.2)
// ================================================================

void hv_cpuid_impersonate(uint32_t leaf, uint32_t subleaf, GUEST_REGS* regs) {
    (void)subleaf;

    switch (leaf) {
    case 0x40000000:
        // Hypervisor vendor: "Microsoft Hv"
        regs->rax = 0x40000006; // max hypervisor CPUID leaf
        regs->rbx = 0x7263694D; // "Micr"
        regs->rcx = 0x666F736F; // "osof"
        regs->rdx = 0x76482074; // "t Hv"
        break;

    case 0x40000001:
        // Hyper-V interface identification
        regs->rax = 0x31237648; // "Hv#1"
        regs->rbx = 0;
        regs->rcx = 0;
        regs->rdx = 0;
        break;

    case 0x40000002:
        // Hyper-V version (synthetic — use a plausible build number)
        regs->rax = 0x00003001; // build number
        regs->rbx = 0x000A0000; // version 10.0
        regs->rcx = 0;
        regs->rdx = 0;
        break;

    case 0x40000003:
        // Feature/enlightenment recommendations
        // Minimal set to force Hyper-V to use standard VMX code paths
        regs->rax = (1 << 5)    // recommend relaxed timing
                  | (1 << 3)    // AccessVpIndex available
                  | (1 << 1);   // AccessHypercallMsrs available
        regs->rbx = 0;
        regs->rcx = 0;
        regs->rdx = 0;
        break;

    case 0x40000004:
        // Implementation limits
        regs->rax = 64;  // max virtual processors
        regs->rbx = 64;  // max logical processors
        regs->rcx = 0;
        regs->rdx = 0;
        break;

    case 0x40000005:
    case 0x40000006:
        regs->rax = 0;
        regs->rbx = 0;
        regs->rcx = 0;
        regs->rdx = 0;
        break;
    }
}

// ================================================================
// Synthetic MSR handlers (spec §8.3)
// ================================================================

int hv_handle_msr_read(NESTED_STATE* ns, uint32_t msr, uint64_t* value) {
    switch (msr) {
    case HV_X64_MSR_GUEST_OS_ID:
        *value = ns->hv.guest_os_id;
        return 1;
    case HV_X64_MSR_HYPERCALL:
        *value = ns->hv.hypercall_msr;
        return 1;
    case HV_X64_MSR_VP_INDEX:
        // Return current virtual processor index (= APIC ID for simplicity)
        *value = 0; // TODO: return actual VP index from per-CPU state
        return 1;
    case HV_X64_MSR_REFERENCE_TSC:
        *value = ns->hv.reference_tsc_gpa | 1; // bit 0 = enabled
        return 1;
    case HV_X64_MSR_SCONTROL:
        *value = ns->hv.scontrol;
        return 1;
    case HV_X64_MSR_SVERSION:
        *value = 1; // SynIC version 1
        return 1;
    case HV_X64_MSR_SIEFP:
        *value = ns->hv.siefp;
        return 1;
    case HV_X64_MSR_SIMP:
        *value = ns->hv.simp;
        return 1;
    case HV_X64_MSR_EOM:
        *value = 0;
        return 1;
    default:
        // SINT0-15
        if (msr >= HV_X64_MSR_SINT0 && msr <= HV_X64_MSR_SINT15) {
            *value = ns->hv.sint[msr - HV_X64_MSR_SINT0];
            return 1;
        }
        // STIMER0-3 config/count pairs
        if (msr >= HV_X64_MSR_STIMER0_CONFIG && msr <= HV_X64_MSR_STIMER3_COUNT) {
            uint32_t timer_idx = (msr - HV_X64_MSR_STIMER0_CONFIG) / 2;
            int is_count = (msr - HV_X64_MSR_STIMER0_CONFIG) % 2;
            if (timer_idx < 4) {
                *value = is_count ? ns->hv.stimer_count[timer_idx]
                                  : ns->hv.stimer_config[timer_idx];
                return 1;
            }
        }
        break;
    }
    return 0; // not a Hyper-V MSR
}

int hv_handle_msr_write(NESTED_STATE* ns, uint32_t msr, uint64_t value) {
    switch (msr) {
    case HV_X64_MSR_GUEST_OS_ID:
        ns->hv.guest_os_id = value;
        DBG_PRINT("[hv] GUEST_OS_ID = ");
        DBG_HEX(value);
        DBG_PRINT("\n");
        return 1;

    case HV_X64_MSR_HYPERCALL: {
        ns->hv.hypercall_msr = value;
        uint64_t gpa = value & ~0xFFFULL;
        int enabled = value & 1;
        ns->hv.hypercall_page_gpa = gpa;

        if (enabled && gpa) {
            // Allocate and map the hypercall trampoline page
            // The page contains: VMCALL; RET
            // When L2 calls this page, VMCALL exits to L0, we reflect to L1

            // For now: we assume the page is already mapped by L1's EPT.
            // We write the trampoline bytes at the GPA's physical address.
            uint64_t hpa = gpa; // L1 GPA == HPA (identity mapped)
            // ASSUMPTION: L0's host page tables identity-map all physical RAM.
            // This holds for EFI RuntimeServicesData allocations and main RAM.
            // If Hyper-V places pages in MMIO or unmapped regions, this breaks.
            uint8_t trampoline[] = { 0x0F, 0x01, 0xC1, 0xC3 }; // VMCALL; RET
            vmm_memcpy((void*)hpa, trampoline, sizeof(trampoline));

            ns->hv.hypercall_page_hpa = hpa;
            DBG_PRINT("[hv] Hypercall page at GPA=");
            DBG_HEX(gpa);
            DBG_PRINT(" (trampoline written)\n");
        }
        return 1;
    }

    case HV_X64_MSR_REFERENCE_TSC: {
        uint64_t gpa = value & ~0xFFFULL;
        ns->hv.reference_tsc_gpa = gpa;
        if (gpa) {
            // Write reference TSC page structure
            uint64_t hpa = gpa; // identity mapped
            // ASSUMPTION: L0's host page tables identity-map all physical RAM.
            // This holds for EFI RuntimeServicesData allocations and main RAM.
            // If Hyper-V places pages in MMIO or unmapped regions, this breaks.
            // HV_REFERENCE_TSC_PAGE: { uint32_t tsc_sequence; uint64_t tsc_scale; int64_t tsc_offset }
            uint32_t* page = (uint32_t*)hpa;
            page[0] = 1; // tsc_sequence (nonzero = valid)
            // tsc_scale and tsc_offset: use default (1:1 mapping)
            uint64_t* scale = (uint64_t*)(page + 2);
            *scale = 0x100000000ULL; // 1.0 in fixed-point (scale factor = 1)
            int64_t* offset = (int64_t*)(scale + 1);
            *offset = 0; // no offset

            ns->hv.reference_tsc_hpa = hpa;
            DBG_PRINT("[hv] Reference TSC page at GPA=");
            DBG_HEX(gpa);
            DBG_PRINT("\n");
        }
        return 1;
    }

    case HV_X64_MSR_SCONTROL:
        ns->hv.scontrol = value;
        return 1;
    case HV_X64_MSR_SIEFP:
        ns->hv.siefp = value;
        return 1;
    case HV_X64_MSR_SIMP:
        ns->hv.simp = value;
        return 1;
    case HV_X64_MSR_EOM:
        // End of message — trigger interrupt scan (no-op for now)
        return 1;

    default:
        if (msr >= HV_X64_MSR_SINT0 && msr <= HV_X64_MSR_SINT15) {
            ns->hv.sint[msr - HV_X64_MSR_SINT0] = value;
            return 1;
        }
        if (msr >= HV_X64_MSR_STIMER0_CONFIG && msr <= HV_X64_MSR_STIMER3_COUNT) {
            uint32_t timer_idx = (msr - HV_X64_MSR_STIMER0_CONFIG) / 2;
            int is_count = (msr - HV_X64_MSR_STIMER0_CONFIG) % 2;
            if (timer_idx < 4) {
                if (is_count) ns->hv.stimer_count[timer_idx] = value;
                else          ns->hv.stimer_config[timer_idx] = value;
                return 1;
            }
        }
        break;
    }
    return 0;
}
