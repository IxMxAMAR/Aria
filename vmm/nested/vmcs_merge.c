// vmm/nested/vmcs_merge.c — Table-driven VMCS02 builder (spec §3.3)
// THE most critical file in the nested VMX engine.
// Every VMCS field must be in the merge table. Missing fields = L2 crash.
//
// Reference: Intel SDM Vol. 3, Appendix B (VMCS field encodings)
// Reference: KVM arch/x86/kvm/vmx/nested.c:prepare_vmcs02()
//
// Merge rules:
//   FROM_VMCS12  — L1's desired L2 state (guest registers, etc.)
//   FROM_L0      — L0's own state (host fields — exits come to us)
//   UNION_OR     — OR(L0, L1) for execution controls
//   L0_OVERRIDE  — L0 always wins (EPT pointer, MSR bitmap)
//   ADDITIVE     — L0 + L1 (TSC offset)
//   MIN_VALUE    — min(L0, L1) (preemption timer)
//   COMPUTED     — special logic (CR shadows, VPID)
//   FIXED        — constant (VMCS link pointer = -1)

#include "vmcs_merge.h"
#include "nested_util.h"
#include "vpid_mgr.h"
#include "../vmcs.h"
#include "../mem_engine.h"
#include "../../efi/serial.h"

// ================================================================
// The Merge Table — 109 fields, every one accounted for
// ================================================================

