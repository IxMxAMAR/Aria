// efi/vmx_init.c — VT-x initialization: check, enable, VMCS setup, launch
#include "vmx_init.h"
#include "serial.h"
#include "../vmm/vmm.h"
#include "../vmm/vmcs.h"

extern void __cpuid(int info[4], int leaf);
extern void __cpuidex(int info[4], int leaf, int subleaf);

VMX_STATUS vmx_check_support(void) {
    int info[4];
    __cpuid(info, 1);

    if (!(info[2] & CPUID_VMX_BIT)) {
        serial_print("[!] CPU does not support VMX\n");
        return VMX_NO_SUPPORT;
    }

    uint64_t fc = __readmsr(IA32_FEATURE_CONTROL);
    if ((fc & FEATURE_CONTROL_LOCK) && !(fc & FEATURE_CONTROL_VMXON)) {
        serial_print("[!] VMX locked off in BIOS\n");
        return VMX_LOCKED_OFF;
    }

    if (!(fc & FEATURE_CONTROL_LOCK)) {
        fc |= FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMXON;
        __writemsr(IA32_FEATURE_CONTROL, fc);
        serial_print("[+] Enabled VMX in IA32_FEATURE_CONTROL\n");
    }

    serial_print("[+] VT-x supported and enabled\n");
    return VMX_OK;
}

static void vmx_adjust_control_registers(void) {
    uint64_t cr0 = __readcr0();
    uint64_t cr0_fixed0 = __readmsr(IA32_VMX_CR0_FIXED0);
    uint64_t cr0_fixed1 = __readmsr(IA32_VMX_CR0_FIXED1);
    cr0 |= cr0_fixed0;
    cr0 &= cr0_fixed1;
    __writecr0(cr0);

    uint64_t cr4 = __readcr4();
    uint64_t cr4_fixed0 = __readmsr(IA32_VMX_CR4_FIXED0);
    uint64_t cr4_fixed1 = __readmsr(IA32_VMX_CR4_FIXED1);
    cr4 |= cr4_fixed0;
    cr4 &= cr4_fixed1;
    cr4 |= CR4_VMXE;
    __writecr4(cr4);
}

static uint32_t vmx_adjust_controls(uint32_t desired, uint32_t msr) {
    uint64_t val = __readmsr(msr);
    uint32_t low  = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(val >> 32);
    desired |= low;
    desired &= high;
    return desired;
}

VMX_STATUS vmx_enable(VMM_STATE* state) {
    vmx_adjust_control_registers();

    uint64_t vmx_basic = __readmsr(IA32_VMX_BASIC);
    uint32_t revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFF);

    *(uint32_t*)state->vmxon_region_va = revision_id;

    extern uint8_t __vmx_on(uint64_t* pa);
    uint8_t status = __vmx_on(&state->vmxon_region_pa);
    if (status) {
        serial_print("[!] VMXON failed, status: ");
        serial_print_dec(status);
        serial_print("\n");
        return VMX_VMXON_FAIL;
    }

    serial_print("[+] VMXON succeeded at PA ");
    serial_print_hex(state->vmxon_region_pa);
    serial_print("\n");

    *(uint32_t*)state->vmcs_region_va = revision_id;

    // VMCLEAR must be called before first VMPTRLD — initializes
    // implementation-specific VMCS metadata and sets launch state = "clear".
    // Without this, Alder Lake (and other modern CPUs) reject VMLAUNCH
    // because the internal VMCS format is uninitialized.
    extern uint8_t __vmx_vmclear(uint64_t* pa);
    uint8_t clr_status = __vmx_vmclear(&state->vmcs_region_pa);
    if (clr_status) {
        serial_print("[!] VMCLEAR failed\n");
        return VMX_VMCS_FAIL;
    }
    serial_print("[+] VMCLEAR succeeded\n");

    extern uint8_t __vmx_vmptrld(uint64_t* pa);
    status = __vmx_vmptrld(&state->vmcs_region_pa);
    if (status) {
        serial_print("[!] VMPTRLD failed\n");
        return VMX_VMCS_FAIL;
    }

    serial_print("[+] VMCS loaded at PA ");
    serial_print_hex(state->vmcs_region_pa);
    serial_print("\n");

    return VMX_OK;
}

