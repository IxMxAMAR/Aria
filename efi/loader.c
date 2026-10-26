// efi/loader.c — make the hypervisor survive the guest calling ExitBootServices
//
// The problem: the firmware loads this module into EfiLoaderCode/EfiLoaderData.
// When the guest OS calls ExitBootServices, that memory becomes free for the OS
// to reuse, but the VMCS still points HOST_RIP at the exit stub inside it and
// HOST_CR3 at the firmware's page tables. CPUID is intercepted, so the first VM
// exit after the handoff lands in reused memory and faults.
//
// The fix: keep a copy of the whole image in EfiRuntimeServicesCode (which the
// OS must preserve), give the host its own CR3, GDT, IDT and TSS in
// EfiRuntimeServicesData, and point the VMCS at them at launch time rather than
// at the handoff — an AP has to launch with the relocated host state already
// installed, because it cannot repair its own VMCS afterwards.
//
// No relocation engine is needed. The image is verified position independent:
// it has no .reloc section, no BASERELOC data directory, ImageBase is 0, and a
// scan of .data finds no absolute pointers into the image. So a byte copy is
// valid at any base, and every image-relative address shifts by one constant.

#include "loader.h"
#include "serial.h"

#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/LoadedImage.h>

#include "../vmm/vmm.h"
#include "../vmm/vmcs.h"

ARIA_LOADER g_loader = {0};

// Defined in vmm/asm/exit_stub.asm
extern void   vm_exit_stub(void);
extern void   host_fault_halt(void);
extern UINT64 aria_vmcall(UINT64 nr);

// How much physical address space the host identity map covers. The relocated
// image and the host stack both come from AllocateMaxAddress below 4GB, so this
// only has to be generous, not exact.
#define HOST_PT_GIGABYTES  16
#define GDT_TSS_OFFSET     0x80
#define TSS_LIMIT          0x67        // 104-byte 64-bit TSS

// Allocate zeroed runtime pages below 4GB so they fall inside the host map.
static UINT64 alloc_rt(UINTN pages, EFI_MEMORY_TYPE type) {
    EFI_PHYSICAL_ADDRESS pa = 0xFFFFFFFFULL;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, type, pages, &pa))) {
        return 0;
    }
    ZeroMem((void*)(UINTN)pa, pages * PAGE_SIZE);
    return (UINT64)pa;
}

// Identity map HOST_PT_GIGABYTES with 2MB pages: one PML4, one PDPT, and one PD
// per gigabyte. 2MB pages rather than 1GB so this does not depend on PDPTE1GB.
static EFI_STATUS build_host_cr3(void) {
    UINT64 pml4 = alloc_rt(1, EfiRuntimeServicesData);
    UINT64 pdpt = alloc_rt(1, EfiRuntimeServicesData);
    if (pml4 == 0 || pdpt == 0) return EFI_OUT_OF_RESOURCES;

    ((UINT64*)(UINTN)pml4)[0] = pdpt | 0x3ULL;   // present, writable

    for (UINTN gi = 0; gi < HOST_PT_GIGABYTES; gi++) {
        UINT64 pd = alloc_rt(1, EfiRuntimeServicesData);
        if (pd == 0) return EFI_OUT_OF_RESOURCES;
        ((UINT64*)(UINTN)pdpt)[gi] = pd | 0x3ULL;

        for (UINTN pi = 0; pi < 512; pi++) {
            UINT64 phys = ((UINT64)gi << 30) | ((UINT64)pi << 21);
            ((UINT64*)(UINTN)pd)[pi] = phys | 0x3ULL | (1ULL << 7);  // PS: 2MB
        }
    }

    g_loader.host_cr3_pa = pml4;
    return EFI_SUCCESS;
}

