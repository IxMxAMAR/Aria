// vmm/nested/nested_vmx.h — Core types for the Aria nested VMX engine
// Reference: Intel SDM Vol. 3, Chapters 24-34; KVM arch/x86/kvm/vmx/nested.c
#pragma once

#include "../vmm.h"
#include "../../shared/protocol.h"

// ============================================================
// Constants
// ============================================================

#define NESTED_MAX_VPID         256
#define NESTED_VPID_BASE        100     // L0 VPIDs 100+ for L2 guests
#define NESTED_MAX_EPTP_LIST    512     // VMFUNC EPTP switching list entries
#define NESTED_SHADOW_EPT_POOL  (64 * 1024 * 1024)  // 64MB shadow EPT pool
#define NESTED_VMCS12_SIZE      4096    // Software VMCS12 page

// ============================================================
// Hyper-V synthetic MSR addresses (Microsoft TLFS v6.0b)
// ============================================================

#define HV_X64_MSR_GUEST_OS_ID          0x40000000
#define HV_X64_MSR_HYPERCALL            0x40000001
#define HV_X64_MSR_VP_INDEX             0x40000002
#define HV_X64_MSR_RESET                0x40000003
#define HV_X64_MSR_REFERENCE_TSC        0x40000021
#define HV_X64_MSR_SCONTROL             0x40000080
#define HV_X64_MSR_SVERSION             0x40000081
#define HV_X64_MSR_SIEFP                0x40000082
#define HV_X64_MSR_SIMP                 0x40000083
#define HV_X64_MSR_EOM                  0x40000084
#define HV_X64_MSR_SINT0                0x40000090
#define HV_X64_MSR_SINT15               0x4000009F
#define HV_X64_MSR_STIMER0_CONFIG       0x400000B0
#define HV_X64_MSR_STIMER0_COUNT        0x400000B1
#define HV_X64_MSR_STIMER1_CONFIG       0x400000B2
#define HV_X64_MSR_STIMER1_COUNT        0x400000B3
#define HV_X64_MSR_STIMER2_CONFIG       0x400000B4
#define HV_X64_MSR_STIMER2_COUNT        0x400000B5
#define HV_X64_MSR_STIMER3_CONFIG       0x400000B6
#define HV_X64_MSR_STIMER3_COUNT        0x400000B7

// ============================================================
// VMCS field merge rules (spec §3.3)
// ============================================================

typedef enum {
    MERGE_FROM_VMCS12,      // Guest state: copy from L1's VMCS
    MERGE_FROM_L0,          // Host state: always L0's values
    MERGE_UNION_OR,         // Execution controls: OR(L0, L1)
    MERGE_L0_OVERRIDE,      // L0 always wins (EPT pointer, MSR bitmap)
    MERGE_ADDITIVE,         // TSC offset: L0 + L1
    MERGE_MIN_VALUE,        // Preemption timer: min(L0, L1)
    MERGE_COMPUTED,         // Special logic (CR shadows, VPID, MSR bitmap compose)
    MERGE_FIXED,            // Fixed value (VMCS link pointer = 0xFFFFFFFFFFFFFFFF)
} VMCS_MERGE_RULE;

typedef struct _VMCS_MERGE_ENTRY {
    uint32_t        field_encoding;
    VMCS_MERGE_RULE rule;
} VMCS_MERGE_ENTRY;

// ============================================================
// Software VMCS12 — complete copy of L1's logical VMCS
// ============================================================

#define VMCS12_FIELD_MAX 256

typedef struct _VMCS12_FIELD {
    uint32_t encoding;
    uint64_t value;
    uint8_t  written;
    uint8_t  pad[7];
} VMCS12_FIELD;

typedef struct _SOFTWARE_VMCS12 {
    VMCS12_FIELD fields[VMCS12_FIELD_MAX];
    uint32_t     field_count;
    uint32_t     pad;
} SOFTWARE_VMCS12;

// ============================================================
// Shadow EPT context (spec §6.5)
// ============================================================

typedef struct _SHADOW_EPT_CONTEXT {
    uint64_t    pml4_pa;
    EPT_ENTRY*  pml4_va;
    uint64_t    l1_eptp;
    uint32_t    valid;
    uint32_t    page_count;
    volatile uint32_t lock;
    uint32_t    pad;
} SHADOW_EPT_CONTEXT;

