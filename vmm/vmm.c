// vmm/vmm.c — VM exit dispatcher
#include "vmm.h"
#include "vmcs.h"
#include "../efi/serial.h"
#include "../shared/protocol.h"

VMM_STATE g_vmm = {0};

// ---------------------------------------------------------------------------
// Build-time behaviour switches
// ---------------------------------------------------------------------------

// Announce Aria to the guest via the hypervisor CPUID leaf (0x40000000). This is
// what a real hypervisor does, and it makes the hypervisor observable from inside
// the guest: `cpuid` in the guest shows the "AriaAriaAria" vendor string.
// Set to 0 for a fully transparent passthrough (guest cannot tell it is virtualised).
#define ARIA_ANNOUNCE_VENDOR 1

// On an unhandled VM exit: 1 = report and stop (loud, diagnosable).
// 0 = log and skip the instruction (silent guest-state corruption; debug only).
// Skipping is only ever valid for instruction-caused exits, because
// VM_EXIT_INSN_LEN is undefined for event-caused exits (INIT/SIPI/triple fault).
#define ARIA_STRICT_EXITS 1

// MSVC floating-point support marker — referenced by any code using float/double.
// In freestanding builds without CRT, we provide the symbol ourselves.
int _fltused = 0;

// MSVC generates implicit memset/memcpy calls for large zero-initializations.
// Provide real symbols since we're freestanding (no CRT).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4164)
#pragma function(memset)
#pragma function(memcpy)
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
int  handle_msr_write(GUEST_REGS* regs);
void handle_cr_access(GUEST_REGS* regs);

static void advance_guest_rip(void) {
    uint64_t rip = vmcs_read64(VMCS_GUEST_RIP);
    uint32_t len = vmcs_read32(VMCS_VM_EXIT_INSN_LEN);
    vmcs_write64(VMCS_GUEST_RIP, rip + len);
}

// Inject a hardware exception into the guest (type 3, deliver-error-code clear).
static void inject_exception(uint32_t vector, uint32_t error_code) {
    vmcs_write32(VMCS_VM_ENTRY_INTR_INFO, 0x80000000u | (3u << 8) | (vector & 0xFF));
    vmcs_write32(VMCS_VM_ENTRY_EXCEPTION_EC, error_code);
}

// ---------------------------------------------------------------------------
// Diagnostics. Serial may not exist and the UEFI console may be unusable once
// the guest owns the machine, so failures also go to the legacy VGA text buffer
// as a last resort.
// ---------------------------------------------------------------------------
static void vga_puts(const char* s) {
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    static uint32_t row = 0, col = 0;
    for (; *s; s++) {
        if (*s == '\n') { col = 0; if (++row > 24) row = 0; continue; }
        vga[row * 80 + col] = (uint16_t)(0x0C00 | (uint8_t)*s);
        if (++col >= 80) { col = 0; if (++row > 24) row = 0; }
    }
}

static void halt_forever(void) {
    for (;;) { extern void _mm_pause(void); _mm_pause(); }
}

// Global exit counter — readable from EFI main after VMLAUNCH
volatile uint32_t aria_exit_count = 0;
// Track first 16 exit reasons for debugging
volatile uint32_t aria_exit_log[16] = {0};
// Per-reason exit counters, indexed by VM-exit reason 0-64. The log above shows
// ordering; these show frequency, and the guest reads them via hypercall 2.
volatile uint64_t aria_exit_reason_counts[65] = {0};

// ---------------------------------------------------------------------------
// Hypercall ABI (guest -> hypervisor)
//
// The guest issues VMCALL with the call number in RAX. Results come back in
// registers only — translating a guest virtual address to a host pointer would
// mean walking the guest's page tables, and no call here needs that.
//
//   0  VERSION      -> RAX=0, RBX=ABI version, RCX=0x61697241 ("Aria")
//   1  EXIT_COUNT   -> RAX=0, RBX=total VM exits since launch
//   2  EXIT_REASON  -> in RCX=reason, RAX=0, RBX=exit count for that reason
//   3  EPT_TYPING   -> RAX=0, RBX=WB pages, RCX=UC pages
//
// An unknown call returns RAX=1.
// ---------------------------------------------------------------------------
#define ARIA_HC_VERSION      0
#define ARIA_HC_EXIT_COUNT   1
#define ARIA_HC_EXIT_REASON  2
#define ARIA_HC_EPT_TYPING   3
#define ARIA_ABI_VERSION     0x00010000ULL