// Build a flat 64-bit GDT with a TSS descriptor, plus the TSS itself, in one
// runtime page: GDT at offset 0, TSS at GDT_TSS_OFFSET.
static EFI_STATUS build_host_gdt_tss(void) {
    UINT64 page = alloc_rt(1, EfiRuntimeServicesData);
    if (page == 0) return EFI_OUT_OF_RESOURCES;

    UINT64* gdt = (UINT64*)(UINTN)page;

    // 0x08: 64-bit code, DPL0, present, L=1
    gdt[1] = 0x00AF9B000000FFFFULL;
    // 0x10: data, DPL0, present, writable
    gdt[2] = 0x00CF93000000FFFFULL;

    UINT64 tss_base  = page + GDT_TSS_OFFSET;
    UINT64 tss_limit = TSS_LIMIT;

    // 64-bit TSS descriptor: 16 bytes, base split across the two halves.
    UINT64 lo = (tss_limit & 0xFFFFULL)
              | ((tss_base & 0xFFFFULL) << 16)
              | (((tss_base >> 16) & 0xFFULL) << 32)
              | (0x89ULL << 40)                      // P=1, DPL=0, type=9
              | (((tss_limit >> 16) & 0xFULL) << 48)
              | (((tss_base >> 24) & 0xFFULL) << 56);
    gdt[3] = lo;
    gdt[4] = (tss_base >> 32) & 0xFFFFFFFFULL;

    // TSS.RSP0 (offset 4) = top of the host stack, which is already runtime data.
    *(UINT64*)(UINTN)(tss_base + 4) = (UINT64)(UINTN)g_vmm.host_stack + 0x10000;

    g_loader.host_gdt_base = page;
    g_loader.host_tr_base  = tss_base;
    g_loader.host_tr_sel   = 0x18;
    return EFI_SUCCESS;
}

// One page of IDT, every vector pointing at host_fault_halt. Interrupts are
// disabled in the exit stub, so this only has to make a host-context fault loud
// instead of a triple fault.
static EFI_STATUS build_host_idt(UINT64 handler) {
    UINT64 page = alloc_rt(1, EfiRuntimeServicesData);
    if (page == 0) return EFI_OUT_OF_RESOURCES;

    UINT64* idt = (UINT64*)(UINTN)page;
    for (UINTN i = 0; i < 256; i++) {
        idt[i * 2]     = (handler & 0xFFFFULL)
                       | ((UINT64)0x08 << 16)          // code selector
                       | ((UINT64)0x8E << 40)          // P=1, DPL0, interrupt gate
                       | (((handler >> 16) & 0xFFFFULL) << 48);
        idt[i * 2 + 1] = (handler >> 32) & 0xFFFFFFFFULL;
    }

    g_loader.host_idt_base = page;
    return EFI_SUCCESS;
}

EFI_STATUS aria_loader_prepare(void) {
    EFI_LOADED_IMAGE_PROTOCOL* li = NULL;
    EFI_STATUS st = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid,
                                        (VOID**)&li);
    if (EFI_ERROR(st) || li == NULL || li->ImageSize == 0) {
        serial_print("[!] loader: cannot read own image\n");
        return EFI_NOT_FOUND;
    }

    g_loader.image_old_base = (UINT64)(UINTN)li->ImageBase;
    g_loader.image_size     = (UINT64)li->ImageSize;

    UINTN pages = (UINTN)((g_loader.image_size + PAGE_SIZE - 1) / PAGE_SIZE);
    UINT64 new_base = alloc_rt(pages, EfiRuntimeServicesCode);
    if (new_base == 0) {
        serial_print("[!] loader: image copy allocation failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    g_loader.image_new_base = new_base;
    g_loader.delta          = new_base - g_loader.image_old_base;

    if (EFI_ERROR(build_host_cr3()))     return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR(build_host_gdt_tss())) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR(build_host_idt((UINT64)(UINTN)host_fault_halt + g_loader.delta))) {
        return EFI_OUT_OF_RESOURCES;
    }

    serial_print("[+] loader: image 0x");
    serial_print_hex(g_loader.image_size);
    serial_print(" at 0x");
    serial_print_hex(g_loader.image_new_base);
    serial_print(", host CR3 0x");
    serial_print_hex(g_loader.host_cr3_pa);
    serial_print("\n");

    g_loader.prepared = TRUE;
    return EFI_SUCCESS;
}

// Copy the live image into the runtime buffer. Safe to call more than once: the
// end of setup calls it so the relocated stub is valid from launch time, and the
// handoff calls it again so the copy sees the final globals.
void aria_loader_snapshot(void) {
    if (!g_loader.prepared || g_loader.image_new_base == 0) return;
    UINT8* dst = (UINT8*)(UINTN)g_loader.image_new_base;
    const UINT8* src = (const UINT8*)(UINTN)g_loader.image_old_base;
    for (UINT64 i = 0; i < g_loader.image_size; i++) {
        dst[i] = src[i];
    }
}

