// vmm/nested/vpid_mgr.c — VPID namespace management (spec §9)
// L0 assigns VPID=1 to L1 in VMCS01. L1 assigns its own VPIDs to L2.
// We translate to avoid collisions: L0_physical_VPID = VPID_BASE + L1_logical_VPID
#include "vpid_mgr.h"

uint16_t vpid_allocate(NESTED_STATE* ns, uint16_t l1_vpid) {
    uint16_t translated = NESTED_VPID_BASE + l1_vpid;
    ns->l1_vpid = l1_vpid;
    ns->l0_vpid_translated = translated;
    return translated;
}

void vpid_free(NESTED_STATE* ns, uint16_t l1_vpid) {
    (void)l1_vpid;
    ns->l0_vpid_translated = 0;
    ns->l1_vpid = 0;
}

uint16_t vpid_translate(NESTED_STATE* ns, uint16_t l1_vpid) {
    if (l1_vpid == ns->l1_vpid && ns->l0_vpid_translated)
        return ns->l0_vpid_translated;
    return vpid_allocate(ns, l1_vpid);
}