// ============================================================
// Hyper-V synthetic state (spec §8)
// ============================================================

typedef struct _HV_SYNTH_STATE {
    uint64_t guest_os_id;
    uint64_t hypercall_msr;
    uint64_t hypercall_page_gpa;
    uint64_t hypercall_page_hpa;
    uint64_t reference_tsc_gpa;
    uint64_t reference_tsc_hpa;
    uint64_t scontrol;
    uint64_t sversion;
    uint64_t siefp;
    uint64_t simp;
    uint64_t sint[16];
    uint64_t stimer_config[4];
    uint64_t stimer_count[4];
} HV_SYNTH_STATE;

// ============================================================
// Per-CPU nested VMX state (spec §4.2)
// ============================================================

typedef struct _NESTED_STATE {
    // Lifecycle flags
    uint8_t     vmx_enabled;
    uint8_t     vmcs_active;
    uint8_t     l2_running;
    uint8_t     pad_flags;

    // VMCS management
    SOFTWARE_VMCS12 vmcs12;
    uint64_t    shadow_vmcs_pa;
    void*       shadow_vmcs_va;
    uint64_t    vmcs01_pa;
    uint64_t    vmcs02_pa;
    void*       vmcs02_va;

    // VMXON tracking
    uint64_t    vmxon_region_gpa;
    uint64_t    current_vmcs_gpa;

    // VPID translation
    uint16_t    l1_vpid;
    uint16_t    l0_vpid_translated;
    uint32_t    pad_vpid;

    // Preemption timer
    uint32_t    l1_preempt_value;
    uint32_t    l0_preempt_value;
    uint64_t    l2_entry_tsc;

    // VMFUNC / EPTP switching
    uint64_t    eptp_list_gpa;
    SHADOW_EPT_CONTEXT shadow_epts[NESTED_MAX_EPTP_LIST];
    uint32_t    active_eptp_index;
    uint32_t    pad_eptp;

    // Shadow EPTP switching list
    uint64_t    shadow_eptp_list_pa;
    void*       shadow_eptp_list_va;

    // Composed MSR bitmap
    uint64_t    composed_msr_bitmap_pa;
    void*       composed_msr_bitmap_va;

    // VMREAD/VMWRITE bitmaps (EFI-allocated, PA for VMCS writes)
    uint64_t    vmread_bitmap_pa;
    void*       vmread_bitmap_va;
    uint64_t    vmwrite_bitmap_pa;
    void*       vmwrite_bitmap_va;

    // Hyper-V synthetic state
    HV_SYNTH_STATE hv;

    // XSAVE areas for L1/L2 context separation (spec §11, review MED-2)
    ARIA_ALIGN(64) uint8_t l1_xsave_area[4096];
    ARIA_ALIGN(64) uint8_t l2_xsave_area[4096];

    // Shadow EPT page pool
    uint8_t*    shadow_ept_pool;
    uint64_t    shadow_ept_pool_pa;
    uint64_t    shadow_ept_pool_offset;
    uint64_t    shadow_ept_pool_size;

    // L0 VMCS field cache (used during VMCS02 build — spec §4.1 step 9)
    uint64_t    l0_field_cache[256];
    uint32_t    l0_field_encodings[256];
    uint32_t    l0_field_count;
} NESTED_STATE;

// ============================================================
// API declarations
// ============================================================

