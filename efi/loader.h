// efi/loader.h — relocate the hypervisor into memory the OS preserves
#pragma once

#include <Uefi.h>

typedef struct {
    UINT64  image_old_base;   // where the firmware loaded this module
    UINT64  image_new_base;   // runtime-preserved copy
    UINT64  image_size;
    UINT64  delta;            // new_base - old_base

    UINT64  host_cr3_pa;      // identity page tables the host runs on
    UINT64  host_gdt_base;    // runtime GDT (also holds the TSS)
    UINT64  host_idt_base;    // runtime IDT, every vector halts
    UINT64  host_tr_base;
    UINT16  host_tr_sel;

    BOOLEAN prepared;
    BOOLEAN active;           // hypervisor launched; VMCALL is safe
    BOOLEAN finalized;
} ARIA_LOADER;

extern ARIA_LOADER g_loader;

// Allocate the runtime image buffer and the host CR3 / GDT / IDT / TSS.
// Must run before VMLAUNCH, while boot services are still available.
EFI_STATUS aria_loader_prepare(void);

// Install the ExitBootServices wrapper. The wrapper runs as the guest, so it
// asks the hypervisor to finalise through a hypercall rather than touching the
// VMCS itself (VMWRITE is not available in non-root operation).
EFI_STATUS aria_loader_hook_exit_boot_services(void);

// Copy the live image into the runtime buffer. Safe to call repeatedly: once at
// the end of setup so the relocated stub is valid from launch time, and again at
// the handoff so the copy sees the final globals.
void aria_loader_snapshot(void);

// Point one VMCS's host state at the runtime copy and the runtime tables. Must
// run on the processor that owns the VMCS, with that VMCS current.
struct _VMM_STATE;
void aria_loader_apply_host_state(struct _VMM_STATE* state);

// Root-side: refresh the runtime image with the final globals. Called from the
// VM-exit handler in response to hypercall 4.
void aria_loader_finalize(void);
