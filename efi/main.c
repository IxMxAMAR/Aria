// efi/main.c — UEFI application entry point
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "vmx_init.h"
#include "ept_init.h"
#include "loader.h"
#include "serial.h"
#include "../vmm/vmm.h"
#include "../vmm/vmcs.h"
#include "../vmm/ept.h"
#include "../shared/protocol.h"

// ---------------------------------------------------------------------------
// Nested VMX (Phase 2). The engine under vmm/nested/ is not yet wired into the
// exit dispatcher and depends on vmm/mem_engine.h, which does not exist. Until
// that work lands, it is compiled out so the core hypervisor can build and boot.
// Flip to 1 once handle_nested_vmx is reachable from vmm_handle_exit.
// ---------------------------------------------------------------------------
#define ARIA_ENABLE_NESTED 0

#if ARIA_ENABLE_NESTED
#include "../vmm/nested/nested_vmx.h"
#endif

static UINT64 get_max_physical_address(void) {
    // Use CPUID to get the physical address width — this covers ALL memory
    // including MMIO regions (GPU framebuffer, APIC, IOAPIC, PCI config)
    // that the UEFI memory map doesn't report.
    int info[4];
    extern void __cpuid(int info[4], int leaf);
    __cpuid(info, 0x80000008);
    uint32_t phys_bits = info[0] & 0xFF; // EAX[7:0] = physical address bits
    if (phys_bits == 0 || phys_bits > 52) phys_bits = 36; // safe default

    // Also scan UEFI memory map for actual RAM extent
    UINTN map_size = 0, key, desc_size;
    UINT32 desc_ver;
    EFI_MEMORY_DESCRIPTOR* map = NULL;
    gBS->GetMemoryMap(&map_size, map, &key, &desc_size, &desc_ver);
    map_size += 4096;
    map = AllocatePool(map_size);
    UINT64 max_ram = 4ULL * 1024 * 1024 * 1024; // 4GB minimum
    if (map) {
        EFI_STATUS status = gBS->GetMemoryMap(&map_size, map, &key, &desc_size, &desc_ver);
        if (!EFI_ERROR(status)) {
            UINTN count = map_size / desc_size;
            for (UINTN i = 0; i < count; i++) {
                EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)map + i * desc_size);
                UINT64 end = desc->PhysicalStart + desc->NumberOfPages * 4096;
                if (end > max_ram) max_ram = end;
            }
        }
        FreePool(map);
    }

    // Map the larger of: RAM extent, or 4GB (to cover all MMIO below 4GB).
    // Cap at 512GB to avoid insane EPT table sizes.
    UINT64 max_pa = max_ram;
    if (max_pa < 4ULL * 1024 * 1024 * 1024)
        max_pa = 4ULL * 1024 * 1024 * 1024;
    // Ensure we don't exceed what the CPU supports
    UINT64 cpu_max = 1ULL << phys_bits;
    if (max_pa > cpu_max) max_pa = cpu_max;
    // Cap at 512GB to keep EPT tables reasonable
    if (max_pa > 512ULL * 1024 * 1024 * 1024)
        max_pa = 512ULL * 1024 * 1024 * 1024;

    return max_pa;
}

// ============================================================
// Persistent log file on the boot device (USB drive)
// ============================================================
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>

static EFI_FILE_PROTOCOL* gLogFile = NULL;

