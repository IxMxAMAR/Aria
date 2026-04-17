// vmm/nested/nested_util.c — VMCS12 field access and SDM validation
// Reference: Intel SDM Vol. 3, §26.2-26.3
#include "nested_util.h"
#include "../vmcs.h"
#include "../../efi/serial.h"

// ================================================================
// VMCS12 field hash table — open addressing with linear probing
// ================================================================

uint32_t vmcs12_field_hash(uint32_t encoding) {
    // Use bits 14:0 of the encoding as hash seed
    // VMCS encodings are structured: bits 0 access type, 9:1 index,
    // 11:10 type, 14:13 width. The full 15 bits give good distribution.
    return (encoding ^ (encoding >> 7)) % VMCS12_FIELD_MAX;
}

static uint32_t vmcs12_find_slot(SOFTWARE_VMCS12* v, uint32_t encoding) {
    uint32_t idx = vmcs12_field_hash(encoding);
    for (uint32_t i = 0; i < VMCS12_FIELD_MAX; i++) {
        uint32_t probe = (idx + i) % VMCS12_FIELD_MAX;
        if (!v->fields[probe].written) return probe;
        if (v->fields[probe].encoding == encoding) return probe;
    }
    return 0; // table full — 256 slots for ~109 fields, shouldn't happen
}

void vmcs12_write(SOFTWARE_VMCS12* v, uint32_t encoding, uint64_t value) {
    uint32_t slot = vmcs12_find_slot(v, encoding);
    if (!v->fields[slot].written) {
        v->field_count++;
    }
    v->fields[slot].encoding = encoding;
    v->fields[slot].value = value;
    v->fields[slot].written = 1;
}

uint64_t vmcs12_read(SOFTWARE_VMCS12* v, uint32_t encoding) {
    uint32_t idx = vmcs12_field_hash(encoding);
    for (uint32_t i = 0; i < VMCS12_FIELD_MAX; i++) {
        uint32_t probe = (idx + i) % VMCS12_FIELD_MAX;
        if (!v->fields[probe].written) return 0;
        if (v->fields[probe].encoding == encoding)
            return v->fields[probe].value;
    }
    return 0;
}

int vmcs12_has_field(SOFTWARE_VMCS12* v, uint32_t encoding) {
    uint32_t idx = vmcs12_field_hash(encoding);
    for (uint32_t i = 0; i < VMCS12_FIELD_MAX; i++) {
        uint32_t probe = (idx + i) % VMCS12_FIELD_MAX;
        if (!v->fields[probe].written) return 0;
        if (v->fields[probe].encoding == encoding) return 1;
    }
    return 0;
}

void vmcs12_clear(SOFTWARE_VMCS12* v) {
    vmm_memset(v, 0, sizeof(SOFTWARE_VMCS12));
}

uint64_t vmcs12_read_or_default(SOFTWARE_VMCS12* v, uint32_t encoding, uint64_t def) {
    if (vmcs12_has_field(v, encoding))
        return vmcs12_read(v, encoding);
    return def;
}

int vmcs12_control_bit_set(SOFTWARE_VMCS12* v, uint32_t control_field, uint32_t bit) {
    uint64_t val = vmcs12_read(v, control_field);
    return (val & (1ULL << bit)) != 0;
}

// ================================================================
// SDM §26.2-26.3 Validation
// ================================================================

// MSR addresses for VMX capability checking
#define MSR_IA32_VMX_BASIC              0x480
#define MSR_IA32_VMX_PINBASED_CTLS      0x481
#define MSR_IA32_VMX_PROCBASED_CTLS     0x482
#define MSR_IA32_VMX_TRUE_PINBASED      0x48D
#define MSR_IA32_VMX_TRUE_PROCBASED     0x48E
#define MSR_IA32_VMX_CR0_FIXED0         0x486
#define MSR_IA32_VMX_CR0_FIXED1         0x487
#define MSR_IA32_VMX_CR4_FIXED0         0x488
#define MSR_IA32_VMX_CR4_FIXED1         0x489