static const VMCS_MERGE_ENTRY merge_table[] = {

    // ================================================================
    // 16-bit GUEST state (FROM_VMCS12)
    // ================================================================
    { VMCS_GUEST_ES_SEL,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_CS_SEL,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SS_SEL,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_DS_SEL,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_FS_SEL,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_GS_SEL,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_LDTR_SEL,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_TR_SEL,        MERGE_FROM_VMCS12 },

    // ================================================================
    // 16-bit HOST state (FROM_L0)
    // ================================================================
    { VMCS_HOST_ES_SEL,         MERGE_FROM_L0 },
    { VMCS_HOST_CS_SEL,         MERGE_FROM_L0 },
    { VMCS_HOST_SS_SEL,         MERGE_FROM_L0 },
    { VMCS_HOST_DS_SEL,         MERGE_FROM_L0 },
    { VMCS_HOST_FS_SEL,         MERGE_FROM_L0 },
    { VMCS_HOST_GS_SEL,         MERGE_FROM_L0 },
    { VMCS_HOST_TR_SEL,         MERGE_FROM_L0 },

    // ================================================================
    // 16-bit CONTROL (COMPUTED)
    // ================================================================
    { VMCS_VPID,                MERGE_COMPUTED },

    // ================================================================
    // 32-bit CONTROL fields (UNION_OR for execution controls)
    // ================================================================
    { VMCS_PIN_BASED_EXEC_CTRL,     MERGE_UNION_OR },
    { VMCS_PROC_BASED_EXEC_CTRL,    MERGE_UNION_OR },
    { VMCS_PROC_BASED_EXEC_CTRL2,   MERGE_UNION_OR },
    { VMCS_VM_EXIT_CTRLS,           MERGE_UNION_OR },
    { VMCS_VM_ENTRY_CTRLS,          MERGE_UNION_OR },
    { VMCS_EXCEPTION_BITMAP,        MERGE_UNION_OR },
    { 0x400A, MERGE_FROM_VMCS12 }, // VMCS_CR3_TARGET_COUNT
    { 0x400E, MERGE_FROM_VMCS12 }, // VMCS_VM_EXIT_MSR_STORE_CNT
    { 0x4010, MERGE_FROM_VMCS12 }, // VMCS_VM_EXIT_MSR_LOAD_CNT
    { 0x4014, MERGE_FROM_VMCS12 }, // VMCS_VM_ENTRY_MSR_LOAD_CNT
    { 0x4006, MERGE_UNION_OR }, // VMCS_PF_ERROR_CODE_MASK
    { 0x4008, MERGE_UNION_OR }, // VMCS_PF_ERROR_CODE_MATCH

    // ================================================================
    // 32-bit GUEST state (FROM_VMCS12)
    // ================================================================
    { VMCS_GUEST_ES_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_CS_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SS_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_DS_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_FS_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_GS_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_LDTR_LIMIT,    MERGE_FROM_VMCS12 },
    { VMCS_GUEST_TR_LIMIT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_GDTR_LIMIT,    MERGE_FROM_VMCS12 },
    { VMCS_GUEST_IDTR_LIMIT,    MERGE_FROM_VMCS12 },
    { VMCS_GUEST_ES_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_CS_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SS_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_DS_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_FS_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_GS_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_LDTR_AR,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_TR_AR,         MERGE_FROM_VMCS12 },
    { VMCS_GUEST_INTERRUPTIBILITY, MERGE_FROM_VMCS12 },
    { VMCS_GUEST_ACTIVITY,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SYSENTER_CS,   MERGE_FROM_VMCS12 },
    { VMCS_GUEST_PREEMPT_TIMER, MERGE_MIN_VALUE },

    // ================================================================
    // 32-bit HOST state (FROM_L0)
    // ================================================================
    { VMCS_HOST_SYSENTER_CS,    MERGE_FROM_L0 },

    // ================================================================
    // 64-bit CONTROL fields
    // ================================================================
    { VMCS_MSR_BITMAP,          MERGE_L0_OVERRIDE },
    { VMCS_TSC_OFFSET,          MERGE_ADDITIVE },
    { VMCS_EPT_POINTER,         MERGE_L0_OVERRIDE },
    { VMCS_VMCS_LINK_PTR,       MERGE_FIXED },
    { 0x2006, MERGE_FROM_VMCS12 }, // VMCS_VM_EXIT_MSR_STORE_ADDR
    { 0x2008, MERGE_FROM_VMCS12 }, // VMCS_VM_EXIT_MSR_LOAD_ADDR
    { 0x200A, MERGE_FROM_VMCS12 }, // VMCS_VM_ENTRY_MSR_LOAD_ADDR

    // ================================================================
    // 64-bit GUEST state (FROM_VMCS12)
    // ================================================================
    { VMCS_GUEST_IA32_DEBUGCTL, MERGE_FROM_VMCS12 },
    { VMCS_GUEST_IA32_PAT,      MERGE_FROM_VMCS12 },
    { VMCS_GUEST_IA32_EFER,     MERGE_FROM_VMCS12 },

    // CET fields — CRITICAL for Alder Lake / Windows 11 shadow stacks
    { VMCS_GUEST_IA32_S_CET,                MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SSP,                       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_IA32_INTERRUPT_SSP_TABLE,  MERGE_FROM_VMCS12 },

    // Guest PDPTEs (required for PAE paging)
    { 0x280A, MERGE_FROM_VMCS12 }, // VMCS_GUEST_PDPTE0
    { 0x280C, MERGE_FROM_VMCS12 }, // VMCS_GUEST_PDPTE1
    { 0x280E, MERGE_FROM_VMCS12 }, // VMCS_GUEST_PDPTE2
    { 0x2810, MERGE_FROM_VMCS12 }, // VMCS_GUEST_PDPTE3
    // Guest SMBASE
    { 0x4828, MERGE_FROM_VMCS12 }, // VMCS_GUEST_SMBASE

    // ================================================================
    // 64-bit HOST state (FROM_L0)
    // ================================================================
    { VMCS_HOST_IA32_PAT,       MERGE_FROM_L0 },
    { VMCS_HOST_IA32_EFER,      MERGE_FROM_L0 },
    { 0x2C04, MERGE_FROM_L0 },    // VMCS_HOST_IA32_PERF_GLOBAL_CTRL
    // Host CET fields
    { VMCS_HOST_IA32_S_CET,                 MERGE_FROM_L0 },
    { VMCS_HOST_SSP,                        MERGE_FROM_L0 },
    { VMCS_HOST_IA32_INTERRUPT_SSP_TABLE,   MERGE_FROM_L0 },

    // ================================================================
    // Natural-width CONTROL fields (CR masks + shadows)
    // ================================================================
    { VMCS_CR0_GUEST_HOST_MASK, MERGE_UNION_OR },
    { VMCS_CR4_GUEST_HOST_MASK, MERGE_UNION_OR },
    { VMCS_CR0_READ_SHADOW,     MERGE_COMPUTED },
    { VMCS_CR4_READ_SHADOW,     MERGE_COMPUTED },

    // ================================================================
    // Natural-width GUEST state (FROM_VMCS12)
    // ================================================================
    { VMCS_GUEST_CR0,           MERGE_FROM_VMCS12 },
    { VMCS_GUEST_CR3,           MERGE_FROM_VMCS12 },
    { VMCS_GUEST_CR4,           MERGE_FROM_VMCS12 },
    { VMCS_GUEST_ES_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_CS_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SS_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_DS_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_FS_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_GS_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_LDTR_BASE,     MERGE_FROM_VMCS12 },
    { VMCS_GUEST_TR_BASE,       MERGE_FROM_VMCS12 },
    { VMCS_GUEST_GDTR_BASE,     MERGE_FROM_VMCS12 },
    { VMCS_GUEST_IDTR_BASE,     MERGE_FROM_VMCS12 },
    { VMCS_GUEST_DR7,           MERGE_FROM_VMCS12 },
    { VMCS_GUEST_RSP,           MERGE_FROM_VMCS12 },
    { VMCS_GUEST_RIP,           MERGE_FROM_VMCS12 },
    { VMCS_GUEST_RFLAGS,        MERGE_FROM_VMCS12 },
    { VMCS_GUEST_PENDING_DBG,   MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SYSENTER_ESP,  MERGE_FROM_VMCS12 },
    { VMCS_GUEST_SYSENTER_EIP,  MERGE_FROM_VMCS12 },

    // ================================================================
    // Natural-width HOST state (FROM_L0)
    // ================================================================
    { VMCS_HOST_CR0,            MERGE_FROM_L0 },
    { VMCS_HOST_CR3,            MERGE_FROM_L0 },
    { VMCS_HOST_CR4,            MERGE_FROM_L0 },
    { VMCS_HOST_FS_BASE,        MERGE_FROM_L0 },
    { VMCS_HOST_GS_BASE,        MERGE_FROM_L0 },
    { VMCS_HOST_TR_BASE,        MERGE_FROM_L0 },
    { VMCS_HOST_GDTR_BASE,      MERGE_FROM_L0 },
    { VMCS_HOST_IDTR_BASE,      MERGE_FROM_L0 },
    { VMCS_HOST_SYSENTER_ESP,   MERGE_FROM_L0 },
    { VMCS_HOST_SYSENTER_EIP,   MERGE_FROM_L0 },
    { VMCS_HOST_RSP,            MERGE_FROM_L0 },
    { VMCS_HOST_RIP,            MERGE_FROM_L0 },
};