static void log_init(EFI_HANDLE ImageHandle) {
    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
    EFI_FILE_PROTOCOL* root = NULL;
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    if (EFI_ERROR(gBS->HandleProtocol(ImageHandle, &lip_guid, (void**)&loaded_image)))
        return;
    if (EFI_ERROR(gBS->HandleProtocol(loaded_image->DeviceHandle, &fs_guid, (void**)&fs)))
        return;
    if (EFI_ERROR(fs->OpenVolume(fs, &root)))
        return;
    // Delete old log, then create fresh
    root->Open(root, &gLogFile, L"aria_log.txt",
               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (gLogFile) {
        // Truncate by setting file size to 0
        EFI_GUID fi_guid = EFI_FILE_INFO_ID;
        UINTN info_size = sizeof(EFI_FILE_INFO) + 64;
        EFI_FILE_INFO* fi = (EFI_FILE_INFO*)AllocatePool(info_size);
        if (fi) {
            if (!EFI_ERROR(gLogFile->GetInfo(gLogFile, &fi_guid, &info_size, fi))) {
                fi->FileSize = 0;
                gLogFile->SetInfo(gLogFile, &fi_guid, info_size, fi);
            }
            FreePool(fi);
        }
    }
}

void log_write(const char* msg) {
    // Write to serial
    serial_print(msg);
    // Write to file
    if (gLogFile) {
        UINTN len = 0;
        const char* p = msg;
        while (*p++) len++;
        gLogFile->Write(gLogFile, &len, (void*)msg);
        gLogFile->Flush(gLogFile);
    }
}

// Log a decimal number to both serial AND file
static void log_dec(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    else {
        char tmp[12]; int ti = 0;
        while (val > 0) { tmp[ti++] = '0' + (char)(val % 10); val /= 10; }
        while (ti > 0) buf[i++] = tmp[--ti];
    }
    buf[i] = '\0';
    log_write(buf);
}

// Log a hex number to both serial AND file
static void log_hex(uint64_t val) {
    char buf[19]; // "0x" + 16 digits + null
    buf[0] = '0'; buf[1] = 'x';
    const char* hex = "0123456789ABCDEF";
    for (int j = 0; j < 16; j++)
        buf[2 + j] = hex[(val >> (60 - j * 4)) & 0xF];
    buf[18] = '\0';
    log_write(buf);
}

static void log_close(void) {
    if (gLogFile) { gLogFile->Close(gLogFile); gLogFile = NULL; }
}

// Screen + serial print helper
static EFI_SYSTEM_TABLE* gST_local;
static void con_print(const CHAR16* msg) {
    if (gST_local && gST_local->ConOut)
        gST_local->ConOut->OutputString(gST_local->ConOut, (CHAR16*)msg);
}
static void step_pause(void) { gBS->Stall(2000000); } // 2 sec

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    gST_local = SystemTable;

    serial_init();
    log_init(ImageHandle);
    log_write("\n\n[Aria] EFI Bootstrap starting...\n");
    con_print(L"[Aria] Initializing hypervisor...\r\n");

    con_print(L"[*] Checking VT-x support...\r\n");
    VMX_STATUS vs = vmx_check_support();
    if (vs != VMX_OK) {
        log_write("[!] VT-x check failed\n");
        con_print(L"[!] VT-x check FAILED\r\n");
        gBS->Stall(10000000); // 10 sec to read
        return EFI_UNSUPPORTED;
    }
    con_print(L"[+] VT-x supported\r\n");

    log_write("[*] Allocating VMM memory...\n");
    con_print(L"[*] Allocating memory...\r\n");

    EFI_PHYSICAL_ADDRESS pa;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa))) {
        log_write("[!] Memory allocation failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    g_vmm.vmxon_region_va = (void*)pa;
    g_vmm.vmxon_region_pa = pa;
    ZeroMem(g_vmm.vmxon_region_va, PAGE_SIZE);

    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa))) {
        log_write("[!] Memory allocation failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    g_vmm.vmcs_region_va = (void*)pa;
    g_vmm.vmcs_region_pa = pa;
    ZeroMem(g_vmm.vmcs_region_va, PAGE_SIZE);

    // Below 4GB: the host identity map built by efi/loader.c covers 0-16GB, and
    // the host stack must be mapped for the exit handler to run at all.
    pa = 0xFFFFFFFFULL;
    if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData, 16, &pa))) {
        log_write("[!] Memory allocation failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    g_vmm.host_stack = (void*)pa;
    g_vmm.host_stack_pa = pa;
    ZeroMem(g_vmm.host_stack, 0x10000);

    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa))) {
        log_write("[!] Memory allocation failed\n");
        return EFI_OUT_OF_RESOURCES;
    }
    g_vmm.msr_bitmap_va = (void*)pa;
    g_vmm.msr_bitmap_pa = pa;
    // MSR bitmap: 0 = passthrough (no exit), 1 = intercept (exit). Only the MSRs
    // that VM entry reloads from the VMCS are intercepted; everything else runs
    // natively. An all-zero bitmap would let a guest write those MSRs and then
    // have the write silently reverted at the next VM entry.
    {
        extern void aria_build_msr_bitmap(void* bitmap);
        aria_build_msr_bitmap(g_vmm.msr_bitmap_va);
    }

    g_vmm.vmm_base_pa = g_vmm.vmxon_region_pa;
    g_vmm.vmm_size = 0x1000000;

