// vmm/vmcs.h — VMCS field encoding constants and access functions
#pragma once

#include "vmm.h"

// 16-bit control fields
#define VMCS_VPID                   0x00000000

// 16-bit guest state
#define VMCS_GUEST_ES_SEL           0x00000800
#define VMCS_GUEST_CS_SEL           0x00000802
#define VMCS_GUEST_SS_SEL           0x00000804
#define VMCS_GUEST_DS_SEL           0x00000806
#define VMCS_GUEST_FS_SEL           0x00000808
#define VMCS_GUEST_GS_SEL          0x0000080A
#define VMCS_GUEST_LDTR_SEL         0x0000080C
#define VMCS_GUEST_TR_SEL           0x0000080E
#define VMCS_GUEST_INTR_STATUS      0x00000810

// 16-bit host state
#define VMCS_HOST_ES_SEL            0x00000C00
#define VMCS_HOST_CS_SEL            0x00000C02
#define VMCS_HOST_SS_SEL            0x00000C04
#define VMCS_HOST_DS_SEL            0x00000C06
#define VMCS_HOST_FS_SEL            0x00000C08
#define VMCS_HOST_GS_SEL            0x00000C0A
#define VMCS_HOST_TR_SEL            0x00000C0C

// 32-bit control fields
#define VMCS_PIN_BASED_EXEC_CTRL    0x00004000
#define VMCS_PROC_BASED_EXEC_CTRL   0x00004002
#define VMCS_EXCEPTION_BITMAP       0x00004004
#define VMCS_PF_ERROR_CODE_MASK     0x00004006
#define VMCS_PF_ERROR_CODE_MATCH    0x00004008
#define VMCS_CR3_TARGET_COUNT       0x0000400A
#define VMCS_VM_EXIT_CTRLS          0x0000400C
#define VMCS_VM_EXIT_MSR_STORE_CNT  0x0000400E
#define VMCS_VM_EXIT_MSR_LOAD_CNT   0x00004010
#define VMCS_VM_ENTRY_CTRLS         0x00004012
#define VMCS_VM_ENTRY_MSR_LOAD_CNT  0x00004014
#define VMCS_VM_ENTRY_INTR_INFO     0x00004016
#define VMCS_VM_ENTRY_EXCEPTION_EC  0x00004018
#define VMCS_VM_ENTRY_INSN_LEN      0x0000401A
#define VMCS_TPR_THRESHOLD          0x0000401C
#define VMCS_PROC_BASED_EXEC_CTRL2  0x0000401E
#define VMCS_PLE_GAP                0x00004020
#define VMCS_PLE_WINDOW             0x00004022

// 32-bit read-only
#define VMCS_VM_INSTRUCTION_ERROR   0x00004400
#define VMCS_EXIT_REASON            0x00004402
#define VMCS_VM_EXIT_INTR_INFO      0x00004404
#define VMCS_VM_EXIT_INTR_EC        0x00004406
#define VMCS_IDT_VECTORING_INFO     0x00004408
#define VMCS_IDT_VECTORING_EC       0x0000440A
#define VMCS_VM_EXIT_INSN_LEN       0x0000440C
#define VMCS_VM_EXIT_INSN_INFO      0x0000440E

// 32-bit guest state
#define VMCS_GUEST_ES_LIMIT         0x00004800
#define VMCS_GUEST_CS_LIMIT         0x00004802
#define VMCS_GUEST_SS_LIMIT         0x00004804
#define VMCS_GUEST_DS_LIMIT         0x00004806
#define VMCS_GUEST_FS_LIMIT         0x00004808
#define VMCS_GUEST_GS_LIMIT         0x0000480A
#define VMCS_GUEST_LDTR_LIMIT       0x0000480C
#define VMCS_GUEST_TR_LIMIT         0x0000480E
#define VMCS_GUEST_GDTR_LIMIT       0x00004810
#define VMCS_GUEST_IDTR_LIMIT       0x00004812
#define VMCS_GUEST_ES_AR            0x00004814
#define VMCS_GUEST_CS_AR            0x00004816
#define VMCS_GUEST_SS_AR            0x00004818
#define VMCS_GUEST_DS_AR            0x0000481A
#define VMCS_GUEST_FS_AR            0x0000481C
#define VMCS_GUEST_GS_AR            0x0000481E
#define VMCS_GUEST_LDTR_AR          0x00004820
#define VMCS_GUEST_TR_AR            0x00004822
#define VMCS_GUEST_INTERRUPTIBILITY 0x00004824
#define VMCS_GUEST_ACTIVITY         0x00004826
#define VMCS_GUEST_SMBASE           0x00004828
#define VMCS_GUEST_SYSENTER_CS      0x0000482A
#define VMCS_GUEST_PREEMPT_TIMER    0x0000482E

