// efi/vmx_init.h — VT-x initialization
#pragma once

#include <Uefi.h>

#define CPUID_VMX_BIT       (1 << 5)
#define CR4_VMXE            (1 << 13)
#define CR0_NE              (1 << 5)
#define CR0_PE              (1 << 0)
#define CR0_PG              (1 << 31)

#define IA32_FEATURE_CONTROL     0x3A
#define IA32_VMX_BASIC           0x480
#define IA32_VMX_CR0_FIXED0     0x486
#define IA32_VMX_CR0_FIXED1     0x487
#define IA32_VMX_CR4_FIXED0     0x488
#define IA32_VMX_CR4_FIXED1     0x489
#define IA32_VMX_PINBASED_CTLS  0x481
#define IA32_VMX_PROCBASED_CTLS 0x482
#define IA32_VMX_EXIT_CTLS      0x483
#define IA32_VMX_ENTRY_CTLS     0x484
#define IA32_VMX_PROCBASED_CTLS2 0x48B
#define IA32_VMX_TRUE_PINBASED  0x48D
#define IA32_VMX_TRUE_PROCBASED 0x48E
#define IA32_VMX_TRUE_EXIT      0x48F
#define IA32_VMX_TRUE_ENTRY     0x490
#define IA32_DEBUGCTL            0x1D9
#define IA32_SYSENTER_CS         0x174
#define IA32_SYSENTER_ESP        0x175
#define IA32_SYSENTER_EIP        0x176
#define IA32_PAT                 0x277
#define IA32_EFER                0xC0000080
#define IA32_FS_BASE             0xC0000100
#define IA32_GS_BASE             0xC0000101
#define IA32_TSC_AUX             0xC0000103

#define FEATURE_CONTROL_LOCK     (1 << 0)
#define FEATURE_CONTROL_VMXON    (1 << 2)

typedef enum {
    VMX_OK = 0,
    VMX_NO_SUPPORT,
    VMX_LOCKED_OFF,
    VMX_ALLOC_FAIL,
    VMX_VMXON_FAIL,
    VMX_VMCS_FAIL,
    VMX_LAUNCH_FAIL
} VMX_STATUS;

typedef struct _VMM_STATE VMM_STATE;

VMX_STATUS vmx_check_support(void);
VMX_STATUS vmx_enable(VMM_STATE* state);
VMX_STATUS vmx_setup_vmcs(VMM_STATE* state);
VMX_STATUS vmx_launch(VMM_STATE* state);