// Point one VMCS's host state at the runtime copy and the runtime tables.
// Must run on the processor that owns the VMCS, with that VMCS current, so it is
// called for the BSP after vmx_setup_vmcs and for each AP inside its own
// start-up procedure. Absolute runtime values stored in the image (EPT pointers,
// VMCS physical addresses) are not image-relative, so copying the image does not
// disturb them.
void aria_loader_apply_host_state(VMM_STATE* state) {
    if (!g_loader.prepared || state == NULL) return;

    vmcs_write64(VMCS_HOST_RIP, (UINT64)(UINTN)vm_exit_stub + g_loader.delta);
    vmcs_write64(VMCS_HOST_RSP, (UINT64)(UINTN)state->host_stack + 0x10000);
    vmcs_write64(VMCS_HOST_CR3, g_loader.host_cr3_pa);
    vmcs_write64(VMCS_HOST_GDTR_BASE, g_loader.host_gdt_base);
    vmcs_write64(VMCS_HOST_IDTR_BASE, g_loader.host_idt_base);
    vmcs_write16(VMCS_HOST_TR_SEL, g_loader.host_tr_sel);
    vmcs_write64(VMCS_HOST_TR_BASE, g_loader.host_tr_base);
}

// Runs in root context (hypercall 4). The host state was already installed at
// launch, so there is nothing left to relocate; what is left is to *check* that
// it really does point at runtime-preserved memory, and record the handoff.
//
// It deliberately does not re-copy the image. This code runs from the relocated
// copy, which has been the running hypervisor since the first VM exit, so copying
// the original over it would overwrite live state with a snapshot frozen at setup
// time: the exit counters, the accumulated TSC offset and g_vmm would all jump
// backwards, and the finalized flag set just below would be overwritten by the
// copy it had just made — defeating the idempotence check that is supposed to
// stop this running twice. The single snapshot at the end of setup is the
// correct one.
void aria_loader_finalize(void) {
    if (g_loader.finalized) return;
    if (!g_loader.prepared || g_loader.image_new_base == 0) return;
    g_loader.finalized = TRUE;

    UINT64 rip = vmcs_read64(VMCS_HOST_RIP);
    UINT64 cr3 = vmcs_read64(VMCS_HOST_CR3);
    UINT64 lo  = g_loader.image_new_base;
    UINT64 hi  = lo + g_loader.image_size;

    serial_print("[+] loader: handoff, HOST_RIP=0x");
    serial_print_hex(rip);
    serial_print(" HOST_CR3=0x");
    serial_print_hex(cr3);
    serial_print("\n");

    // A host RIP outside the runtime copy, or a host CR3 that is not the identity
    // map, means the first exit after the handoff lands in memory the OS is about
    // to reuse. Say so now, on the record, instead of faulting later unexplained.
    if (rip < lo || rip >= hi) {
        serial_print("[!] loader: HOST_RIP outside the runtime image - handoff unsafe\n");
    }
    if (cr3 != g_loader.host_cr3_pa) {
        serial_print("[!] loader: HOST_CR3 is not the host identity map - handoff unsafe\n");
    }
}

// ---------------------------------------------------------------------------
// ExitBootServices hook
//
// The hook runs as the guest (the OS is the one calling ExitBootServices), so it
// cannot VMWRITE. It issues a hypercall, which exits to root where the
// finalisation can touch the VMCS.
// ---------------------------------------------------------------------------
static EFI_EXIT_BOOT_SERVICES m_orig_exit_boot_services = NULL;

// Set in this image's own .data. aria_loader_finalize() runs in the relocated
// copy and so cannot clear or set a flag that this code can see: g_loader here
// and g_loader there are two different objects. The guard therefore has to live
// on this side. ExitBootServices may legitimately be called more than once (the
// OS retries with a fresh map key if the memory map changed), and the handoff
// only needs recording once.
static BOOLEAN m_finalize_sent = FALSE;

static EFI_STATUS EFIAPI aria_exit_boot_services(EFI_HANDLE ImageHandle, UINTN MapKey) {
    if (g_loader.active && !m_finalize_sent) {
        m_finalize_sent = TRUE;
        aria_vmcall(ARIA_HC_FINALIZE);
    }
    return m_orig_exit_boot_services(ImageHandle, MapKey);
}

EFI_STATUS aria_loader_hook_exit_boot_services(void) {
    if (m_orig_exit_boot_services != NULL) return EFI_SUCCESS;
    m_orig_exit_boot_services = gBS->ExitBootServices;
    if (m_orig_exit_boot_services == NULL) return EFI_NOT_FOUND;
    gBS->ExitBootServices = aria_exit_boot_services;
    return EFI_SUCCESS;
}
