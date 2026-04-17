// vmm/exit_handlers/ept_violation.c — EPT violation handling
#include "../vmm.h"
#include "../vmcs.h"
#include "../ept.h"
#include "../channel.h"
#include "../../efi/serial.h"
#include "../../shared/protocol.h"

void handle_ept_violation(GUEST_REGS* regs) {
    uint64_t qualification = vmcs_read64(VMCS_EXIT_QUALIFICATION);
    uint64_t guest_phys    = vmcs_read64(VMCS_GUEST_PHYS_ADDR);
    uint64_t guest_linear  = vmcs_read64(VMCS_GUEST_LINEAR_ADDR);

    int caused_by_read    = (qualification >> 0) & 1;
    int caused_by_write   = (qualification >> 1) & 1;
    int caused_by_execute = (qualification >> 2) & 1;

    (void)caused_by_read;
    (void)caused_by_write;
    (void)guest_linear;

    if (caused_by_execute) {
        for (uint32_t i = 0; i < g_vmm.split_page_count; i++) {
            EPT_SPLIT_PAGE* sp = &g_vmm.split_pages[i];
            if (!sp->active) continue;

            uint64_t page_gpa = guest_phys & PAGE_MASK;
            if (page_gpa == sp->guest_physical) {
                ept_set_split_execute(&g_vmm, i);
                ept_invept_single(&g_vmm, guest_phys);
                return;
            }
        }
    }

    // Read on split page (currently in execute-only dirty state) — swap back
    if (caused_by_read || caused_by_write) {
        for (uint32_t i = 0; i < g_vmm.split_page_count; i++) {
            EPT_SPLIT_PAGE* sp = &g_vmm.split_pages[i];
            if (!sp->active) continue;
            uint64_t page_gpa = guest_phys & PAGE_MASK;
            if (page_gpa == sp->guest_physical) {
                ept_set_split_normal(&g_vmm, i);
                ept_invept_single(&g_vmm, guest_phys);
                return;
            }
        }
    }

    if (guest_phys >= g_vmm.vmm_base_pa &&
        guest_phys < g_vmm.vmm_base_pa + g_vmm.vmm_size) {
        uint32_t intr_info = (14) | (3 << 8) | (1 << 11) | (1u << 31);
        vmcs_write32(VMCS_VM_ENTRY_INTR_INFO, intr_info);
        vmcs_write32(VMCS_VM_ENTRY_EXCEPTION_EC, 0);
        return;
    }

    serial_print("[VMM] EPT violation: GPA=");
    serial_print_hex(guest_phys);
    serial_print(" qual=");
    serial_print_hex(qualification);
    serial_print("\n");

    uint32_t intr_info = (14) | (3 << 8) | (1 << 11) | (1u << 31);
    vmcs_write32(VMCS_VM_ENTRY_INTR_INFO, intr_info);
    vmcs_write32(VMCS_VM_ENTRY_EXCEPTION_EC, 0);
}
