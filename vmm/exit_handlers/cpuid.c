// vmm/exit_handlers/cpuid.c — CPUID interception
#include "../vmm.h"
#include "../vmcs.h"
#include "../channel.h"
#include "../../shared/protocol.h"
#include "../../vmm/nested/hv_interface.h"

extern void __cpuidex(int info[4], int leaf, int subleaf);

void handle_cpuid_exit(GUEST_REGS* regs) {
    uint32_t leaf    = (uint32_t)regs->rax;
    uint32_t subleaf = (uint32_t)regs->rcx;

    if (leaf == ARIA_CPUID_MAGIC) {
        uint32_t status = channel_dispatch(subleaf, regs);
        regs->rax = status;
        regs->rbx = ARIA_VERSION;
        regs->rcx = 0;
        regs->rdx = 0;
        return;
    }

    int info[4];
    __cpuidex(info, leaf, subleaf);

    regs->rax = (uint64_t)(uint32_t)info[0];
    regs->rbx = (uint64_t)(uint32_t)info[1];
    regs->rcx = (uint64_t)(uint32_t)info[2];
    regs->rdx = (uint64_t)(uint32_t)info[3];

    switch (leaf) {
    case 1:
        regs->rcx |= (1ULL << 31);
        break;

    case 0x40000000:
    case 0x40000001:
    case 0x40000002:
    case 0x40000003:
    case 0x40000004:
    case 0x40000005:
    case 0x40000006:
        hv_cpuid_impersonate(leaf, subleaf, regs);
        break;
    }
}