static void handle_hypercall(GUEST_REGS* regs) {
    uint64_t nr = regs->rax;
    regs->rax = 0;
    switch (nr) {
    case ARIA_HC_VERSION:
        regs->rbx = ARIA_ABI_VERSION;
        regs->rcx = 0x61697241ULL; // "Aria"
        break;
    case ARIA_HC_EXIT_COUNT:
        regs->rbx = (uint64_t)aria_exit_count;
        break;
    case ARIA_HC_EXIT_REASON: {
        uint32_t reason = (uint32_t)regs->rcx;
        regs->rbx = (reason < 65) ? aria_exit_reason_counts[reason] : 0;
        break;
    }
    case ARIA_HC_EPT_TYPING:
        regs->rbx = g_vmm.ept_wb_pages;
        regs->rcx = g_vmm.ept_uc_pages;
        break;
    default:
        regs->rax = 1;
        break;
    }
}

void vmm_handle_exit(GUEST_REGS* regs) {
    uint32_t exit_reason = vmcs_read32(VMCS_EXIT_REASON) & 0xFFFF;

    // Log exit reason
    uint32_t idx = aria_exit_count;
    if (idx < 16) aria_exit_log[idx] = exit_reason;
    aria_exit_count++;
    if (exit_reason < 65) aria_exit_reason_counts[exit_reason]++;

    switch (exit_reason) {
    case EXIT_REASON_EXCEPTION_NMI: // 0
        // Re-inject the exception/NMI into the guest.
        {
            uint32_t intr_info = vmcs_read32(VMCS_VM_EXIT_INTR_INFO);
            vmcs_write32(VMCS_VM_ENTRY_INTR_INFO, intr_info);
            if (intr_info & (1u << 11))
                vmcs_write32(VMCS_VM_ENTRY_EXCEPTION_EC, vmcs_read32(VMCS_VM_EXIT_INTR_EC));
            vmcs_write32(VMCS_VM_ENTRY_INSN_LEN, vmcs_read32(VMCS_VM_EXIT_INSN_LEN));
        }
        break;

    case EXIT_REASON_EXTERNAL_INT: // 1
        // External-interrupt exiting is disabled in the pin-based controls, so
        // this should not occur. If it does, just resume; the guest IDT handles it.
        break;

    case EXIT_REASON_CPUID: { // 10
        uint32_t leaf    = (uint32_t)regs->rax;
        uint32_t subleaf = (uint32_t)regs->rcx;

#if ARIA_ANNOUNCE_VENDOR
        // Hypervisor vendor leaf. The 12-byte string is returned in EBX:ECX:EDX
        // in little-endian byte order ("AriaAriaAria").
        if (leaf == 0x40000000) {
            regs->rax = 0x40000002;         // highest supported hypervisor leaf
            regs->rbx = 0x61697241;         // "Aria"
            regs->rcx = 0x61697241;         // "Aria"
            regs->rdx = 0x61697241;         // "Aria"
            advance_guest_rip();
            break;
        }
        if (leaf == 0x40000001) {
            // Interface description: ABI version in EAX, feature flags in EBX,
            // highest hypercall number in ECX. A guest discovers the interface
            // rather than hardcoding assumptions about what Aria supports.
            regs->rax = ARIA_ABI_VERSION;
            regs->rbx = (1u << 0)             // VMCALL hypercalls
                      | (1u << 1)             // exit statistics
                      | (1u << 2)             // EPT memory-typing report
                      | (1u << 3);            // MSR write interception
            regs->rcx = ARIA_HC_EPT_TYPING;   // highest hypercall implemented
            regs->rdx = 0;
            advance_guest_rip();
            break;
        }
        if (leaf == 0x40000002) {
            // Live exit counter: the guest can observe that the hypervisor is
            // actually running underneath it instead of taking that on faith.
            uint64_t n = (uint64_t)aria_exit_count;
            regs->rax = n & 0xFFFFFFFF;
            regs->rbx = n >> 32;
            regs->rcx = 0; regs->rdx = 0;
            advance_guest_rip();
            break;
        }
        if (leaf >= 0x40000003 && leaf <= 0x4000000F) {
            regs->rax = 0; regs->rbx = 0; regs->rcx = 0; regs->rdx = 0;
            advance_guest_rip();
            break;
        }
#endif

        int info[4];
        extern void __cpuidex(int info[4], int leaf, int sub);
        __cpuidex(info, leaf, (int)subleaf);

        if (leaf == 1) {
#if ARIA_ANNOUNCE_VENDOR
            info[2] = (int)((uint32_t)info[2] | (1u << 31)); // hypervisor-present
#else
            info[2] = (int)((uint32_t)info[2] & ~(1u << 31));
#endif
            // VMX is reported as absent: nested VMX is not implemented yet, so a
            // guest that tried VMXON would fail anyway. Re-enable in Phase 2.
            info[2] = (int)((uint32_t)info[2] & ~(1u << 5));
        }

        regs->rax = (uint64_t)(uint32_t)info[0];
        regs->rbx = (uint64_t)(uint32_t)info[1];
        regs->rcx = (uint64_t)(uint32_t)info[2];
        regs->rdx = (uint64_t)(uint32_t)info[3];
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_MSR_READ: { // 31
        uint32_t msr = (uint32_t)regs->rcx;
        extern uint64_t __readmsr(uint32_t);
        uint64_t val = __readmsr(msr);
        regs->rax = val & 0xFFFFFFFF;
        regs->rdx = val >> 32;
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_MSR_WRITE: { // 32
        // Only the MSRs listed in vmm/msr_bitmap.c reach here. The handler
        // mirrors the new value into the VMCS so the next VM entry does not
        // revert it, and returns 0 when it injected an exception instead.
        if (handle_msr_write(regs)) advance_guest_rip();
        break;
    }

    case EXIT_REASON_CR_ACCESS: // 28
        handle_cr_access(regs);
        advance_guest_rip();
        break;

    case EXIT_REASON_XSETBV: { // 55
        extern void __xsetbv(uint32_t idx, uint64_t val);
        uint32_t xcr = (uint32_t)regs->rcx;
        // XCR0 selects the XSAVE state components. A guest request for a
        // component the host does not support raises #GP in *host* context, so
        // validate before forwarding.
        if (xcr != 0) {
            inject_exception(13 /* #GP */, 0);
            break;
        }
        __xsetbv(xcr, ((uint64_t)regs->rdx << 32) | (regs->rax & 0xFFFFFFFF));
        advance_guest_rip();
        break;
    }

    case EXIT_REASON_INVD: // 13
        // INVD invalidates caches without writing back. WBINVD semantics are not
        // reproduced; advance RIP.
        advance_guest_rip();
        break;

    case EXIT_REASON_VMCALL: { // 18
        handle_hypercall(regs);
        advance_guest_rip();
        break;
    }

    // --- VMX instruction exits -------------------------------------------------
    // VMX is reported as absent in CPUID, so these are only reachable if the
    // guest ignores that. A real CPU without VMX raises #UD; match that.
    case EXIT_REASON_VMXON:      // 27
    case EXIT_REASON_VMXOFF:     // 26
    case EXIT_REASON_VMCLEAR:    // 19
    case EXIT_REASON_VMPTRLD:    // 21
    case EXIT_REASON_VMPTRST:    // 22
    case EXIT_REASON_VMREAD:     // 23
    case EXIT_REASON_VMWRITE:    // 25
    case EXIT_REASON_VMLAUNCH:   // 20
    case EXIT_REASON_VMRESUME:   // 24
    case EXIT_REASON_INVEPT:     // 50
    case EXIT_REASON_INVVPID:    // 53
    case EXIT_REASON_VMFUNC:     // 59
        inject_exception(6 /* #UD */, 0);
        break;

    // --- Fatal guest conditions ------------------------------------------------
    case EXIT_REASON_TRIPLE_FAULT: // 2
        serial_print("[!] TRIPLE FAULT in guest at RIP=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_RIP));
        serial_print("\n");
        vga_puts("[Aria] guest triple fault");
        halt_forever();
        break;

    case EXIT_REASON_EPT_VIOLATION: // 48
        // Should not happen while the EPT is a full identity map. If it does, the
        // EPT is inconsistent and continuing would corrupt the guest silently.
        serial_print("[!] EPT violation GPA=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_PHYS_ADDR));
        serial_print(" RIP=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_RIP));
        serial_print("\n");
        vga_puts("[Aria] unexpected EPT violation");
        halt_forever();
        break;

    case EXIT_REASON_EPT_MISCONFIG: // 49
        serial_print("[!] EPT MISCONFIG at GPA ");
        serial_print_hex(vmcs_read64(VMCS_GUEST_PHYS_ADDR));
        serial_print("\n");
        vga_puts("[Aria] EPT misconfiguration");
        halt_forever();
        break;

    default:
        serial_print("[!] Unhandled exit reason ");
        serial_print_dec(exit_reason);
        serial_print(" RIP=");
        serial_print_hex(vmcs_read64(VMCS_GUEST_RIP));
        serial_print(" qual=");
        serial_print_hex(vmcs_read64(VMCS_EXIT_QUALIFICATION));
        serial_print("\n");
#if ARIA_STRICT_EXITS
        vga_puts("[Aria] unhandled VM exit - see serial");
        halt_forever();
#else
        // Debug only: skipping is correct for instruction-caused exits and wrong
        // for event-caused ones, where VM_EXIT_INSN_LEN is undefined.
        advance_guest_rip();
#endif
        break;
    }
    return;
}