static void read_segment_descriptor(uint16_t selector, SEGMENT_DESC* desc) {
    #pragma pack(push, 1)
    struct { uint16_t limit; uint64_t base; } gdtr;
    #pragma pack(pop)
    extern void __sgdt(void* gdtr);
    __sgdt(&gdtr);

    desc->selector = selector;
    uint16_t idx = selector & 0xFFF8;

    if (idx == 0) {
        desc->base = 0;
        desc->limit = 0;
        desc->access_rights = 0x10000;
        return;
    }

    uint8_t* gdt = (uint8_t*)gdtr.base;
    uint64_t* entry = (uint64_t*)(gdt + idx);
    uint64_t raw = *entry;

    desc->base  = ((raw >> 16) & 0xFFFFFF) | (((raw >> 56) & 0xFF) << 24);
    desc->limit = (raw & 0xFFFF) | (((raw >> 48) & 0xF) << 16);

    uint32_t ar = (uint32_t)((raw >> 40) & 0xF0FF);
    if (raw & (1ULL << 55)) {
        desc->limit = (desc->limit << 12) | 0xFFF;
    }
    // SDM §26.3.1.2: if segment is not present (P=0), set unusable bit (bit 16).
    // UEFI often has DS/ES/FS/GS with P=0 — without this, VM entry fails Error 33.
    if (!(ar & 0x80)) {
        ar = 0x10000; // unusable — other bits ignored
    }
    desc->access_rights = ar;

    if ((ar & 0x10000) == 0 && (ar & 0x10) == 0 && (ar & 0xF) != 0) {
        // System segment (S=0, not unusable) — 64-bit TSS/LDT: read upper 32 bits of base
        uint64_t high = *(entry + 1);
        desc->base |= (high & 0xFFFFFFFF) << 32;
    }
}

// Ensure TR is loaded — UEFI firmware sometimes leaves TR=0.
// If TR=0, allocate a new GDT with a TSS entry and load it.
static ARIA_ALIGN(16) uint8_t aria_tss[104];   // minimal 64-bit TSS
static ARIA_ALIGN(16) uint8_t aria_gdt[4096];  // our own GDT copy
static uint16_t aria_tr_sel;

static void ensure_tr_loaded(void) {
    extern uint16_t __read_tr(void);
    uint16_t tr = __read_tr();
    if (tr != 0) {
        aria_tr_sel = tr;
        return;
    }

    serial_print("[!] TR=0, building GDT+TSS...\n");

    // Read current GDT
    #pragma pack(push, 1)
    struct { uint16_t limit; uint64_t base; } gdtr;
    #pragma pack(pop)
    extern void __sgdt(void*);
    __sgdt(&gdtr);

    // Copy existing GDT entries into our larger buffer
    uint16_t old_size = gdtr.limit + 1;
    for (uint16_t i = 0; i < old_size && i < sizeof(aria_gdt); i++)
        aria_gdt[i] = ((uint8_t*)gdtr.base)[i];

    // TSS selector = first 16-byte aligned slot after existing entries
    // (TSS descriptor is 16 bytes in 64-bit mode)
    uint16_t tss_off = (old_size + 15) & ~15; // align to 16
    if (tss_off < old_size) tss_off = old_size; // safety
    uint16_t tss_sel = tss_off;

    // Build 64-bit TSS descriptor at tss_off
    uint64_t tss_base = (uint64_t)&aria_tss[0];
    uint32_t tss_limit = sizeof(aria_tss) - 1;

    uint64_t desc_lo = 0;
    desc_lo |= (tss_limit & 0xFFFF);
    desc_lo |= ((tss_base & 0xFFFF) << 16);
    desc_lo |= (((tss_base >> 16) & 0xFF) << 32);
    desc_lo |= (0x89ULL << 40);  // type=9(avail TSS64), P=1, DPL=0
    desc_lo |= (((uint64_t)(tss_limit >> 16) & 0xF) << 48);
    desc_lo |= (((tss_base >> 24) & 0xFF) << 56);
    uint64_t desc_hi = (tss_base >> 32) & 0xFFFFFFFF;

    *(uint64_t*)(aria_gdt + tss_off) = desc_lo;
    *(uint64_t*)(aria_gdt + tss_off + 8) = desc_hi;

    // Load new GDT with LGDT (larger limit includes TSS entry)
    uint16_t new_limit = tss_off + 16 - 1;
    {
        #pragma pack(push, 1)
        struct { uint16_t limit; uint64_t base; } new_gdtr;
        #pragma pack(pop)
        new_gdtr.limit = new_limit;
        new_gdtr.base = (uint64_t)&aria_gdt[0];
        extern void __lgdt(void*);
        __lgdt(&new_gdtr);
    }

    // Load TR
    extern void __load_tr(uint16_t sel);
    __load_tr(tss_sel);

    aria_tr_sel = tss_sel;
    serial_print("[+] TSS at sel=0x");
    serial_print_hex(tss_sel);
    serial_print(" GDT limit=0x");
    serial_print_hex(new_limit);
    serial_print("\n");
}