#if ARIA_ENABLE_NESTED
    // Allocate nested VMX state
    log_write("[*] Allocating nested VMX state...\n");
    {
        // NESTED_STATE struct (~36KB, need 9 pages)
        UINTN nested_pages = (sizeof(NESTED_STATE) + PAGE_SIZE - 1) / PAGE_SIZE;
        EFI_STATUS ns_status = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, nested_pages, &pa);
        if (EFI_ERROR(ns_status)) {
            log_write("[!] Failed to allocate nested state\n");
            return EFI_OUT_OF_RESOURCES;
        }
        NESTED_STATE* ns = (NESTED_STATE*)pa;
        ZeroMem(ns, nested_pages * PAGE_SIZE);

        // Shadow EPT pool (64MB)
        EFI_PHYSICAL_ADDRESS pool_pa;
        UINTN pool_pages = NESTED_SHADOW_EPT_POOL / PAGE_SIZE;
        EFI_STATUS pool_status = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, pool_pages, &pool_pa);
        if (EFI_ERROR(pool_status)) {
            log_write("[!] Failed to allocate shadow EPT pool\n");
            return EFI_OUT_OF_RESOURCES;
        }
        ZeroMem((void*)pool_pa, NESTED_SHADOW_EPT_POOL);

        g_vmm.nested = ns;
        nested_init(g_vmm.nested);  // zeroes the struct

        // Set pool fields AFTER nested_init (which memsets to zero)
        ns->shadow_ept_pool = (uint8_t*)pool_pa;
        ns->shadow_ept_pool_pa = pool_pa;
        ns->shadow_ept_pool_offset = 0;
        ns->shadow_ept_pool_size = NESTED_SHADOW_EPT_POOL;

        // Set vmcs01_pa so nested engine can switch back to it
        ns->vmcs01_pa = g_vmm.vmcs_region_pa;

        // Allocate VMCS02 (L2 guest VMCS)
        EFI_STATUS s2 = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa);
        if (EFI_ERROR(s2)) { serial_print("[!] VMCS02 alloc failed\n"); return EFI_OUT_OF_RESOURCES; }
        ns->vmcs02_va = (void*)pa;
        ns->vmcs02_pa = pa;
        ZeroMem(ns->vmcs02_va, PAGE_SIZE);

        // Allocate shadow VMCS page
        s2 = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa);
        if (EFI_ERROR(s2)) { serial_print("[!] Shadow VMCS alloc failed\n"); return EFI_OUT_OF_RESOURCES; }
        ns->shadow_vmcs_va = (void*)pa;
        ns->shadow_vmcs_pa = pa;
        ZeroMem(ns->shadow_vmcs_va, PAGE_SIZE);

        // Allocate composed MSR bitmap
        s2 = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa);
        if (EFI_ERROR(s2)) { serial_print("[!] MSR bitmap alloc failed\n"); return EFI_OUT_OF_RESOURCES; }
        ns->composed_msr_bitmap_va = (void*)pa;
        ns->composed_msr_bitmap_pa = pa;
        ZeroMem(ns->composed_msr_bitmap_va, PAGE_SIZE);

        // Allocate VMREAD bitmap (4KB, page-aligned PA)
        s2 = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa);
        if (EFI_ERROR(s2)) { serial_print("[!] VMREAD bitmap alloc failed\n"); return EFI_OUT_OF_RESOURCES; }
        ns->vmread_bitmap_pa = pa;
        ns->vmread_bitmap_va = (void*)pa;
        ZeroMem(ns->vmread_bitmap_va, PAGE_SIZE);

        // Allocate VMWRITE bitmap (4KB, page-aligned PA)
        s2 = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &pa);
        if (EFI_ERROR(s2)) { serial_print("[!] VMWRITE bitmap alloc failed\n"); return EFI_OUT_OF_RESOURCES; }
        ns->vmwrite_bitmap_pa = pa;
        ns->vmwrite_bitmap_va = (void*)pa;
        ZeroMem(ns->vmwrite_bitmap_va, PAGE_SIZE);

        log_write("[+] Nested state at PA ");
        serial_print_hex((uint64_t)pa);
        serial_print(", pool at ");
        serial_print_hex(pool_pa);
        serial_print("\n");
    }

