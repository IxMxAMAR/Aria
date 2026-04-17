// vmm/nested/vmcs_shadow.c — Shadow VMCS page management (spec §3.1)
// Handles: shadow page init (bit 31!), VMREAD/VMWRITE bitmap config,
// and writeback to shadow page for L1's VMREAD visibility.
#include "vmcs_shadow.h"
#include "../vmcs.h"
#include "../../efi/serial.h"

int vmcs_shadow_init(NESTED_STATE* ns, uint32_t revision_id) {
    if (!ns->shadow_vmcs_va) return -1;

    // CRITICAL: bit 31 = shadow-VMCS indicator (Intel SDM §24.10)
    // Without this bit, hardware treats the page as a regular VMCS.
    // bits 30:0 = VMCS revision ID from IA32_VMX_BASIC.
    *(uint32_t*)ns->shadow_vmcs_va = revision_id | (1u << 31);

    // Zero the rest of the shadow page
    vmm_memset((uint8_t*)ns->shadow_vmcs_va + 4, 0, PAGE_SIZE - 4);

    DBG_PRINT("[nested] Shadow VMCS init: PA=");
    DBG_HEX(ns->shadow_vmcs_pa);
    DBG_PRINT(" rev=");
    DBG_HEX(revision_id | (1u << 31));
    DBG_PRINT("\n");

    return 0;
}

int vmcs_shadow_alloc_bitmaps(NESTED_STATE* ns) {
    if (!ns->vmwrite_bitmap_va || !ns->vmread_bitmap_va) return -1;

    // VMWRITE bitmap: ALL bits = 1
    // Every L1 VMWRITE causes a VM exit -> L0 updates software VMCS12.
    vmm_memset(ns->vmwrite_bitmap_va, 0xFF, PAGE_SIZE);

    // VMREAD bitmap: ALL bits = 0 (hardware handles) EXCEPT read-only fields
    vmm_memset(ns->vmread_bitmap_va, 0x00, PAGE_SIZE);

    // Intercept VMREADs of read-only fields -- L0 serves from software VMCS12.
    // These fields can't be VMWRITTEn to the shadow page (SDM: VMWRITE error 13)
    // so we must intercept and return values from our software copy.
    static const uint32_t ro_fields[] = {
        0x4400, // VM_INSTRUCTION_ERROR
        0x4402, // EXIT_REASON
        0x4404, // VM_EXIT_INTR_INFO
        0x4406, // VM_EXIT_INTR_EC
        0x4408, // IDT_VECTORING_INFO
        0x440A, // IDT_VECTORING_EC
        0x440C, // VM_EXIT_INSN_LEN
        0x440E, // VM_EXIT_INSN_INFO
        0x2400, // GUEST_PHYS_ADDR
        0x6400, // EXIT_QUALIFICATION
        0x6402, // IO_RCX
        0x6404, // IO_RSI
        0x6406, // IO_RDI
        0x6408, // IO_RIP
        0x640A, // GUEST_LINEAR_ADDR
    };
    uint8_t* rb = (uint8_t*)ns->vmread_bitmap_va;
    for (uint32_t i = 0; i < sizeof(ro_fields)/sizeof(ro_fields[0]); i++) {
        uint32_t enc = ro_fields[i];
        rb[enc / 8] |= (1u << (enc % 8));
    }

    return 0;
}

void vmcs_shadow_setup_bitmaps(NESTED_STATE* ns) {
    // Configure VMCS01 for VMCS shadowing.
    // VMCS01 must be the current VMCS when this is called.

    // 1. Link pointer -> shadow VMCS physical address
    vmcs_write64(VMCS_VMCS_LINK_PTR, ns->shadow_vmcs_pa);

    // 2. Enable VMCS shadowing (secondary proc-based bit 14)
    uint32_t procbased2 = vmcs_read32(VMCS_PROC_BASED_EXEC_CTRL2);
    procbased2 |= (1u << 14);
    vmcs_write32(VMCS_PROC_BASED_EXEC_CTRL2, procbased2);

    // 3. Set bitmap addresses (EFI-allocated physical addresses)
    vmcs_write64(VMCS_VMREAD_BITMAP, ns->vmread_bitmap_pa);
    vmcs_write64(VMCS_VMWRITE_BITMAP, ns->vmwrite_bitmap_pa);

    DBG_PRINT("[nested] VMCS shadowing enabled in VMCS01\n");
}

void vmcs_shadow_writeback(NESTED_STATE* ns, uint32_t encoding, uint64_t value) {
    // Write to software VMCS12 only — shadow page synced in bulk later
    vmcs12_write(&ns->vmcs12, encoding, value);
}

void vmcs_shadow_sync_to_hardware(NESTED_STATE* ns) {
    // After VMCS01 is restored as current, sync dirty VMCS12 fields
    // to the shadow VMCS page so L1's VMREADs see updated values.
    // We temporarily load the shadow page as a regular VMCS.

    if (!ns->shadow_vmcs_va) return;

    // 1. Save shadow bit and clear it
    uint32_t* header = (uint32_t*)ns->shadow_vmcs_va;
    uint32_t saved_rev = *header;
    *header = saved_rev & ~(1u << 31);  // clear shadow indicator

    // NOTE: VMCS01 is briefly not current during shadow sync.
    // NMIs in this window would see wrong VMCS. Window is ~30 insns.
    // Production fix: use NMI-window exiting to defer NMIs.

    // 2. VMCLEAR the shadow page (required before VMPTRLD as regular)
    extern uint8_t __vmx_vmclear(uint64_t* pa);
    __vmx_vmclear(&ns->shadow_vmcs_pa);

    // 3. Save current VMCS (VMCS01) and load shadow as regular
    extern uint8_t __vmx_vmptrld(uint64_t* pa);
    uint64_t vmcs01_pa = ns->vmcs01_pa;
    __vmx_vmptrld(&ns->shadow_vmcs_pa);

    // 4. Write all dirty fields from software copy
    for (uint32_t i = 0; i < ns->vmcs12.field_count; i++) {
        if (ns->vmcs12.fields[i].written) {
            uint32_t enc = ns->vmcs12.fields[i].encoding;
            // Skip read-only fields -- VMWRITE would fail (error 13)
            // These are served via VMREAD interception from software copy
            uint32_t access_type = enc & 1;
            uint32_t field_type = (enc >> 10) & 3;
            if (field_type == 1 && access_type == 0) continue; // read-only data field
            // More precise: 32-bit RO (0x44xx), 64-bit RO (0x24xx), natural RO (0x64xx)
            if ((enc >= 0x4400 && enc <= 0x440E) ||
                (enc >= 0x2400 && enc <= 0x2402) ||
                (enc >= 0x6400 && enc <= 0x640A))
                continue;
            vmcs_write64(enc, ns->vmcs12.fields[i].value);
        }
    }

    // 5. Restore shadow indicator and reload VMCS01
    // Do NOT VMCLEAR here — it would wipe all fields just written above.
    // The shadow page retains its data in memory; hardware VMCS shadowing
    // reads from the physical page content, not from "current" status.
    *header = saved_rev;  // restore shadow indicator (bit 31)
    __vmx_vmptrld(&vmcs01_pa);  // VMCS01 current again; shadow page stays intact
}