// 32-bit host state
#define VMCS_HOST_SYSENTER_CS       0x00004C00

// 64-bit control fields
#define VMCS_IO_BITMAP_A            0x00002000
#define VMCS_IO_BITMAP_B            0x00002002
#define VMCS_MSR_BITMAP             0x00002004
#define VMCS_VM_EXIT_MSR_STORE      0x00002006
#define VMCS_VM_EXIT_MSR_LOAD       0x00002008
#define VMCS_VM_ENTRY_MSR_LOAD      0x0000200A
#define VMCS_EXECUTIVE_VMCS_PTR     0x0000200C
#define VMCS_TSC_OFFSET             0x00002010
#define VMCS_VIRTUAL_APIC_ADDR      0x00002012
#define VMCS_APIC_ACCESS_ADDR       0x00002014
#define VMCS_POSTED_INTR_DESC       0x00002016
#define VMCS_VMFUNC_CONTROLS        0x00002018
#define VMCS_EPT_POINTER            0x0000201A
#define VMCS_EOI_EXIT_BITMAP0       0x0000201C
#define VMCS_EOI_EXIT_BITMAP1       0x0000201E
#define VMCS_EOI_EXIT_BITMAP2       0x00002020
#define VMCS_EOI_EXIT_BITMAP3       0x00002022
#define VMCS_EPTP_LIST_ADDR         0x00002024
#define VMCS_VMREAD_BITMAP          0x00002026
#define VMCS_VMWRITE_BITMAP         0x00002028
#define VMCS_VMCS_LINK_PTR          0x00002800

// 64-bit read-only
#define VMCS_GUEST_PHYS_ADDR        0x00002400

// 64-bit guest state
#define VMCS_GUEST_IA32_DEBUGCTL    0x00002802
#define VMCS_GUEST_IA32_PAT         0x00002804
#define VMCS_GUEST_IA32_EFER        0x00002806
#define VMCS_GUEST_IA32_PERF_GLOBAL 0x00002808
#define VMCS_GUEST_PDPTE0           0x0000280A
#define VMCS_GUEST_PDPTE1           0x0000280C
#define VMCS_GUEST_PDPTE2           0x0000280E
#define VMCS_GUEST_PDPTE3           0x00002810
#define VMCS_GUEST_IA32_BNDCFGS     0x00002812

// 64-bit host state
#define VMCS_HOST_IA32_PAT          0x00002C00
#define VMCS_HOST_IA32_EFER         0x00002C02
#define VMCS_HOST_IA32_PERF_GLOBAL  0x00002C04

// Natural-width control fields
#define VMCS_CR0_GUEST_HOST_MASK    0x00006000
#define VMCS_CR4_GUEST_HOST_MASK    0x00006002
#define VMCS_CR0_READ_SHADOW        0x00006004
#define VMCS_CR4_READ_SHADOW        0x00006006

// Natural-width read-only
#define VMCS_EXIT_QUALIFICATION     0x00006400
#define VMCS_IO_RCX                 0x00006402
#define VMCS_IO_RSI                 0x00006404
#define VMCS_IO_RDI                 0x00006406
#define VMCS_IO_RIP                 0x00006408
#define VMCS_GUEST_LINEAR_ADDR      0x0000640A

// Natural-width guest state
#define VMCS_GUEST_CR0              0x00006800
#define VMCS_GUEST_CR3              0x00006802
#define VMCS_GUEST_CR4              0x00006804
#define VMCS_GUEST_ES_BASE          0x00006806
#define VMCS_GUEST_CS_BASE          0x00006808
#define VMCS_GUEST_SS_BASE          0x0000680A
#define VMCS_GUEST_DS_BASE          0x0000680C
#define VMCS_GUEST_FS_BASE          0x0000680E
#define VMCS_GUEST_GS_BASE          0x00006810
#define VMCS_GUEST_LDTR_BASE        0x00006812
#define VMCS_GUEST_TR_BASE          0x00006814
#define VMCS_GUEST_GDTR_BASE        0x00006816
#define VMCS_GUEST_IDTR_BASE        0x00006818
#define VMCS_GUEST_DR7              0x0000681A
#define VMCS_GUEST_RSP              0x0000681C
#define VMCS_GUEST_RIP              0x0000681E
#define VMCS_GUEST_RFLAGS           0x00006820
#define VMCS_GUEST_PENDING_DBG      0x00006822
#define VMCS_GUEST_SYSENTER_ESP     0x00006824
#define VMCS_GUEST_SYSENTER_EIP     0x00006826