VMX_STATUS vmx_setup_vmcs(VMM_STATE* state) {
    serial_print("[*] Configuring VMCS...\n");

    // Ensure TR is valid before configuring host state
    ensure_tr_loaded();

    // Pin-based: NO external-interrupt exiting, NO NMI exiting.
    // Guest handles all interrupts via its own IDT.
    // This prevents the infinite VM-exit storm that freezes the system.
    // (Without "ack interrupt on exit" in exit controls, pending interrupts
    //  cause immediate re-exit after VMRESUME → infinite loop)
    uint32_t pinbased = vmx_adjust_controls(
        0, // only CPU must-be-1 bits
        IA32_VMX_TRUE_PINBASED
    );
    vmcs_write32(VMCS_PIN_BASED_EXEC_CTRL, pinbased);

    // Proc-based: MSR bitmap (bit 28) + secondary controls (bit 31)
    uint32_t procbased = vmx_adjust_controls(
        0x10000000 | 0x80000000,
        IA32_VMX_TRUE_PROCBASED
    );
    vmcs_write32(VMCS_PROC_BASED_EXEC_CTRL, procbased);

    // Secondary: EPT (bit 1) + VPID (bit 5)
    uint32_t procbased2 = vmx_adjust_controls(
        0x00000002 | 0x00000020,
        IA32_VMX_PROCBASED_CTLS2
    );
    vmcs_write32(VMCS_PROC_BASED_EXEC_CTRL2, procbased2);

    // ================================================================
    // EXIT CONTROLS
    // Bit 9:  Host address-space size (64-bit host)
    // Bit 18: Save IA32_PAT     | needed for correct host restore
    // Bit 19: Load IA32_PAT     |
    // Bit 20: Save IA32_EFER    |
    // Bit 21: Load IA32_EFER    |
    // Strip: PERF(12), CET(28), PKRS(29)
    // ================================================================
    // Exit: HostAddr(9) + SaveEFER(20) + LoadEFER(21).
    // PAT is deliberately neither saved nor loaded, so it passes through
    // untouched (entry does not load it either — see the entry-controls note).
    uint32_t exit_ctrls = vmx_adjust_controls(
        0x00000200 | 0x00100000 | 0x00200000,
        IA32_VMX_TRUE_EXIT
    );
    vmcs_write32(VMCS_VM_EXIT_CTRLS, exit_ctrls);

    // ================================================================
    // ENTRY CONTROLS
    // Bit 9:  IA-32e mode guest
    // Bit 15: Load IA32_EFER
    // (Bit 14 is Load IA32_PAT — the previous value here — which left
    //  VMCS_GUEST_IA32_EFER inert and reverted every guest WRMSR to EFER.)
    // vmx_adjust_controls() already reconciles the must-be-1/may-be-1 bits
    // from the capability MSR, so no additional masking is needed.
    // ================================================================
    uint32_t entry_ctrls = vmx_adjust_controls(
        0x00000200 | 0x00008000,
        IA32_VMX_TRUE_ENTRY
    );
    vmcs_write32(VMCS_VM_ENTRY_CTRLS, entry_ctrls);

    vmcs_write16(VMCS_VPID, 1);
    vmcs_write64(VMCS_EPT_POINTER, state->eptp.value);
    vmcs_write64(VMCS_MSR_BITMAP, state->msr_bitmap_pa);
    vmcs_write64(VMCS_TSC_OFFSET, 0);

    // Guest segments
    SEGMENT_DESC seg;
    extern uint16_t __read_cs(void);
    extern uint16_t __read_ss(void);
    extern uint16_t __read_ds(void);
    extern uint16_t __read_es(void);
    extern uint16_t __read_fs(void);
    extern uint16_t __read_gs(void);
    extern uint16_t __read_tr(void);
    extern uint16_t __read_ldtr(void);

    uint16_t cs = __read_cs();
    uint16_t ss = __read_ss();
    uint16_t ds = __read_ds();
    uint16_t es = __read_es();
    uint16_t fs = __read_fs();
    uint16_t gs = __read_gs();
    uint16_t tr = aria_tr_sel; // use ensure_tr_loaded() result (handles TR=0 case)
    uint16_t ldtr = __read_ldtr();

    read_segment_descriptor(cs, &seg);
    vmcs_write16(VMCS_GUEST_CS_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_CS_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_CS_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_CS_BASE, seg.base);

    read_segment_descriptor(ss, &seg);
    vmcs_write16(VMCS_GUEST_SS_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_SS_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_SS_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_SS_BASE, seg.base);

    read_segment_descriptor(ds, &seg);
    vmcs_write16(VMCS_GUEST_DS_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_DS_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_DS_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_DS_BASE, seg.base);

    read_segment_descriptor(es, &seg);
    vmcs_write16(VMCS_GUEST_ES_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_ES_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_ES_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_ES_BASE, seg.base);

    read_segment_descriptor(fs, &seg);
    vmcs_write16(VMCS_GUEST_FS_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_FS_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_FS_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_FS_BASE, __readmsr(IA32_FS_BASE));

    read_segment_descriptor(gs, &seg);
    vmcs_write16(VMCS_GUEST_GS_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_GS_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_GS_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_GS_BASE, __readmsr(IA32_GS_BASE));

    read_segment_descriptor(tr, &seg);
    vmcs_write16(VMCS_GUEST_TR_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_TR_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_TR_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_TR_BASE, seg.base);

    read_segment_descriptor(ldtr, &seg);
    vmcs_write16(VMCS_GUEST_LDTR_SEL, seg.selector);
    vmcs_write32(VMCS_GUEST_LDTR_LIMIT, seg.limit);
    vmcs_write32(VMCS_GUEST_LDTR_AR, seg.access_rights);
    vmcs_write64(VMCS_GUEST_LDTR_BASE, seg.base);

    #pragma pack(push, 1)
    struct { uint16_t limit; uint64_t base; } gdtr, idtr;
    #pragma pack(pop)
    extern void __sgdt(void*);
    extern void __sidt(void*);
    __sgdt(&gdtr);
    __sidt(&idtr);

    vmcs_write64(VMCS_GUEST_GDTR_BASE, gdtr.base);
    vmcs_write32(VMCS_GUEST_GDTR_LIMIT, gdtr.limit);
    vmcs_write64(VMCS_GUEST_IDTR_BASE, idtr.base);
    vmcs_write32(VMCS_GUEST_IDTR_LIMIT, idtr.limit);

    vmcs_write64(VMCS_GUEST_CR0, __readcr0());
    vmcs_write64(VMCS_GUEST_CR3, __readcr3());
    vmcs_write64(VMCS_GUEST_CR4, __readcr4());
    vmcs_write64(VMCS_CR0_READ_SHADOW, __readcr0());
    // The guest must not see CR4.VMXE: that bit belongs to the host, and the
    // mask below makes any guest attempt to change it exit.
    vmcs_write64(VMCS_CR4_READ_SHADOW, __readcr4() & ~((uint64_t)CR4_VMXE));
    vmcs_write64(VMCS_CR4_GUEST_HOST_MASK, CR4_VMXE);

    vmcs_write64(VMCS_GUEST_IA32_DEBUGCTL, __readmsr(IA32_DEBUGCTL));
    // PAT is not loaded on entry (entry control bit 14 is clear), so this field
    // is currently inert. Write the live value rather than a hardcoded constant
    // so it cannot silently disagree with the CPU if PAT loading is enabled.
    vmcs_write64(VMCS_GUEST_IA32_PAT, __readmsr(IA32_PAT));
    vmcs_write64(VMCS_GUEST_IA32_EFER, __readmsr(IA32_EFER));
    vmcs_write32(VMCS_GUEST_SYSENTER_CS, (uint32_t)__readmsr(IA32_SYSENTER_CS));
    vmcs_write64(VMCS_GUEST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
    vmcs_write64(VMCS_GUEST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));

    extern uint64_t __read_rflags(void);
    vmcs_write64(VMCS_GUEST_RFLAGS, __read_rflags() & ~(1ULL << 8));
    // Required guest fields — must be initialized or VM entry fails (SDM §26.3.1)
    vmcs_write64(VMCS_GUEST_DR7, 0x400); // default DR7 value
    vmcs_write32(VMCS_GUEST_ACTIVITY, 0); // active
    vmcs_write32(VMCS_GUEST_INTERRUPTIBILITY, 0); // no blocking
    vmcs_write64(VMCS_GUEST_PENDING_DBG, 0); // no pending debug exceptions

    vmcs_write64(VMCS_VMCS_LINK_PTR, 0xFFFFFFFFFFFFFFFFULL);

    // Host state
    vmcs_write16(VMCS_HOST_CS_SEL, cs & 0xFFF8);
    vmcs_write16(VMCS_HOST_SS_SEL, ss & 0xFFF8);
    vmcs_write16(VMCS_HOST_DS_SEL, ds & 0xFFF8);
    vmcs_write16(VMCS_HOST_ES_SEL, es & 0xFFF8);
    vmcs_write16(VMCS_HOST_FS_SEL, fs & 0xFFF8);
    vmcs_write16(VMCS_HOST_GS_SEL, gs & 0xFFF8);
    vmcs_write16(VMCS_HOST_TR_SEL, tr & 0xFFF8);

    vmcs_write64(VMCS_HOST_CR0, __readcr0());
    vmcs_write64(VMCS_HOST_CR3, __readcr3());
    vmcs_write64(VMCS_HOST_CR4, __readcr4());
    vmcs_write64(VMCS_HOST_FS_BASE, __readmsr(IA32_FS_BASE));
    vmcs_write64(VMCS_HOST_GS_BASE, __readmsr(IA32_GS_BASE));

    read_segment_descriptor(tr, &seg);
    vmcs_write64(VMCS_HOST_TR_BASE, seg.base);
    vmcs_write64(VMCS_HOST_GDTR_BASE, gdtr.base);
    vmcs_write64(VMCS_HOST_IDTR_BASE, idtr.base);

    // These host fields are only loaded on VM exit when the matching exit
    // control bit is set. PAT is not loaded in this build (exit control bit 19
    // is clear), so the PAT field below is inert; it holds the live value so it
    // cannot drift if PAT loading is enabled later. EFER *is* loaded (bit 21)
    // and must have LMA=1 to match host-address-space-size=1.
    vmcs_write64(VMCS_HOST_IA32_PAT, __readmsr(IA32_PAT));
    vmcs_write64(VMCS_HOST_IA32_EFER, __readmsr(IA32_EFER));
    vmcs_write32(VMCS_HOST_SYSENTER_CS, (uint32_t)__readmsr(IA32_SYSENTER_CS));
    vmcs_write64(VMCS_HOST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
    vmcs_write64(VMCS_HOST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));

    // ================================================================
    // OPTIONAL FIELDS — only write when corresponding control bits are set.
    // Writing to unsupported VMCS encodings can corrupt VMCS internal state.
    // ================================================================
    #define IA32_PERF_GLOBAL_CTRL       0x38F
    #define CR4_CET_BIT                 (1ULL << 23)

    // PERF_GLOBAL — only if exit/entry controls load it
    if (exit_ctrls & (1u << 12)) {
        int pi[4]; __cpuid(pi, 0x0A);
        uint32_t gpc=(pi[0]>>8)&0xFF, fxc=pi[3]&0x1F;
        uint64_t m=0;
        if (gpc>0) m|=((1ULL<<gpc)-1);
        if (fxc>0) m|=((1ULL<<fxc)-1)<<32;
        vmcs_write64(VMCS_HOST_IA32_PERF_GLOBAL, __readmsr(IA32_PERF_GLOBAL_CTRL)&m);
    }
    if (entry_ctrls & (1u << 13)) {
        int pi[4]; __cpuid(pi, 0x0A);
        uint32_t gpc=(pi[0]>>8)&0xFF, fxc=pi[3]&0x1F;
        uint64_t m=0;
        if (gpc>0) m|=((1ULL<<gpc)-1);
        if (fxc>0) m|=((1ULL<<fxc)-1)<<32;
        vmcs_write64(VMCS_GUEST_IA32_PERF_GLOBAL, __readmsr(IA32_PERF_GLOBAL_CTRL)&m);
    }

    // CET fields — only if exit/entry controls load CET
    if (exit_ctrls & (1u << 28)) {
        vmcs_write64(VMCS_HOST_IA32_S_CET, 0);
        vmcs_write64(VMCS_HOST_SSP, 0);
        vmcs_write64(VMCS_HOST_IA32_INTERRUPT_SSP_TABLE, 0);
    }
    if (entry_ctrls & (1u << 20)) {
        vmcs_write64(VMCS_GUEST_IA32_S_CET, 0);
        vmcs_write64(VMCS_GUEST_SSP, 0);
        vmcs_write64(VMCS_GUEST_IA32_INTERRUPT_SSP_TABLE, 0);
    }

    // Clear CR4.CET from host and guest if set
    {
        uint64_t host_cr4 = vmcs_read64(VMCS_HOST_CR4);
        if (host_cr4 & CR4_CET_BIT) {
            host_cr4 &= ~CR4_CET_BIT;
            vmcs_write64(VMCS_HOST_CR4, host_cr4);
            __writecr4(host_cr4);
        }
        uint64_t guest_cr4 = vmcs_read64(VMCS_GUEST_CR4);
        if (guest_cr4 & CR4_CET_BIT) {
            guest_cr4 &= ~CR4_CET_BIT;
            vmcs_write64(VMCS_GUEST_CR4, guest_cr4);
            vmcs_write64(VMCS_CR4_READ_SHADOW, guest_cr4 & ~((uint64_t)CR4_VMXE));
        }
    }

    extern void vm_exit_stub(void);
    vmcs_write64(VMCS_HOST_RIP, (uint64_t)vm_exit_stub);
    // HOST_RSP must be 16-byte aligned for x64 ABI compliance.
    // exit_stub pushes 128 bytes + 0x20 shadow = 160 bytes before CALL.
    // (HOST_RSP - 160) mod 16 must == 0, so HOST_RSP mod 16 must == 0.
    vmcs_write64(VMCS_HOST_RSP, (uint64_t)state->host_stack + 0x10000);

    // Dump host state for debug
    serial_print("[*] Host state dump:\n");
    serial_print("  CR0="); serial_print_hex(vmcs_read64(VMCS_HOST_CR0)); serial_print("\n");
    serial_print("  CR3="); serial_print_hex(vmcs_read64(VMCS_HOST_CR3)); serial_print("\n");
    serial_print("  CR4="); serial_print_hex(vmcs_read64(VMCS_HOST_CR4)); serial_print("\n");
    serial_print("  CS=");  serial_print_hex(vmcs_read16(VMCS_HOST_CS_SEL));
    serial_print(" SS=");   serial_print_hex(vmcs_read16(VMCS_HOST_SS_SEL));
    serial_print(" TR=");   serial_print_hex(vmcs_read16(VMCS_HOST_TR_SEL)); serial_print("\n");
    serial_print("  RIP="); serial_print_hex(vmcs_read64(VMCS_HOST_RIP)); serial_print("\n");
    serial_print("  RSP="); serial_print_hex(vmcs_read64(VMCS_HOST_RSP)); serial_print("\n");
    serial_print("  EFER="); serial_print_hex(vmcs_read64(VMCS_HOST_IA32_EFER)); serial_print("\n");
    serial_print("  Exit ctrls="); serial_print_hex(vmcs_read32(VMCS_VM_EXIT_CTRLS)); serial_print("\n");
    serial_print("  Entry ctrls="); serial_print_hex(vmcs_read32(VMCS_VM_ENTRY_CTRLS)); serial_print("\n");
    serial_print("  Pin ctrls="); serial_print_hex(vmcs_read32(VMCS_PIN_BASED_EXEC_CTRL)); serial_print("\n");
    serial_print("  Proc ctrls="); serial_print_hex(vmcs_read32(VMCS_PROC_BASED_EXEC_CTRL)); serial_print("\n");
    serial_print("  Proc2 ctrls="); serial_print_hex(vmcs_read32(VMCS_PROC_BASED_EXEC_CTRL2)); serial_print("\n");

    serial_print("[+] VMCS configured\n");
    return VMX_OK;
}

