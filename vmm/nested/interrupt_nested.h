// vmm/nested/interrupt_nested.h — Nested interrupt/NMI handling (spec §5.3, §5.6, §5.7)
#pragma once

#include "nested_vmx.h"

// Handle external interrupt from L2 (spec §5.3)
void nested_handle_external_interrupt(NESTED_STATE* ns, GUEST_REGS* regs, uint32_t vector);

// Handle NMI from L2 (spec §5.6) — propagates NMI-blocking bit
void nested_handle_nmi(NESTED_STATE* ns, GUEST_REGS* regs);

// Handle interrupt-window exit from L2 (spec §5.7) — prevents livelock
void nested_handle_interrupt_window(NESTED_STATE* ns, GUEST_REGS* regs);
