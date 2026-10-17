// efi/smp.c — virtualise every logical processor
//
// Only the BSP runs VMX until this does its work. Every other processor then
// runs the firmware natively, which means none of the intercepts apply to AP
// workloads and the guest sees inconsistent behaviour between cores. Here each
// AP brings up VMX on itself and launches in place, so it carries on running the
// firmware but as a guest.
//
// The launch deliberately does not go through vmx_launch(): that function writes
// to the UEFI console, which is not safe to call concurrently from several APs.
// It calls vmx_do_launch() directly instead, and keeps the reporting on serial.

#include "smp.h"
#include "serial.h"
#include "loader.h"
#include "vmx_init.h"

#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>

#include "../vmm/vmm.h"
#include "../vmm/vmcs.h"

// One AP state per non-BSP processor. 63 is well beyond anything this is likely
// to meet and keeps the bookkeeping trivial.
#define ARIA_MAX_AP 63

extern int vmx_do_launch(VMM_STATE* state);

static EFI_MP_SERVICES_PROTOCOL* m_mp        = NULL;
static VMM_STATE*                m_ap_state[ARIA_MAX_AP];
static UINTN                     m_ap_total  = 0;
static volatile UINTN            m_ap_ok     = 0;
static volatile UINTN            m_ap_failed = 0;

// Runs on each AP. Brings up VMX on that processor and launches it in place.
static VOID EFIAPI aria_ap_entry(IN VOID* Arg) {
    (void)Arg;

    UINTN index = 0;
    if (EFI_ERROR(m_mp->WhoAmI(m_mp, &index)) || index == 0 || index > ARIA_MAX_AP) {
        m_ap_failed++;
        return;
    }

    VMM_STATE* s = m_ap_state[index - 1];
    if (s == NULL) {
        m_ap_failed++;
        return;
    }

    if (vmx_enable(s) != VMX_OK) {
        m_ap_failed++;
        return;
    }
    if (vmx_setup_vmcs(s) != VMX_OK) {
        m_ap_failed++;
        return;
    }

    // The host state has to be installed before the launch, and on this
    // processor, because VMWRITE acts on whichever VMCS is current here.
    aria_loader_apply_host_state(s);

    if (vmx_do_launch(s) == 0) {
        s->launched = 1;
        m_ap_ok++;
    } else {
        s->target_pid = vmcs_read32(VMCS_VM_INSTRUCTION_ERROR);
        m_ap_failed++;
    }
}

EFI_STATUS aria_smp_init(void) {
    EFI_STATUS st = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid, NULL, (VOID**)&m_mp);
    if (EFI_ERROR(st) || m_mp == NULL) {
        serial_print("[!] smp: MP services unavailable, APs stay unvirtualised\n");
        return EFI_NOT_FOUND;
    }

    UINTN total = 0, enabled = 0;
    m_mp->GetNumberOfProcessors(m_mp, &total, &enabled);
    serial_print("[*] smp: ");
    serial_print_dec(total);
    serial_print(" processors, ");
    serial_print_dec(enabled);
    serial_print(" enabled\n");

    if (total <= 1) return EFI_SUCCESS;
    if (total - 1 > ARIA_MAX_AP) total = ARIA_MAX_AP + 1;
    m_ap_total = total - 1;

    for (UINTN i = 0; i < m_ap_total; i++) {
        EFI_PHYSICAL_ADDRESS pa;
        UINTN state_pages = (sizeof(VMM_STATE) + PAGE_SIZE - 1) / PAGE_SIZE;

        if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData,
                                         state_pages, &pa))) {
            serial_print("[!] smp: AP state allocation failed\n");
            break;
        }
        VMM_STATE* s = (VMM_STATE*)(UINTN)pa;
        // Inherit the EPT, EPTP, MSR bitmap and control settings from the BSP.
        CopyMem(s, &g_vmm, sizeof(VMM_STATE));

        if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa))) break;
        s->vmxon_region_va = (void*)(UINTN)pa;
        s->vmxon_region_pa = pa;
        ZeroMem(s->vmxon_region_va, PAGE_SIZE);

        if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa))) break;
        s->vmcs_region_va = (void*)(UINTN)pa;
        s->vmcs_region_pa = pa;
        ZeroMem(s->vmcs_region_va, PAGE_SIZE);

        // Below 4GB, because the host identity map built by efi/loader.c covers
        // 0-16GB and the exit handler runs on this stack.
        pa = 0xFFFFFFFFULL;
        if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData, 16, &pa))) break;
        s->host_stack = (void*)(UINTN)pa;
        s->host_stack_pa = pa;
        ZeroMem(s->host_stack, 0x10000);

        s->launched   = 0;
        s->target_pid = 0;
        m_ap_state[i] = s;
    }

    serial_print("[+] smp: prepared ");
    serial_print_dec(m_ap_total);
    serial_print(" AP states\n");
    return EFI_SUCCESS;
}

EFI_STATUS aria_smp_launch_aps(void) {
    if (m_mp == NULL || m_ap_total == 0) return EFI_SUCCESS;

    m_ap_ok = 0;
    m_ap_failed = 0;

    EFI_STATUS st = m_mp->StartupAllAPs(m_mp, aria_ap_entry, TRUE, NULL, 0, NULL, NULL);

    serial_print("[+] smp: ");
    serial_print_dec(m_ap_ok);
    serial_print(" of ");
    serial_print_dec(m_ap_total);
    serial_print(" APs virtualised");
    if (m_ap_failed) {
        serial_print(", ");
        serial_print_dec(m_ap_failed);
        serial_print(" failed");
    }
    serial_print("\n");

    return st;
}

UINTN aria_smp_ap_count(void)    { return m_ap_total; }
UINTN aria_smp_ap_launched(void) { return m_ap_ok; }