// CET fields
#define VMCS_GUEST_IA32_S_CET                   0x6828
#define VMCS_GUEST_SSP                          0x682A
#define VMCS_GUEST_IA32_INTERRUPT_SSP_TABLE     0x682C
#define VMCS_HOST_IA32_S_CET                    0x6C18
#define VMCS_HOST_SSP                           0x6C1A
#define VMCS_HOST_IA32_INTERRUPT_SSP_TABLE      0x6C1C

// Natural-width host state
#define VMCS_HOST_CR0               0x00006C00
#define VMCS_HOST_CR3               0x00006C02
#define VMCS_HOST_CR4               0x00006C04
#define VMCS_HOST_FS_BASE           0x00006C06
#define VMCS_HOST_GS_BASE           0x00006C08
#define VMCS_HOST_TR_BASE           0x00006C0A
#define VMCS_HOST_GDTR_BASE         0x00006C0C
#define VMCS_HOST_IDTR_BASE         0x00006C0E
#define VMCS_HOST_SYSENTER_ESP      0x00006C10
#define VMCS_HOST_SYSENTER_EIP      0x00006C12
#define VMCS_HOST_RSP               0x00006C14
#define VMCS_HOST_RIP               0x00006C16

// VM exit reasons
#define EXIT_REASON_EXCEPTION_NMI   0
#define EXIT_REASON_EXTERNAL_INT    1
#define EXIT_REASON_TRIPLE_FAULT    2
#define EXIT_REASON_INIT_SIGNAL     3
#define EXIT_REASON_SIPI            4
#define EXIT_REASON_IO_SMI          5
#define EXIT_REASON_OTHER_SMI       6
#define EXIT_REASON_INT_WINDOW      7
#define EXIT_REASON_NMI_WINDOW      8
#define EXIT_REASON_TASK_SWITCH     9
#define EXIT_REASON_CPUID           10
#define EXIT_REASON_GETSEC          11
#define EXIT_REASON_HLT             12
#define EXIT_REASON_INVD            13
#define EXIT_REASON_INVLPG          14
#define EXIT_REASON_RDPMC           15
#define EXIT_REASON_RDTSC           16
#define EXIT_REASON_RSM             17
#define EXIT_REASON_VMCALL          18
#define EXIT_REASON_VMCLEAR         19
#define EXIT_REASON_VMLAUNCH        20
#define EXIT_REASON_VMPTRLD         21
#define EXIT_REASON_VMPTRST         22
#define EXIT_REASON_VMREAD          23
#define EXIT_REASON_VMRESUME        24
#define EXIT_REASON_VMWRITE         25
#define EXIT_REASON_VMXOFF          26
#define EXIT_REASON_VMXON           27
#define EXIT_REASON_CR_ACCESS       28
#define EXIT_REASON_DR_ACCESS       29
#define EXIT_REASON_IO              30
#define EXIT_REASON_MSR_READ        31
#define EXIT_REASON_MSR_WRITE       32
#define EXIT_REASON_INVALID_GUEST   33
#define EXIT_REASON_MSR_LOAD_FAIL   34
#define EXIT_REASON_MWAIT           36
#define EXIT_REASON_MTF             37
#define EXIT_REASON_MONITOR         39
#define EXIT_REASON_PAUSE           40
#define EXIT_REASON_MCE_DURING_ENTRY 41
#define EXIT_REASON_TPR_BELOW       43
#define EXIT_REASON_APIC_ACCESS     44
#define EXIT_REASON_VIRTUALIZED_EOI 45
#define EXIT_REASON_GDTR_IDTR       46
#define EXIT_REASON_LDTR_TR         47
#define EXIT_REASON_EPT_VIOLATION   48
#define EXIT_REASON_EPT_MISCONFIG   49
#define EXIT_REASON_INVEPT          50
#define EXIT_REASON_RDTSCP          51
#define EXIT_REASON_PREEMPT_TIMER   52
#define EXIT_REASON_INVVPID         53
#define EXIT_REASON_WBINVD          54
#define EXIT_REASON_XSETBV          55
#define EXIT_REASON_APIC_WRITE      56
#define EXIT_REASON_RDRAND          57
#define EXIT_REASON_INVPCID         58
#define EXIT_REASON_VMFUNC          59
#define EXIT_REASON_ENCLS           60
#define EXIT_REASON_RDSEED          61
#define EXIT_REASON_PML_FULL        62
#define EXIT_REASON_XSAVES          63
#define EXIT_REASON_XRSTORS         64

// VMCS read/write functions
void vmcs_write16(uint32_t field, uint16_t value);
void vmcs_write32(uint32_t field, uint32_t value);
void vmcs_write64(uint32_t field, uint64_t value);
uint16_t vmcs_read16(uint32_t field);
uint32_t vmcs_read32(uint32_t field);
uint64_t vmcs_read64(uint32_t field);
