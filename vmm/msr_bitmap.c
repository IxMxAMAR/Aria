// vmm/msr_bitmap.c — build the VMX MSR bitmaps
//
// The MSR bitmap is 4KB, laid out as four 1KB regions (SDM Vol 3C 25.6.9):
//
//   0x000  read,  MSRs 0x00000000-0x00001FFF
//   0x400  read,  MSRs 0xC0000000-0xC0001FFF
//   0x800  write, MSRs 0x00000000-0x00001FFF
//   0xC00  write, MSRs 0xC0000000-0xC0001FFF
//
// A set bit causes a VM exit for that MSR; a clear bit lets the access run
// natively on the hardware. Only the write side is populated here: reads return
// the real MSR, which handle_msr_write() keeps in sync with the VMCS, so a read
// needs no exit.
//
// Why any of this is necessary: VM entry reloads several MSRs *from the VMCS*
// (IA32_EFER via entry control bit 15, and FS/GS base, SYSENTER and DEBUGCTL
// unconditionally). A guest that writes one of those MSRs natively has the
// write silently reverted at the next entry, because the VMCS field it is
// reloaded from was never updated. Intercepting the write and mirroring it into
// the VMCS is what makes the guest's value stick.

#include "vmm.h"
#include "vmcs.h"

#define MSR_BITMAP_SIZE       4096u
#define MSR_BITMAP_READ_LOW   0x000u
#define MSR_BITMAP_READ_HIGH  0x400u
#define MSR_BITMAP_WRITE_LOW  0x800u
#define MSR_BITMAP_WRITE_HIGH 0xC00u

// MSRs that VM entry reloads from the VMCS. Writing any of these natively would
// be undone on the next entry, so each must exit and be mirrored.
static const uint32_t aria_synced_msrs[] = {
    0x00000174, // IA32_SYSENTER_CS    -> VMCS_GUEST_SYSENTER_CS
    0x00000175, // IA32_SYSENTER_ESP   -> VMCS_GUEST_SYSENTER_ESP
    0x00000176, // IA32_SYSENTER_EIP   -> VMCS_GUEST_SYSENTER_EIP
    0x000001D9, // IA32_DEBUGCTL       -> VMCS_GUEST_IA32_DEBUGCTL
    0x00000277, // IA32_PAT            -> VMCS_GUEST_IA32_PAT
    0xC0000080, // IA32_EFER           -> VMCS_GUEST_IA32_EFER
    0xC0000100, // IA32_FS_BASE        -> VMCS_GUEST_FS_BASE
    0xC0000101, // IA32_GS_BASE        -> VMCS_GUEST_GS_BASE
};

#define ARIA_SYNCED_MSR_COUNT \
    (sizeof(aria_synced_msrs) / sizeof(aria_synced_msrs[0]))

static void msr_bitmap_set_write(uint8_t* bitmap, uint32_t msr) {
    uint32_t idx;
    if ((msr & 0xC0000000u) == 0xC0000000u) {
        idx = msr & 0x1FFFu;
        bitmap[MSR_BITMAP_WRITE_HIGH + (idx >> 3)] |= (uint8_t)(1u << (idx & 7u));
    } else if (msr <= 0x1FFFu) {
        idx = msr;
        bitmap[MSR_BITMAP_WRITE_LOW + (idx >> 3)] |= (uint8_t)(1u << (idx & 7u));
    }
}

// Populate a 4KB, 4KB-aligned MSR bitmap. Everything not listed above passes
// through to the hardware, which is the fast path.
void aria_build_msr_bitmap(void* bitmap) {
    uint8_t* bm = (uint8_t*)bitmap;
    for (uint32_t i = 0; i < MSR_BITMAP_SIZE; i++) {
        bm[i] = 0;
    }
    for (uint32_t i = 0; i < ARIA_SYNCED_MSR_COUNT; i++) {
        msr_bitmap_set_write(bm, aria_synced_msrs[i]);
    }
}