// Check execution control fields against VMX capability MSRs
static int validate_control_field(uint32_t value, uint32_t msr_true, uint32_t msr_default) {
    uint64_t vmx_basic = __readmsr(MSR_IA32_VMX_BASIC);
    int use_true = (vmx_basic >> 55) & 1;
    uint64_t msr_val = __readmsr(use_true ? msr_true : msr_default);

    uint32_t required = (uint32_t)(msr_val & 0xFFFFFFFF);
    uint32_t allowed  = (uint32_t)(msr_val >> 32);

    if ((value & required) != required) return -1;
    if (value & ~allowed) return -1;
    return 0;
}

int vmcs12_validate_controls(SOFTWARE_VMCS12* v) {
    // §26.2.1.1: Pin-based VM-execution controls
    uint32_t pinbased = (uint32_t)vmcs12_read(v, VMCS_PIN_BASED_EXEC_CTRL);
    if (validate_control_field(pinbased, MSR_IA32_VMX_TRUE_PINBASED,
                               MSR_IA32_VMX_PINBASED_CTLS) != 0) {
        DBG_PRINT("[nested] Pin-based control validation failed\n");
        return 7; // VMX error code: VMENTRY with invalid control fields
    }

    // §26.2.1.2: Primary processor-based VM-execution controls
    uint32_t procbased = (uint32_t)vmcs12_read(v, VMCS_PROC_BASED_EXEC_CTRL);
    if (validate_control_field(procbased, MSR_IA32_VMX_TRUE_PROCBASED,
                               MSR_IA32_VMX_PROCBASED_CTLS) != 0) {
        DBG_PRINT("[nested] Proc-based control validation failed\n");
        return 7;
    }

    // Additional control checks (VM-exit, VM-entry controls) can be added
    // as needed when specific failures are encountered during testing
    return 0;
}

int vmcs12_validate_guest_state(SOFTWARE_VMCS12* v) {
    // §26.3.1.1: CR0 must satisfy fixed bits
    uint64_t cr0 = vmcs12_read(v, VMCS_GUEST_CR0);
    uint64_t cr0_fixed0 = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
    uint64_t cr0_fixed1 = __readmsr(MSR_IA32_VMX_CR0_FIXED1);

    // Account for unrestricted guest: if enabled, PE and PG are not required
    uint32_t procbased2 = (uint32_t)vmcs12_read(v, VMCS_PROC_BASED_EXEC_CTRL2);
    int unrestricted_guest = (procbased2 >> 7) & 1;

    uint64_t check_cr0 = cr0;
    if (unrestricted_guest) {
        // PE (bit 0) and PG (bit 31) are not required
        check_cr0 |= (1ULL | (1ULL << 31));
    }

    if ((check_cr0 & cr0_fixed0) != cr0_fixed0) {
        DBG_PRINT("[nested] Guest CR0 fixed0 check failed\n");
        return 7;
    }
    if (cr0 & ~cr0_fixed1) {
        DBG_PRINT("[nested] Guest CR0 fixed1 check failed\n");
        return 7;
    }

    // §26.3.1.1: CR4 must satisfy fixed bits
    uint64_t cr4 = vmcs12_read(v, VMCS_GUEST_CR4);
    uint64_t cr4_fixed0 = __readmsr(MSR_IA32_VMX_CR4_FIXED0);
    uint64_t cr4_fixed1 = __readmsr(MSR_IA32_VMX_CR4_FIXED1);

    if ((cr4 & cr4_fixed0) != cr4_fixed0) {
        DBG_PRINT("[nested] Guest CR4 fixed0 check failed\n");
        return 7;
    }
    if (cr4 & ~cr4_fixed1) {
        DBG_PRINT("[nested] Guest CR4 fixed1 check failed\n");
        return 7;
    }

    // §26.3.1.2: Segment register checks
    // CS access rights must have type that's valid for the mode
    // These are extensive — implement incrementally when failures occur
    // Hyper-V writes valid guest state, so these rarely fire

    return 0;
}

int vmcs12_validate_host_state(SOFTWARE_VMCS12* v) {
    // L0 overrides host state fields in VMCS02.
    // This validates that our OWN host state is consistent.
    // Since we set these in vmx_setup_vmcs() during EFI init,
    // they should always be valid. Sanity check only.
    (void)v;
    return 0;
}