// Pre-launch VMCS validator — checks SDM §26.2 host-state rules
static int vmcs_validate_host_state(void) {
    int errors = 0;
    uint32_t exit_ctrls = vmcs_read32(VMCS_VM_EXIT_CTRLS);
    uint32_t entry_ctrls = vmcs_read32(VMCS_VM_ENTRY_CTRLS);

    // §26.2.1: Host CR0 fixed bits
    uint64_t cr0 = vmcs_read64(VMCS_HOST_CR0);
    uint64_t cr0_f0 = __readmsr(IA32_VMX_CR0_FIXED0);
    uint64_t cr0_f1 = __readmsr(IA32_VMX_CR0_FIXED1);
    if ((cr0 & cr0_f0) != cr0_f0) { serial_print("[VALIDATE] HOST_CR0 missing fixed0 bits\n"); errors++; }
    if ((cr0 & ~cr0_f1) != 0)     { serial_print("[VALIDATE] HOST_CR0 has disallowed bits\n"); errors++; }

    // Host CR4 fixed bits
    uint64_t cr4 = vmcs_read64(VMCS_HOST_CR4);
    uint64_t cr4_f0 = __readmsr(IA32_VMX_CR4_FIXED0);
    uint64_t cr4_f1 = __readmsr(IA32_VMX_CR4_FIXED1);
    if ((cr4 & cr4_f0) != cr4_f0) { serial_print("[VALIDATE] HOST_CR4 missing fixed0 bits\n"); errors++; }
    if ((cr4 & ~cr4_f1) != 0)     { serial_print("[VALIDATE] HOST_CR4 has disallowed bits\n"); errors++; }

    // Host segment selectors: CS, SS, TR must not be 0; all must have RPL=0, TI=0
    uint16_t cs_sel = vmcs_read16(VMCS_HOST_CS_SEL);
    uint16_t ss_sel = vmcs_read16(VMCS_HOST_SS_SEL);
    uint16_t tr_sel = vmcs_read16(VMCS_HOST_TR_SEL);
    if (cs_sel == 0) { serial_print("[VALIDATE] HOST_CS_SEL is 0!\n"); errors++; }
    if (ss_sel == 0) { serial_print("[VALIDATE] HOST_SS_SEL is 0!\n"); errors++; }
    if (tr_sel == 0) { serial_print("[VALIDATE] HOST_TR_SEL is 0!\n"); errors++; }
    if (cs_sel & 7) { serial_print("[VALIDATE] HOST_CS RPL/TI not 0\n"); errors++; }
    if (ss_sel & 7) { serial_print("[VALIDATE] HOST_SS RPL/TI not 0\n"); errors++; }
    if (tr_sel & 7) { serial_print("[VALIDATE] HOST_TR RPL/TI not 0\n"); errors++; }

    // Host RIP must be canonical (bits 63:48 must be sign-extension of bit 47)
    uint64_t rip = vmcs_read64(VMCS_HOST_RIP);
    uint64_t rip_top = rip >> 47;
    if (rip_top != 0 && rip_top != 0x1FFFF) {
        serial_print("[VALIDATE] HOST_RIP non-canonical: ");
        serial_print_hex(rip); serial_print("\n");
        errors++;
    }

    // If "host address-space size" (bit 9 of exit controls) is 1 (64-bit host):
    if (exit_ctrls & 0x200) {
        // Host CR4.PAE must be 1
        if (!(cr4 & (1ULL << 5))) { serial_print("[VALIDATE] 64-bit host but CR4.PAE=0\n"); errors++; }
        // Host RIP must be canonical (checked above)
        // Host EFER.LME and EFER.LMA must be 1
        uint64_t efer = vmcs_read64(VMCS_HOST_IA32_EFER);
        if (!(efer & (1ULL << 8)))  { serial_print("[VALIDATE] HOST_EFER.LME=0 in 64-bit mode\n"); errors++; }
        if (!(efer & (1ULL << 10))) { serial_print("[VALIDATE] HOST_EFER.LMA=0 in 64-bit mode\n"); errors++; }
    }

    // If exit controls bit 12 (Load IA32_PERF_GLOBAL_CTRL) is set, validate reserved bits
    if (exit_ctrls & (1u << 12)) {
        int perf_info[4];
        __cpuid(perf_info, 0x0A);
        uint32_t gp_count    = (perf_info[0] >> 8) & 0xFF;
        uint32_t fixed_count = perf_info[3] & 0x1F;
        uint64_t mask = 0;
        if (fixed_count > 0) mask |= ((1ULL << fixed_count) - 1);
        if (gp_count > 0)    mask |= ((1ULL << gp_count) - 1) << 32;

        uint64_t perf_val = vmcs_read64(VMCS_HOST_IA32_PERF_GLOBAL);
        if (perf_val & ~mask) {
            serial_print("[VALIDATE] HOST_PERF_GLOBAL has reserved bits! val=");
            serial_print_hex(perf_val);
            serial_print(" mask=");
            serial_print_hex(mask);
            serial_print("\n");
            errors++;
        }
    }

    // If exit controls bit 20 (Save IA32_EFER), host EFER must be consistent
    // If exit controls bit 21 (Load IA32_EFER), checked above

    // CET checks — after our fix, if bits survived stripping, fields are written.
    // Only error if the fields are obviously wrong.
    if (exit_ctrls & (1u << 28)) {
        serial_print("[VALIDATE] Exit ctrl bit28 (Load CET) ON — HOST CET fields should be written\n");
    }
    if (entry_ctrls & (1u << 20)) {
        serial_print("[VALIDATE] Entry ctrl bit20 (Load CET) ON — GUEST CET fields should be written\n");
    }

    // HOST_CR4.CET must be 0 if we didn't write CET fields
    uint64_t cr4_cet = vmcs_read64(VMCS_HOST_CR4);
    if ((cr4_cet & (1ULL << 23)) && !(exit_ctrls & (1u << 28))) {
        serial_print("[VALIDATE] HOST_CR4.CET=1 but no CET load in exit controls — will fail!\n");
        errors++;
    }

    serial_print("[VALIDATE] Host state check: ");
    serial_print_dec(errors);
    serial_print(" errors\n");
    return errors;
}

