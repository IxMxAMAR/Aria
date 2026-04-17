// vmm/nested/preempt_timer.c — Preemption timer MIN logic + accounting (spec §5.4)
#include "preempt_timer.h"
#include "../vmcs.h"
#include "../../efi/serial.h"

extern uint64_t __rdtsc(void);

void nested_preempt_setup(NESTED_STATE* ns, uint32_t l0_timer, uint32_t l1_timer) {
    ns->l0_preempt_value = l0_timer;
    ns->l1_preempt_value = l1_timer;
    ns->l2_entry_tsc = __rdtsc();
}

// Returns: 1 = reflect to L1 (L1's timer expired), 0 = L0 handles
int nested_preempt_handle_exit(NESTED_STATE* ns) {
    uint64_t now = __rdtsc();
    uint64_t vmx_misc = __readmsr(0x485); // IA32_VMX_MISC
    uint32_t shift = (uint32_t)(vmx_misc & 0x1F);
    uint64_t ticks_elapsed = (now - ns->l2_entry_tsc) >> shift;

    int l1_expired = (ticks_elapsed >= ns->l1_preempt_value);
    int l0_expired = (ticks_elapsed >= ns->l0_preempt_value);

    if (l1_expired) return 1; // reflect preemption timer exit to L1
    if (l0_expired) return 0; // L0 handles its own timer
    return 0; // spurious
}