#endif // ARIA_ENABLE_NESTED

    con_print(L"[+] Memory allocated\r\n");

    UINT64 max_phys = get_max_physical_address(); (void)max_phys;
    con_print(L"[*] Building EPT (1GB+MTRR)...\r\n");
    extern int ept_build_identity_map_minimal(VMM_STATE*);
    if (ept_build_identity_map_minimal(&g_vmm) != 0) {
        log_write("[!] EPT build failed\n");
        con_print(L"[!] EPT build FAILED\r\n");
        gBS->Stall(10000000);
        return EFI_OUT_OF_RESOURCES;
    }
    con_print(L"[+] EPT built\r\n");

    if (ept_setup_shared_page(&g_vmm) != 0) {
        log_write("[!] Shared page setup failed\n");
        con_print(L"[!] Shared page FAILED\r\n");
        gBS->Stall(10000000);
        return EFI_OUT_OF_RESOURCES;
    }

    con_print(L"[*] Enabling VMX (VMXON)...\r\n");
    vs = vmx_enable(&g_vmm);
    if (vs != VMX_OK) {
        log_write("[!] VMX enable failed\n");
        con_print(L"[!] VMXON FAILED\r\n");
        gBS->Stall(10000000);
        return EFI_DEVICE_ERROR;
    }
    con_print(L"[+] VMXON succeeded\r\n");

    con_print(L"[*] Configuring VMCS...\r\n");
    vs = vmx_setup_vmcs(&g_vmm);
    if (vs != VMX_OK) {
        log_write("[!] VMCS setup failed\n");
        con_print(L"[!] VMCS setup FAILED\r\n");
        gBS->Stall(10000000);
        return EFI_DEVICE_ERROR;
    }
    con_print(L"[+] VMCS configured\r\n");

    // Hide VMM memory from guest EPT BEFORE vmx_launch.
    // SKIP EPT hiding for now — test pure identity map first.
    // Re-enable once we confirm EPT + exit handler works.
    log_write("[*] EPT hiding SKIPPED (debug)\n");

    // Compact VMCS dump — fits on one screen
    {
        CHAR16 hb[20];
        #define H(val) { \
            uint64_t _v=(uint64_t)(val); \
            hb[0]=L'0';hb[1]=L'x'; \
            for(int _i=0;_i<16;_i++) hb[17-_i]=L"0123456789ABCDEF"[(_v>>(_i*4))&0xF]; \
            hb[18]=L'\0'; con_print(hb); }
        // Two values per line to save space
        #define R2(a,av,b,bv) { con_print(L"" a L"=");H(av);con_print(L" " b L"=");H(bv);con_print(L"\r\n"); }
        #define R1(a,av) { con_print(L"" a L"=");H(av);con_print(L"\r\n"); }

        uint32_t ec = vmcs_read32(VMCS_VM_EXIT_CTRLS);
        uint32_t nc = vmcs_read32(VMCS_VM_ENTRY_CTRLS);
        extern uint64_t __readmsr(uint32_t);

        con_print(L"\r\n=== HOST STATE ===\r\n");
        // Raw TRUE_EXIT MSR: low=must1, high=may1
        uint64_t raw_ex = __readmsr(0x48F);
        R2(L"MUST1",(uint64_t)(uint32_t)(raw_ex&0xFFFFFFFF), L"MAY1",(uint64_t)(uint32_t)(raw_ex>>32))
        R2(L"EC",ec, L"NC",nc)
        con_print(L"EC: ");
        if (ec&(1u<<9))  con_print(L"Addr ");
        if (ec&(1u<<12)) con_print(L"PERF ");
        if (ec&(1u<<19)) con_print(L"PAT ");
        if (ec&(1u<<21)) con_print(L"EFER ");
        if (ec&(1u<<28)) con_print(L"CET ");
        if (ec&(1u<<29)) con_print(L"PKRS ");
        con_print(L"\r\n");
        R2(L"CR0",vmcs_read64(VMCS_HOST_CR0), L"CR3",vmcs_read64(VMCS_HOST_CR3))
        R2(L"CR4",vmcs_read64(VMCS_HOST_CR4), L"EFER",vmcs_read64(VMCS_HOST_IA32_EFER))
        R2(L"CS",(uint64_t)vmcs_read16(VMCS_HOST_CS_SEL), L"SS",(uint64_t)vmcs_read16(VMCS_HOST_SS_SEL))
        R2(L"TR",(uint64_t)vmcs_read16(VMCS_HOST_TR_SEL), L"DS",(uint64_t)vmcs_read16(VMCS_HOST_DS_SEL))
        R2(L"FS",(uint64_t)vmcs_read16(VMCS_HOST_FS_SEL), L"GS",(uint64_t)vmcs_read16(VMCS_HOST_GS_SEL))
        R2(L"RIP",vmcs_read64(VMCS_HOST_RIP), L"RSP",vmcs_read64(VMCS_HOST_RSP))
        R2(L"TR_B",vmcs_read64(VMCS_HOST_TR_BASE), L"GDT",vmcs_read64(VMCS_HOST_GDTR_BASE))
        R2(L"IDT",vmcs_read64(VMCS_HOST_IDTR_BASE), L"FS_B",vmcs_read64(VMCS_HOST_FS_BASE))
        R2(L"GS_B",vmcs_read64(VMCS_HOST_GS_BASE), L"SESP",vmcs_read64(VMCS_HOST_SYSENTER_ESP))
        R1(L"SEIP",vmcs_read64(VMCS_HOST_SYSENTER_EIP))
        if (ec&(1u<<12)) { R1(L"PERF",vmcs_read64(VMCS_HOST_IA32_PERF_GLOBAL)) }
        if (ec&(1u<<19)) { R1(L"PAT",vmcs_read64(VMCS_HOST_IA32_PAT)) }
        R1(L"EPTP",vmcs_read64(VMCS_EPT_POINTER))
        R1(L"EPTCAP",__readmsr(0x48C))

        // Show CR0/CR4 FIXED0 MSRs — reveals CPU-required bits
        extern uint64_t __readmsr(uint32_t);
        R2(L"CR0F0",__readmsr(0x486), L"CR4F0",__readmsr(0x488))
        // Check FIXED0 satisfaction
        uint64_t cr0v=vmcs_read64(VMCS_HOST_CR0), cr4h=vmcs_read64(VMCS_HOST_CR4);
        uint64_t cr0f0=__readmsr(0x486), cr4f0=__readmsr(0x488);
        if ((cr0v&cr0f0)!=cr0f0) { con_print(L"!! CR0 MISS="); H(cr0f0&~cr0v); con_print(L"\r\n"); }
        if ((cr4h&cr4f0)!=cr4f0) { con_print(L"!! CR4 MISS="); H(cr4f0&~cr4h); con_print(L"\r\n"); }
        // Other checks
        uint16_t cs_s=vmcs_read16(VMCS_HOST_CS_SEL), tr_s=vmcs_read16(VMCS_HOST_TR_SEL);
        uint64_t efer=vmcs_read64(VMCS_HOST_IA32_EFER);
        if (!cs_s) con_print(L"!! CS=0\r\n");
        if (!tr_s) con_print(L"!! TR=0\r\n");
        if (cr4h&(1ULL<<23)) con_print(L"!! CET\r\n");
        if ((ec&(1u<<9)) && !(efer&(1ULL<<10))) con_print(L"!! LMA\r\n");

        // VMWRITE failure check
        {
            extern volatile uint32_t vmcs_write_fail_count;
            extern volatile uint32_t vmcs_write_fail_field;
            if (vmcs_write_fail_count > 0) {
                con_print(L"!! VMWR FAIL=");
                CHAR16 nb[12]; int ni=0; uint32_t nv=vmcs_write_fail_count;
                if(!nv){nb[ni++]=L'0';}else{CHAR16 t[10];int ti=0;while(nv){t[ti++]=L'0'+(CHAR16)(nv%10);nv/=10;}while(ti>0)nb[ni++]=t[--ti];}
                nb[ni]=0; con_print(nb);
                con_print(L" fld="); H((uint64_t)vmcs_write_fail_field);
                con_print(L"\r\n");
            } else {
                con_print(L"[+] All VMWRITE OK\r\n");
            }
        }
        con_print(L"==================\r\n");

        #undef H
        #undef R2
        #undef R1
    }

    // The EPT split pool must be in memory the OS preserves: the page tables it
    // hands out are referenced by the EPT, and a static array would sit in the
    // image BSS, which the guest frees at ExitBootServices.
    {
        EFI_PHYSICAL_ADDRESS pool_pa = 0xFFFFFFFFULL;
        if (!EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData,
                                         0x400, &pool_pa))) {
            ept_set_pool((void*)(UINTN)pool_pa, 0x400000);
        } else {
            log_write("[!] EPT split pool allocation failed\n");
        }
    }

    // Relocate the hypervisor into runtime-preserved memory, and arrange to move
    // the VMCS host state there just before the guest calls ExitBootServices.
    // Without this the exit stub lives in EfiLoaderCode, which the OS frees.
    if (EFI_ERROR(aria_loader_prepare())) {
        log_write("[!] loader prepare failed - will not survive handoff\n");
    }
    aria_loader_hook_exit_boot_services();

    // Protect the hypervisor own memory from the guest. These pages are all
    // runtime-preserved, so a well-behaved OS never touches them; if something
    // does, it takes an EPT violation and the panic dump names the address.
    // Note this is integrity, not concealment: the guest gets a fault, and the
    // fault is reported rather than hidden.
    if (g_vmm.vmxon_region_pa)
        ept_hide_range(&g_vmm, g_vmm.vmxon_region_pa, PAGE_SIZE);
    if (g_vmm.vmcs_region_pa)
        ept_hide_range(&g_vmm, g_vmm.vmcs_region_pa, PAGE_SIZE);
    if (g_vmm.msr_bitmap_pa)
        ept_hide_range(&g_vmm, g_vmm.msr_bitmap_pa, PAGE_SIZE);
    if (g_vmm.host_stack_pa)
        ept_hide_range(&g_vmm, g_vmm.host_stack_pa, 0x10000);
    if (g_loader.image_new_base)
        ept_hide_range(&g_vmm, g_loader.image_new_base, g_loader.image_size);
    if (g_loader.host_cr3_pa)
        ept_hide_range(&g_vmm, g_loader.host_cr3_pa, PAGE_SIZE);
    if (g_loader.host_gdt_base)
        ept_hide_range(&g_vmm, g_loader.host_gdt_base, PAGE_SIZE);
    if (g_loader.host_idt_base)
        ept_hide_range(&g_vmm, g_loader.host_idt_base, PAGE_SIZE);
    log_write("[+] hypervisor memory protected in the EPT\n");

    // Log right before VMLAUNCH so we know we got here
    log_write("[*] About to call vmx_launch\n");
    con_print(L"[*] VMLAUNCH in 2s\r\n");
    gBS->Stall(2000000);
    log_write("[*] >>> VMLAUNCH NOW <<<\n");
    con_print(L">>> NOW <<<\r\n");
    vs = vmx_launch(&g_vmm);

    // After VMLAUNCH, we're in guest mode with ALL-UC EPT.
    // UEFI services (USB, GOP) use DMA that needs WB memory.
    // With UC, any UEFI call (log_write, con_print, Stall) hangs.
    // Just return immediately — let UEFI boot manager load Windows.
    // If we reach here, vmx_do_launch returned (either success or fail)
    con_print(L"<<< BACK <<<\r\n");
    if (vs != VMX_OK) {
        log_write("[!] VMLAUNCH failed\n");
        // Display error code on screen
        uint32_t vm_err = g_vmm.target_pid; // stashed by vmx_launch
        CHAR16 err_msg[64];
        err_msg[0] = L'E'; err_msg[1] = L'r'; err_msg[2] = L'r';
        err_msg[3] = L'o'; err_msg[4] = L'r'; err_msg[5] = L' ';
        err_msg[6] = L'c'; err_msg[7] = L'o'; err_msg[8] = L'd';
        err_msg[9] = L'e'; err_msg[10] = L':'; err_msg[11] = L' ';
        // Convert error number to string
        int pos = 11;
        if (vm_err == 0) { err_msg[++pos] = L'0'; }
        else {
            CHAR16 tmp[16]; int ti = 0;
            uint32_t v = vm_err;
            while (v > 0) { tmp[ti++] = L'0' + (CHAR16)(v % 10); v /= 10; }
            while (ti > 0) { err_msg[++pos] = tmp[--ti]; }
        }
        err_msg[++pos] = L'\r'; err_msg[++pos] = L'\n'; err_msg[++pos] = L'\0';
        con_print(L"[!] VMLAUNCH FAILED\r\n");
        con_print(err_msg);
        // SDM Vol 3C Table 30-1. These are the codes a VMLAUNCH can return.
        // (Codes 7 and 8 previously carried meanings that belong to other
        // numbers entirely, and 28/29 — the two most likely launch failures —
        // were missing, which sent the reader after the wrong fault.)
        if      (vm_err == 7)  con_print(L"  = VMREAD/VMWRITE from/to unsupported VMCS component\r\n");
        else if (vm_err == 8)  con_print(L"  = VMWRITE to read-only VMCS component\r\n");
        else if (vm_err == 26) con_print(L"  = VMRESUME with non-launched VMCS\r\n");
        else if (vm_err == 28) con_print(L"  = VM entry with invalid control field(s)\r\n");
        else if (vm_err == 29) con_print(L"  = VM entry with invalid host-state field(s)\r\n");
        else if (vm_err == 33) con_print(L"  = VM entry with invalid guest-state field(s)\r\n");
        else if (vm_err == 34) con_print(L"  = VM entry failure due to MSR loading\r\n");
        else con_print(L"  (see Intel SDM Vol 3C Table 30-1)\r\n");
        log_close();
        gBS->Stall(30000000); // 30 seconds to read
        return EFI_DEVICE_ERROR;
    }

    g_loader.active = TRUE;

    // Hypervisor is active and the guest is running. The EPT is a full identity
    // map typed from the MTRRs, so RAM is cacheable (the earlier all-uncached
    // mapping was a bug; see docs/STATUS.md).
    // Return immediately — UEFI boot manager will boot Windows.
    // The hypervisor runs underneath, intercepting CPUID/CR/XSETBV.
    return EFI_SUCCESS;
}
