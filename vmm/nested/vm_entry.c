// vmm/nested/vm_entry.c — L1 → L2 entry sequence (spec §4.1)
//
// Complete sequence:
// 1. VMCS12 already populated via intercepted VMWRITEs
// 2. Validate VMCS12 per SDM §26.2-26.3
// 3. Cache L0 host fields from VMCS01 (current)
// 4. XSAVE L1's FPU/SSE/AVX state
// 5. Initialize shadow EPT if needed
// 6. VMCLEAR VMCS02 (always — no VMRESUME on reuse)
// 7. VMPTRLD VMCS02
// 8. Write all merged fields via merge table
// 9. Record entry TSC for timer accounting
// 10. VMLAUNCH into L2
//
// CRITICAL: VMCLEAR on every entry guarantees correctness and eliminates
// cross-CPU VMCS ownership issues. Cost: ~200 cycles. Negligible.

#include "vm_entry.h"
#include "vmcs_merge.h"
#include "nested_util.h"
#include "vmcs_shadow.h"
#include "../vmcs.h"
#include "../../efi/serial.h"

// VMX intrinsics
extern uint8_t __vmx_vmclear(uint64_t* pa);
extern uint8_t __vmx_vmptrld(uint64_t* pa);
extern uint64_t __rdtsc(void);
extern void __xsave(void* area, uint64_t mask);
extern void __xrstor(void* area, uint64_t mask);

// Assembly: VMLAUNCH and return to guest_resume on success
extern int vmx_do_launch(void* state);

int nested_enter_l2(NESTED_STATE* ns) {

    // ================================================================
    // Step 1: VMCS12 is already complete (all VMWRITEs intercepted)
    // ================================================================

    // ================================================================
    // Step 2: Validate VMCS12
    // ================================================================
    int err = vmcs12_validate_controls(&ns->vmcs12);
    if (err) {
        DBG_PRINT("[nested] L2 entry: control validation failed\n");
        return err;
    }

    err = vmcs12_validate_guest_state(&ns->vmcs12);
    if (err) {
        DBG_PRINT("[nested] L2 entry: guest state validation failed\n");
        return err;
    }

    // ================================================================
    // Step 3: Cache L0 host fields from VMCS01 (must be current)
    // These are needed by the merge table for FROM_L0 and UNION_OR rules.
    // After we VMPTRLD VMCS02, we can't read VMCS01 anymore.
    // ================================================================
    vmcs_merge_cache_l0(ns);

    // Also cache VMCS01's PA for switching back during exit reflection
    // (ns->vmcs01_pa should already be set during initial VMM setup)

    // ================================================================
    // Step 4: XSAVE L1's FPU/SSE/AVX state
    // L2 will use different XMM registers. We save L1's state now
    // and restore it when reflecting an exit back to L1.
    // ================================================================
    __xsave(ns->l1_xsave_area, 0x7); // components: x87 + SSE + AVX

    // ================================================================
    // Step 5: Initialize shadow EPT if needed
    // ================================================================
    uint64_t l1_eptp = vmcs12_read(&ns->vmcs12, VMCS_EPT_POINTER);
    if (l1_eptp) {
        uint32_t eptp_idx = ns->active_eptp_index;
        SHADOW_EPT_CONTEXT* ctx = &ns->shadow_epts[eptp_idx];
        if (!ctx->valid || ctx->l1_eptp != l1_eptp) {
            ctx->l1_eptp = l1_eptp;
            shadow_ept_init(ns, eptp_idx);
        }
    }

    // ================================================================
    // Step 6: VMCLEAR VMCS02
    // Always VMCLEAR before reuse. Puts VMCS02 in "clear" state.
    // This eliminates cross-CPU ownership issues and makes VMLAUNCH
    // (not VMRESUME) the correct instruction. ~200 cycles.
    // ================================================================
    __vmx_vmclear(&ns->vmcs02_pa);

    // ================================================================
    // Step 7: VMPTRLD VMCS02
    // VMCS02 is now "clear" and current on this CPU.
    // ================================================================
    __vmx_vmptrld(&ns->vmcs02_pa);

    // ================================================================
    // Step 8: Write all merged fields
    // Uses L0 field cache (step 3) + VMCS12 software copy.
    // VMCS02 is current — all vmcs_write64 calls go to VMCS02.
    // ================================================================
    err = vmcs_merge_build_vmcs02(ns);
    if (err) {
        // Merge failed — switch back to VMCS01
        __vmx_vmptrld(&ns->vmcs01_pa);
        __xrstor(ns->l1_xsave_area, 0x7);
        DBG_PRINT("[nested] L2 entry: merge failed\n");
        return err;
    }

    // Optional: validate merged VMCS02 before launch
    // (Uncomment during debugging, comment out for performance in release)
    #ifdef ARIA_DEBUG
    err = vmcs_merge_validate(ns);
    if (err) {
        __vmx_vmptrld(&ns->vmcs01_pa);
        __xrstor(ns->l1_xsave_area, 0x7);
        DBG_PRINT("[nested] L2 entry: validation failed, err=");
        DBG_DEC(err);
        DBG_PRINT("\n");
        return err;
    }
    #endif

    // ================================================================
    // Step 9: Record entry TSC for preemption timer accounting
    // ================================================================
    ns->l2_entry_tsc = __rdtsc();
    ns->l2_running = 1;

    // ================================================================
    // Step 10: VMLAUNCH into L2
    // Always VMLAUNCH (not VMRESUME) because we VMCLEAR'd in step 6.
    // On success: L2 begins executing. We return here only when L2
    // causes a VM exit and our exit handler processes it.
    // ================================================================
    DBG_PRINT("[nested] Entering L2 via VMLAUNCH\n");

    int launch_result = vmx_do_launch(NULL);

    if (launch_result != 0) {
        // VMLAUNCH failed — read error code
        uint32_t vm_err = vmcs_read32(VMCS_VM_INSTRUCTION_ERROR);
        DBG_PRINT("[nested] VMLAUNCH failed: error=");
        DBG_DEC(vm_err);
        DBG_PRINT("\n");

        // Switch back to VMCS01 and restore L1 state
        __vmx_vmptrld(&ns->vmcs01_pa);
        __xrstor(ns->l1_xsave_area, 0x7);
        ns->l2_running = 0;

        return (int)vm_err;
    }

    // L2 is now running. Execution reaches here when L2 eventually causes
    // a VM exit, our exit_stub.asm saves state, vmm_handle_exit classifies
    // the exit, and if it was reflected to L1 and L1 did VMRESUME, we
    // re-enter here through nested_enter_l2 again.

    return 0;
}