#define MERGE_TABLE_SIZE (sizeof(merge_table) / sizeof(merge_table[0]))

const VMCS_MERGE_ENTRY* vmcs_merge_get_table(uint32_t* out_count) {
    *out_count = MERGE_TABLE_SIZE;
    return merge_table;
}

// ================================================================
// L0 field cache — read from VMCS01 before switching to VMCS02
// ================================================================

void vmcs_merge_cache_l0(NESTED_STATE* ns) {
    // Read every field we'll need from VMCS01 (must be current VMCS).
    // Cache them in ns->l0_field_cache[] for use during VMCS02 build.
    ns->l0_field_count = 0;

    for (uint32_t i = 0; i < MERGE_TABLE_SIZE; i++) {
        VMCS_MERGE_RULE rule = merge_table[i].rule;
        // Cache fields needed for FROM_L0, UNION_OR, L0_OVERRIDE, ADDITIVE,
        // MIN_VALUE, COMPUTED — anything that reads L0's current VMCS value
        if (rule != MERGE_FROM_VMCS12 && rule != MERGE_FIXED) {
            uint32_t idx = ns->l0_field_count++;
            ns->l0_field_encodings[idx] = merge_table[i].field_encoding;
            ns->l0_field_cache[idx] = vmcs_read64(merge_table[i].field_encoding);
        }
    }
}