// Progress marker — written by vmx_launch, read by main after return
volatile uint32_t vmx_launch_progress = 0;

VMX_STATUS vmx_launch(VMM_STATE* state) {
    extern void log_write(const char*);
    log_write("[L] vmx_launch entered\n");

    // Cross-check the host state against the SDM 26.2 rules now, so a violation
    // shows up as a specific message instead of an opaque VM-entry failure.
    // Report-only: this build has not been run on hardware yet, and a validator
    // false positive must not be able to block a launch that would otherwise
    // work. Read the error count together with the VM-instruction error below.
    vmcs_validate_host_state();

    extern int vmx_do_launch(VMM_STATE* state);
    log_write("[L] calling vmx_do_launch\n");
    int result = vmx_do_launch(state);
    // DON'T call log_write here — UEFI USB driver hangs under UC EPT.
    // Just return the result. Main will handle it.

    if (result != 0) {
        uint32_t err = vmcs_read32(VMCS_VM_INSTRUCTION_ERROR);
        serial_print("[!] VMLAUNCH failed, error code: ");
        serial_print_dec(err);
        serial_print("\n");
        state->target_pid = err;
        return VMX_LAUNCH_FAIL;
    }

    serial_print("[+] VM running — guest mode active\n");
    state->launched = 1;
    return VMX_OK;
}
