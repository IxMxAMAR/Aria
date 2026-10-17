// vmm/vmm.h — Core VMM types and state
#pragma once

// ============================================================
// Platform compatibility
// ============================================================
#ifdef _MSC_VER
  #define ARIA_ALIGN(n)   __declspec(align(n))
  #define ARIA_INLINE     __forceinline
#else
  #define ARIA_ALIGN(n)   __attribute__((aligned(n)))
  #define ARIA_INLINE     static inline __attribute__((always_inline))
#endif

// ============================================================
// Type definitions (freestanding — no CRT dependency)
// ============================================================
#ifndef ARIA_TYPES_DEFINED
#define ARIA_TYPES_DEFINED
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef uint64_t            size_t;
typedef uint64_t            uintptr_t;
#ifndef UINT64
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t  UINT8;
typedef int32_t  INT32;
#endif
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
typedef int BOOL;

#define PAGE_SIZE       0x1000ULL
#define PAGE_2MB        0x200000ULL
#define PAGE_1GB        0x40000000ULL
#define PAGE_MASK       (~(PAGE_SIZE - 1))
#define VMX_REGION_SIZE PAGE_SIZE

// MSR/CR intrinsics
extern uint64_t __readmsr(uint32_t msr);
extern void __writemsr(uint32_t msr, uint64_t val);
extern uint64_t __readcr0(void);
extern uint64_t __readcr3(void);
extern uint64_t __readcr4(void);
extern void __writecr0(uint64_t val);
extern void __writecr4(uint64_t val);

typedef struct _SEGMENT_DESC {
    uint16_t selector;
    uint32_t limit;
    uint32_t access_rights;
    uint64_t base;
} SEGMENT_DESC;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nameless struct/union
#endif

typedef union _EPT_ENTRY {
    uint64_t value;
    struct {
        uint64_t read        : 1;
        uint64_t write       : 1;
        uint64_t execute     : 1;
        uint64_t memory_type : 3;
        uint64_t ignore_pat  : 1;
        uint64_t large_page  : 1;
        uint64_t accessed    : 1;
        uint64_t dirty       : 1;
        uint64_t user_exec   : 1;
        uint64_t reserved1   : 1;
        uint64_t pfn         : 40;
        uint64_t reserved2   : 12;
    };
} EPT_ENTRY;

typedef union _EPTP {
    uint64_t value;
    struct {
        uint64_t memory_type : 3;
        uint64_t page_walk   : 3;
        uint64_t dirty_flag  : 1;
        uint64_t reserved1   : 5;
        uint64_t pml4_pfn    : 40;
        uint64_t reserved2   : 12;
    };
} EPTP;

#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef struct _EPT_SPLIT_PAGE {
    uint64_t guest_physical;
    uint64_t clean_physical;
    uint64_t dirty_physical;
    uint32_t active;
    uint32_t reserved;
} EPT_SPLIT_PAGE;

#define MAX_SPLIT_PAGES 16

typedef struct _GUEST_REGS {
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} GUEST_REGS;

// Forward declare NESTED_STATE
struct _NESTED_STATE;

// Per-processor VMM state
typedef struct _VMM_STATE {
    uint64_t    vmxon_region_pa;
    void*       vmxon_region_va;
    uint64_t    vmcs_region_pa;
    void*       vmcs_region_va;

    EPT_ENTRY*  ept_pml4;
    uint64_t    ept_pml4_pa;
    EPTP        eptp;

    EPT_SPLIT_PAGE split_pages[MAX_SPLIT_PAGES];
    uint32_t    split_page_count;

    void*       shared_page_va;
    uint64_t    shared_page_pa;
    void*       draw_buf_va;
    uint64_t    draw_buf_pa;

    uint64_t    tsc_offset;
    uint64_t    last_exit_tsc;

    uint64_t    target_cr3;
    uint64_t    target_base;
    uint32_t    target_pid;

    uint64_t    gworld_addr;

    GUEST_REGS  guest_regs;

    void*       host_stack;
    uint64_t    host_stack_pa;

    void*       msr_bitmap_va;
    uint64_t    msr_bitmap_pa;

    // EPT page-typing totals from the identity-map build. Reported to the guest
    // by hypercall 3 so it can confirm its RAM is actually mapped cacheable
    // rather than taking the hypervisor's word for it.
    uint64_t    ept_wb_pages;
    uint64_t    ept_uc_pages;

    uint64_t    vmm_base_pa;
    uint64_t    vmm_size;

    uint32_t    launched;

    // Nested VMX state (populated by nested engine)
    struct _NESTED_STATE* nested;
} VMM_STATE;

extern VMM_STATE g_vmm;

// Hypercall numbers, passed to VMCALL in RAX. Documented in README.md.
#define ARIA_HC_VERSION      0
#define ARIA_HC_EXIT_COUNT   1
#define ARIA_HC_EXIT_REASON  2
#define ARIA_HC_EPT_TYPING   3
#define ARIA_HC_FINALIZE     4
#define ARIA_HC_MAX          ARIA_HC_FINALIZE

void vmm_handle_exit(GUEST_REGS* regs);

static inline void vmm_memset(void* dst, uint8_t val, size_t len) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < len; i++) p[i] = val;
}

static inline void vmm_memcpy(void* dst, const void* src, size_t len) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
}

static inline int vmm_memcmp(const void* a, const void* b, size_t len) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (size_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

static inline int vmm_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