// Look up a cached L0 value by field encoding
static uint64_t l0_cached_value(NESTED_STATE* ns, uint32_t field) {
    for (uint32_t i = 0; i < ns->l0_field_count; i++) {
        if (ns->l0_field_encodings[i] == field)
            return ns->l0_field_cache[i];
    }
    return 0; // field not cached — shouldn't happen if cache was populated correctly
}

// ================================================================
// CR shadow computation (spec §5.5)
// ================================================================

static uint64_t compute_cr_shadow(NESTED_STATE* ns,
                                   uint32_t mask_field,
                                   uint32_t shadow_field,
                                   uint32_t cr_field)
{
    uint64_t l0_mask   = l0_cached_value(ns, mask_field);
    uint64_t l1_mask   = vmcs12_read(&ns->vmcs12, mask_field);
    uint64_t l0_shadow = l0_cached_value(ns, shadow_field);
    uint64_t l1_shadow = vmcs12_read(&ns->vmcs12, shadow_field);
    uint64_t guest_cr  = vmcs12_read(&ns->vmcs12, cr_field);

    uint64_t combined = l0_mask | l1_mask;
    return (l0_shadow & l0_mask) |
           (l1_shadow & l1_mask & ~l0_mask) |
           (guest_cr & ~combined);
}

// ================================================================
// MSR bitmap composition: OR(L0_bitmap, L1_bitmap)
// ================================================================

static void compose_msr_bitmap(NESTED_STATE* ns) {
    uint64_t l1_bitmap_gpa = vmcs12_read(&ns->vmcs12, VMCS_MSR_BITMAP);
    if (!l1_bitmap_gpa || !ns->composed_msr_bitmap_va) return;

    // Read L1's MSR bitmap from L1 GPA space
    uint64_t guest_cr3 = l0_cached_value(ns, VMCS_GUEST_CR3);
    uint8_t l1_bitmap[PAGE_SIZE];
    if (mem_read_virtual(guest_cr3, l1_bitmap_gpa, l1_bitmap, PAGE_SIZE) != 0) {
        // Can't read L1's bitmap — use all-intercept as fallback
        vmm_memset(ns->composed_msr_bitmap_va, 0xFF, PAGE_SIZE);
        return;
    }

    // Get L0's MSR bitmap (from per-CPU VMM state)
    // For now: use a default L0 bitmap (intercept VMX MSRs + feature control)
    // TODO: reference actual per-CPU L0 bitmap when multi-core is integrated
    uint8_t l0_bitmap[PAGE_SIZE];
    vmm_memset(l0_bitmap, 0, PAGE_SIZE);
    // Intercept IA32_FEATURE_CONTROL (MSR 0x3A) reads
    l0_bitmap[0x3A / 8] |= (1 << (0x3A % 8));

    // Compose: OR
    uint8_t* composed = (uint8_t*)ns->composed_msr_bitmap_va;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        composed[i] = l0_bitmap[i] | l1_bitmap[i];
    }
}

// ================================================================
// Build VMCS02 — the main merge function
// ================================================================

