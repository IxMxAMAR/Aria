// vmm/nested/preempt_timer.h — Preemption timer accounting (spec §5.4)
#pragma once
#include "nested_vmx.h"

void nested_preempt_setup(NESTED_STATE* ns, uint32_t l0_timer, uint32_t l1_timer);
int  nested_preempt_handle_exit(NESTED_STATE* ns);