// Lifecycle (nested_vmx.c)
void nested_init(NESTED_STATE* ns);
void nested_teardown(NESTED_STATE* ns);
int  nested_handle_vmxon(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmxoff(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmptrld(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmclear(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmwrite(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmread(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmlaunch(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_vmresume(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_invept(NESTED_STATE* ns, GUEST_REGS* regs);
int  nested_handle_invvpid(NESTED_STATE* ns, GUEST_REGS* regs);

// VM Entry (vm_entry.c)
int  nested_enter_l2(NESTED_STATE* ns);

// Exit dispatch (exit_dispatch.c)
int  nested_classify_exit(NESTED_STATE* ns, GUEST_REGS* regs,
                          uint32_t exit_reason, uint64_t exit_qual);

// Exit reflection (exit_reflect.c)
void nested_reflect_to_l1(NESTED_STATE* ns, GUEST_REGS* regs,
                          uint32_t exit_reason, uint64_t exit_qual);

// Shadow EPT (shadow_ept.c)
int  shadow_ept_init(NESTED_STATE* ns, uint32_t eptp_index);
int  shadow_ept_compose_page(NESTED_STATE* ns, uint32_t eptp_index,
                             uint64_t l2_gpa);
void shadow_ept_invalidate(NESTED_STATE* ns, uint32_t eptp_index);
void shadow_ept_invalidate_all(NESTED_STATE* ns);

// VMFUNC EPTP (eptp_switch.c)
int  eptp_switch_setup(NESTED_STATE* ns);
void eptp_switch_rebuild(NESTED_STATE* ns);

// Hyper-V (hv_interface.c)
int  hv_handle_msr_read(NESTED_STATE* ns, uint32_t msr, uint64_t* value);
int  hv_handle_msr_write(NESTED_STATE* ns, uint32_t msr, uint64_t value);
void hv_cpuid_impersonate(uint32_t leaf, uint32_t subleaf, GUEST_REGS* regs);

// VPID (vpid_mgr.c)
uint16_t vpid_allocate(NESTED_STATE* ns, uint16_t l1_vpid);
void     vpid_free(NESTED_STATE* ns, uint16_t l1_vpid);
uint16_t vpid_translate(NESTED_STATE* ns, uint16_t l1_vpid);

// Interrupt/NMI (interrupt_nested.c)
void nested_handle_external_interrupt(NESTED_STATE* ns, GUEST_REGS* regs, uint32_t vector);
void nested_handle_nmi(NESTED_STATE* ns, GUEST_REGS* regs);
void nested_handle_interrupt_window(NESTED_STATE* ns, GUEST_REGS* regs);

// Preemption timer (preempt_timer.c)
void nested_preempt_setup(NESTED_STATE* ns, uint32_t l0_timer, uint32_t l1_timer);
int  nested_preempt_handle_exit(NESTED_STATE* ns);

// CR access (cr_nested.c)
int  nested_handle_cr_access(NESTED_STATE* ns, GUEST_REGS* regs, uint64_t exit_qual);

// VMCS merge (vmcs_merge.c)
int  vmcs_merge_build_vmcs02(NESTED_STATE* ns);
int  vmcs_merge_validate(NESTED_STATE* ns);
const VMCS_MERGE_ENTRY* vmcs_merge_get_table(uint32_t* out_count);

// VMCS shadow (vmcs_shadow.c)
int  vmcs_shadow_init(NESTED_STATE* ns, uint32_t revision_id);
void vmcs_shadow_setup_bitmaps(NESTED_STATE* ns);
void vmcs_shadow_writeback(NESTED_STATE* ns, uint32_t encoding, uint64_t value);
void vmcs_shadow_sync_to_hardware(NESTED_STATE* ns);
int  vmcs_shadow_alloc_bitmaps(NESTED_STATE* ns);

// VMCS12 software copy (nested_util.c)
void     vmcs12_write(SOFTWARE_VMCS12* v, uint32_t encoding, uint64_t value);
uint64_t vmcs12_read(SOFTWARE_VMCS12* v, uint32_t encoding);
int      vmcs12_has_field(SOFTWARE_VMCS12* v, uint32_t encoding);
void     vmcs12_clear(SOFTWARE_VMCS12* v);
uint64_t vmcs12_read_or_default(SOFTWARE_VMCS12* v, uint32_t encoding, uint64_t def);
int      vmcs12_control_bit_set(SOFTWARE_VMCS12* v, uint32_t control_field, uint32_t bit);
int      vmcs12_validate_controls(SOFTWARE_VMCS12* v);
int      vmcs12_validate_guest_state(SOFTWARE_VMCS12* v);
int      vmcs12_validate_host_state(SOFTWARE_VMCS12* v);
uint32_t vmcs12_field_hash(uint32_t encoding);

// Spinlock helpers (freestanding — no OS dependency)
#ifdef _MSC_VER
#include <intrin.h>
static inline void spinlock_acquire(volatile uint32_t* lock) {
    while (_InterlockedExchange((volatile long*)lock, 1)) {
        while (*lock) { _mm_pause(); }
    }
}
static inline void spinlock_release(volatile uint32_t* lock) {
    _InterlockedExchange((volatile long*)lock, 0);
}
#else
static inline void spinlock_acquire(volatile uint32_t* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) { __builtin_ia32_pause(); }
    }
}
static inline void spinlock_release(volatile uint32_t* lock) {
    __sync_lock_release(lock);
}
#endif