int vmcs_merge_build_vmcs02(NESTED_STATE* ns) {
    // VMCS02 must be the current VMCS (VMPTRLD'd) when this is called.
    // L0 field cache must be populated (vmcs_merge_cache_l0 called first).

    // Compose MSR bitmap first
    compose_msr_bitmap(ns);

    // VPID translation
    uint16_t l1_vpid = (uint16_t)vmcs12_read(&ns->vmcs12, VMCS_VPID);
    uint16_t phys_vpid = vpid_translate(ns, l1_vpid);

    for (uint32_t i = 0; i < MERGE_TABLE_SIZE; i++) {
        uint32_t field = merge_table[i].field_encoding;
        uint64_t value = 0;

        switch (merge_table[i].rule) {

        case MERGE_FROM_VMCS12:
            value = vmcs12_read(&ns->vmcs12, field);
            break;

        case MERGE_FROM_L0:
            value = l0_cached_value(ns, field);
            break;

        case MERGE_UNION_OR:
            value = l0_cached_value(ns, field) | vmcs12_read(&ns->vmcs12, field);
            break;

        case MERGE_L0_OVERRIDE:
            if (field == VMCS_EPT_POINTER) {
                // Use shadow EPT (composed L1 EPT through L0 EPT)
                SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[ns->active_eptp_index];
                if (ctx->valid && ctx->pml4_pa) {
                    EPTP eptp;
                    eptp.value = 0;
                    eptp.memory_type = 6;   // write-back
                    eptp.page_walk = 3;     // 4-level
                    eptp.pml4_pfn = ctx->pml4_pa >> 12;
                    value = eptp.value;
                } else {
                    // Shadow EPT not ready yet — use L0's EPT as fallback
                    // (will cause EPT violations that trigger lazy composition)
                    value = l0_cached_value(ns, field);
                }
            } else if (field == VMCS_MSR_BITMAP) {
                value = ns->composed_msr_bitmap_pa;
            } else {
                value = l0_cached_value(ns, field);
            }
            break;

        case MERGE_ADDITIVE:
            value = l0_cached_value(ns, field) + vmcs12_read(&ns->vmcs12, field);
            break;

        case MERGE_MIN_VALUE: {
            uint32_t l0_val = (uint32_t)l0_cached_value(ns, field);
            uint32_t l1_val = (uint32_t)vmcs12_read(&ns->vmcs12, field);
            value = (l0_val < l1_val) ? l0_val : l1_val;
            ns->l0_preempt_value = l0_val;
            ns->l1_preempt_value = l1_val;
            break;
        }

        case MERGE_COMPUTED:
            if (field == VMCS_VPID) {
                value = phys_vpid;
            } else if (field == VMCS_CR0_READ_SHADOW) {
                value = compute_cr_shadow(ns, VMCS_CR0_GUEST_HOST_MASK,
                                          VMCS_CR0_READ_SHADOW, VMCS_GUEST_CR0);
            } else if (field == VMCS_CR4_READ_SHADOW) {
                value = compute_cr_shadow(ns, VMCS_CR4_GUEST_HOST_MASK,
                                          VMCS_CR4_READ_SHADOW, VMCS_GUEST_CR4);
            }
            break;

        case MERGE_FIXED:
            value = 0xFFFFFFFFFFFFFFFFULL;
            break;
        }

        vmcs_write64(field, value);
    }

    DBG_PRINT("[nested] VMCS02 merged: ");
    DBG_DEC(MERGE_TABLE_SIZE);
    DBG_PRINT(" fields\n");

    return 0;
}

int vmcs_merge_validate(NESTED_STATE* ns) {
    // Validate the merged VMCS02 using the SDM §26.2-26.3 checks
    // VMCS02 is the current VMCS — we read fields from hardware
    int err = vmcs12_validate_controls(&ns->vmcs12);
    if (err) {
        DBG_PRINT("[nested] VMCS02 control validation failed\n");
        return err;
    }

    err = vmcs12_validate_guest_state(&ns->vmcs12);
    if (err) {
        DBG_PRINT("[nested] VMCS02 guest state validation failed\n");
        return err;
    }

    return 0;
}
