# Aria â€” implementation status

Per-subsystem status, kept honest on purpose. The vocabulary is deliberate:

- **works** â€” verified by a real build and a real run on hardware
- **implemented** â€” the code exists and is reachable from the entry point
- **partial** â€” some of it exists; the gaps are listed
- **not implemented** â€” the code does not exist, or exists but is unreachable
- **excluded** â€” source is present but deliberately not built

If a row says "implemented" and you find it does not work, that is a bug in this
document as much as in the code. Please fix the document.

## Core

| Subsystem | State | Notes |
|---|---|---|
| UEFI application entry + bring-up | implemented | `efi/main.c` |
| VT-x capability check | partial | Checks `CPUID.1:ECX[5]` and `IA32_FEATURE_CONTROL` bit 2 only; never tests bit 1 (VMX outside SMX) |
| VMXON / VMCLEAR / VMPTRLD | implemented | Statuses checked (`efi/vmx_init.c`) |
| VMCS host-state population | implemented | Single processor |
| VMCS guest-state population | implemented | Guest state is the live CPU state (in-place design) |
| VMCS control fields | implemented | See "Control-field caveats" below |
| VM-entry / VM-exit | implemented | `vmx_do_launch` in `vmm/asm/vmx_asm.asm` |
| EPT construction | partial | Full identity map, 0â€“512 GB, 2 MB pages below 4 GB and 1 GB pages above |
| EPT memory typing | partial | MTRR-derived; see caveats |
| EPT hypervisor-memory protection | **not implemented** | Guest can read/write all VMM memory â€” see Security |
| Exit dispatcher | implemented | `vmm/vmm.c` |
| Exit dispatch table | not implemented | A `switch`, not a table |
| Unhandled-exit behaviour | implemented | Reports and stops (`ARIA_STRICT_EXITS`) |

## Interception

| Subsystem | State | Notes |
|---|---|---|
| CPUID interception | implemented | Announces Aria; VMX bit reported absent |
| CR access interception | partial | CR4 only (`vmm/exit_handlers/cr_access.c`) |
| MSR interception | **not implemented** | MSR bitmap is all zeros, so no MSR access exits |
| I/O port interception | not implemented | Unconditional I/O exiting is off; guest has native port I/O |
| Exception interception | not implemented | Exception bitmap is 0 |
| Interrupt / NMI interception | not implemented | Disabled in pin-based controls by design |
| XSETBV interception | implemented | XCR index validated before forwarding |
| TSC offsetting / scaling | not implemented | `TSC_OFFSET` written as 0 |

## Control-field caveats

These are the ones that have already caused bugs, or are known to be wrong:

- **Entry controls load `IA32_EFER` (bit 15), not `IA32_PAT` (bit 14).** These
  were previously swapped, which made `VMCS_GUEST_IA32_EFER` inert and silently
  reverted every guest `WRMSR` to `IA32_EFER` on the next exit/entry cycle.
- **PAT is neither saved nor loaded** on exit or entry, so it passes through
  untouched. `VMCS_GUEST_IA32_PAT` is therefore inert; it holds the live value so
  it cannot drift if PAT loading is enabled later.
- **`CR0_GUEST_HOST_MASK` is 0**, so guest `CR0` accesses do not exit.
  `CR4_GUEST_HOST_MASK` is `CR4_VMXE` only.
- **`CR4_READ_SHADOW` hides `VMXE`** from the guest. That bit belongs to the host.
- **Debug controls are not saved or loaded**, so `VMCS_GUEST_DR7` and
  `VMCS_GUEST_IA32_DEBUGCTL` are inert and the guest's values leak to the host.
- **No MSR load/store lists** are configured.

## Not implemented

Ordered roughly by how much they block a usable hypervisor.

| Gap | Impact |
|---|---|
| **SMP / AP bring-up** | Only the boot processor is virtualized. Every other core runs the guest natively, un-intercepted, with access to VMM memory. There is no per-CPU state, no `INIT`/`SIPI` handling, no IPI coordination. |
| **Host lifetime management** | No `ExitBootServices` hook. `HOST_CR3` is the firmware's CR3 and `HOST_RIP` points into the application's own image, which is `EfiLoaderCode`/`EfiLoaderData` â€” memory the OS is free to reuse. The first VM exit after the OS takes over memory can fault in host context. |
| **Nested VMX** | See below. |
| **Interrupt / APIC virtualization** | No virtual interrupt delivery, no TPR shadow, no posted interrupts, no APIC-access page. |
| **Timers / preemption** | No VMX-preemption timer, no interrupt-window exiting. A guest that masks interrupts and spins is never preempted. |
| **Hypervisor memory protection** | VMM pages are inside the guest's identity map, writable. |
| **IOMMU / VT-d** | No DMA remapping; devices DMA against host physical addresses, bypassing the EPT. |
| **Secure Boot signing** | `mok/` holds a self-signed public certificate. There is no private key in the repository and no signing step in any script. MOK is a shim concept and cannot authorize a firmware-launched UEFI application, so the current path is incomplete. |
| **Testing** | No unit tests. CI builds only. |

## Nested VMX (excluded)

`vmm/nested/` is a partial nested-VMX engine â€” shadow VMCS, shadow EPT, VMX
instruction emulation, L2 exit reflection â€” totalling roughly 2,800 lines. It is
**not built and not reachable**:

- `handle_nested_vmx` was `static` with no call sites; it has been removed from
  `vmm/vmm.c`. The engine is compiled out via `ARIA_ENABLE_NESTED` (0) in
  `efi/main.c` and is absent from `efi/AriaEfi.inf`.
- Six of its translation units include `vmm/mem_engine.h`, which does not exist.
- It depends on VMCS fields absent from its merge table, most importantly the
  VM-entry interruption-information fields, without which L2 can never receive an
  interrupt.
- `exit_handlers/cpuid.c` and `exit_handlers/ept_violation.c` are also excluded:
  they are unreachable duplicates of the live handlers in `vmm/vmm.c` and include
  `vmm/channel.h`, which does not exist.

Re-enabling nested support means: writing `vmm/mem_engine.h`, wiring the
dispatcher into `vmm_handle_exit`, adding the missing merge-table fields, adding
guest register context switching, tracking VMCS launch state, and re-enabling the
VMX CPUID bit. It is a Phase 2 project, not a flag flip.

## Security

Aria is not a security boundary, and as it stands it is not even a reliable
isolation layer. Read this before running it on hardware you care about.

- **The guest can read and write hypervisor memory.** The EPT is a full identity
  map with read, write and execute set on every entry, and hypervisor-memory
  hiding is not implemented. The guest can therefore rewrite the VMCS region, the
  EPT tables, the MSR bitmap, the host stack and the exit stub.
- **The host uses the guest's IDT and GDT.** `HOST_IDTR_BASE` and `HOST_GDTR_BASE`
  are captured from the live machine, and no private tables exist. Any host-side
  fault vectors through memory the guest owns.
- **No DMA isolation.** No IOMMU, and the guest has native port I/O.
- **Unvalidated guest state could halt the host.** Guest `CR4` writes are now
  masked against `CR4_FIXED0`/`CR4_FIXED1`, which removes one path to a failed VM
  entry; the general class of problem remains.

## Build

**Verified.** A clean rebuild (build output deleted, then rebuilt) completes with
zero errors and produces:

```
AriaEfi.efi   26,624 bytes
PE32+, machine 0x8664, subsystem 10 (EFI Application), entry RVA 0x1010
SHA-256  2C80659EE8FA60B6FADC0E714CD45DB5818BF17794D03199CCDD522DAB865376
```

on Visual Studio 2026 (18.3) with MSVC 14.50.35717, against EDK2
`edk2-stable202411`, with NASM 2.16.03. Repeating the build produces a
byte-identical image.

| Item | State |
|---|---|
| EDK2 package layout | `AriaPkg` exposed to the EDK2 workspace by a directory junction; `AriaPkg.dsc` has a `[Packages]` section |
| Toolchain tag | EDK2 `VS2022`. EDK2's own `vswhere` detection stops below VS 2022, so `scripts/build_efi.bat` locates Visual Studio itself and sets `VS2022_PREFIX` to the newest MSVC toolset |
| Python build tools | Installed from PyPI (`pip install edk2-basetools`) |
| `GenFw` | **Cannot be compiled from source** on MSVC 14.4x+: EDK2's BaseTools sources trip newer warnings (`C4311`/`C4312`/`C4267`, promoted by `/WX`), and `MdePkg/IndustryStandard/PeImage.h` redefines `RUNTIME_FUNCTION` against the current Windows SDK. `scripts/build_efi.bat` fetches the prebuilt binaries tianocore publishes in `edk2-BaseTools-win32` instead. |
| NASM | Mandatory â€” `MdePkg/BaseLib` has a `CpuId.nasm` even though all of Aria's own assembly is MASM |
| Invoking `build` | Must be called with `call`. EDK2 puts a `build.bat` wrapper on the path, and calling a batch file from a batch file without `call` transfers control permanently, silently truncating the calling script. |
| CI | `.github/workflows/build.yml` â€” builds on `windows-latest` |
| Debug output | Always on, including in RELEASE builds; `serial_print`/`log_write` are called directly rather than through the `DBG_*` macros |

### Not verified

**Aria has never been run.** Every claim above is build-level only. No part of
this document reflects an actual `VMLAUNCH` on hardware.

## Code audit

The VMCS layer was audited against the Intel SDM rather than assumed correct.

### Verified correct

- All VMCS field encodings in `vmm/vmcs.h` (~100 constants) match the SDM
  scheme: width in bits 14:13, access type in 11:10, index in 9:1. Includes the
  easy-to-miss ones — `VMCS_LINK_PTR` at 0x2800 as 64-bit *guest* state,
  `GUEST_PHYS_ADDR` at 0x2400, the reserved gap at 0x482C correctly skipped
  before `PREEMPT_TIMER` at 0x482E, and the CET fields at 0x6828/0x6C18.
- Exit-reason constants 0-64 match the SDM.
- `read_segment_descriptor()` handles null selectors and not-present
  descriptors: both yield access rights 0x10000 (unusable), which is what SDM
  26.3.1.2 requires. Without it, UEFI's common P=0 DS/ES/FS/GS would fail VM
  entry with error 33. The extraction `(raw >> 40) & 0xF0FF` matches the VMCS
  access-rights layout exactly.
- `vmx_adjust_controls()` uses the CR0_FIXED0/FIXED1 convention
  `(desired | must_be_1) & may_be_1`, which is correct.
- The control set is coherent: proc-based bit 28 (use MSR bitmaps) is enabled
  and `VMCS_MSR_BITMAP` is programmed; the bitmap is all zeros, so MSRs pass
  through and no MSR exits occur. Secondary controls enable EPT and VPID.
  Pin-based requests no external-interrupt or NMI exiting, so the guest keeps
  its own interrupts.
- `VMCS_VMCS_LINK_PTR` is 0xFFFFFFFFFFFFFFFF, required when VMCS shadowing is
  off.

### Fixed in this pass

- `vmcs_validate_host_state()` was dead code — 95 lines of SDM 26.2 host-state
  checks that nothing called. It now runs at the top of `vmx_launch()`, so a
  host-state violation prints a specific message before the launch attempt. It
  is deliberately report-only: the build has not been run on hardware, and a
  validator false positive must not be able to block a launch that would
  otherwise work.
- The VM-instruction error table printed on launch failure mislabelled codes 7
  and 8 (they are VMREAD/VMWRITE errors, not control/host-state errors),
  invented a meaning for 9, and omitted 28 and 29 — the two codes a broken
  VMLAUNCH actually returns. It now matches SDM Vol 3C Table 30-1.

### Not audited

`ept_init.c` beyond the identity-map construction and MTRR typing, and the
assembly beyond the register-save order against `GUEST_REGS`.
### Fixed after the audit: EPT memory typing

`mtrr_is_wb()` returned UC for any address not covered by a variable MTRR. On
real firmware `IA32_MTRR_DEF_TYPE` is normally WB and system RAM is deliberately
left uncovered — the variable registers exist to mark MMIO holes UC. So the
whitelist marked every byte of RAM uncached. That is why `main.c` describes the
EPT as "ALL-UC", and why UEFI services (USB, GOP) hung: their DMA buffers were
mapped uncached.

Uncovered addresses now take the MTRR default type, which is what SDM Vol 3
11.11.2.1 specifies. The builder also counts the resulting page types and prints
them, so a hardware run can confirm the fix instead of assuming it:

    [+] EPT typing: N WB pages, M UC pages

A WB count of 0 means the typing is still wrong.

Residual risk: a 1GB page that spans an MMIO hole lying strictly inside it is
typed from its two endpoints. Firmware normally covers MMIO with UC variable
MTRRs, which the endpoint checks do catch, but this is not a general
MTRR-overlap resolution.
## Features

### MSR interception

The MSR bitmap (`vmm/msr_bitmap.c`) intercepts writes to the eight MSRs whose
values VM entry reloads from the VMCS:

    IA32_SYSENTER_CS / ESP / EIP, IA32_DEBUGCTL, IA32_PAT,
    IA32_EFER, IA32_FS_BASE, IA32_GS_BASE

Without interception a guest `WRMSR` to any of these succeeds on the hardware and
is then silently reverted at the next VM entry, because the CPU reloads the MSR
from a VMCS field that was never updated. Reads are deliberately not intercepted:
they return the real MSR, which the handler keeps identical to the VMCS, so the
fast path stays exit-free.

`handle_msr_write()` validates before touching hardware. It runs in host context,
so a value that would raise #GP on WRMSR would fault the hypervisor rather than
the guest; a guest attempt to change EFER.LMA or set a reserved EFER bit is
reflected back as a guest #GP instead.

Known gap: `swapgs` changes GS base without an MSR write and is not
interceptable. Whether VM exit independently saves the guest GS base is **not
verified here**. If it does not, a guest that relies on `swapgs` would need the
GS base captured and restored around each exit, and that needs hardware testing
to establish.

### Hypercall ABI

The guest issues `VMCALL` with the call number in RAX; results come back in
registers. The interface is discoverable rather than hardcoded — CPUID leaf
0x40000001 reports the ABI version, a feature bitmap, and the highest
implemented call.

    leaf 0x40000000  vendor "AriaAriaAria", EAX = highest leaf (0x40000002)
    leaf 0x40000001  ABI version (EAX), feature flags (EBX), max call (ECX)
    leaf 0x40000002  live VM-exit counter (EDX:EAX)

    call 0  VERSION      RBX = ABI version, RCX = "Aria"
    call 1  EXIT_COUNT   RBX = total VM exits since launch
    call 2  EXIT_REASON  in RCX = reason; RBX = count for that reason
    call 3  EPT_TYPING   RBX = WB pages, RCX = UC pages

An unknown call returns RAX = 1.

This is the point of the interface: a guest can confirm the hypervisor is really
there, how many exits it has taken, and whether its RAM is mapped cacheable —
rather than trusting the hypervisor'"'"'s own log.

### Exit statistics

`aria_exit_reason_counts[65]` counts exits per reason. `aria_exit_log[16]` keeps
the first sixteen in order, so a crash shows the sequence that led to it. Both
are readable from the guest through hypercall 2.
### Surviving ExitBootServices — implemented, unverified

`efi/loader.c` keeps a copy of the whole image in `EfiRuntimeServicesCode` and
gives the host its own state in `EfiRuntimeServicesData`:

    image copy   EfiRuntimeServicesCode, allocated below 4GB
    host CR3     identity map, 0-16GB, 2MB pages
    host GDT     flat 64-bit code/data plus a TSS descriptor
    host TSS     104-byte, RSP0 = top of the host stack
    host IDT     every vector -> host_fault_halt

No relocation engine is needed. The image is position independent, which was
verified rather than assumed: there is no `.reloc` section and no `BASERELOC`
data directory, `ImageBase` is 0, and a scan of `.data` found no absolute
pointers into the image — the four 8-byte values that initially looked like
pointers sit inside ASCII string literals.

The handoff goes through a hypercall because the hook executes as the guest and
cannot `VMWRITE`. `gBS->ExitBootServices` is wrapped; the wrapper issues
`VMCALL(ARIA_HC_FINALIZE)`, which exits to root, where `aria_loader_finalize()`
copies the live image (including current `.data`/`.bss`) into the runtime buffer
and repoints `HOST_RIP`, `HOST_CR3`, `HOST_GDTR_BASE`, `HOST_IDTR_BASE`,
`HOST_TR_SEL` and `HOST_TR_BASE`. The next VM exit enters the relocated stub.

The exit stub now executes `cli` on entry so the handler cannot be interrupted in
host context; VM entry restores the guest `RFLAGS` from `GUEST_RFLAGS`, so the
guest `IF` is unaffected.

The host stack is allocated below 4GB, because the host identity map covers
0-16GB and the exit handler runs on that stack.

**Unverified.** None of this has run. A hardware run needs to confirm three
things: that the image really is position independent at a nonzero base; that the
host CR3 and GDT are accepted on the first exit after the handoff; and that no
relocated path touches `gBS`/`gST`, which are freed at that point.
### TSC offsetting — implemented, unverified

The processor-based control "use TSC offsetting" (bit 3) is now enabled and
`vmm_handle_exit` charges the time it spends in root context to the hypervisor:
`VMCS_TSC_OFFSET` accumulates `-(tsc_out - tsc_in)`, so the guest's `RDTSC` does
not advance while the guest is not running. The accumulated value is readable
through hypercall 5 (`TSC_OFFSET`).

`vmx_adjust_controls` masks bit 3 off if `IA32_VMX_PROCBASED_CTLS` does not
allow it, in which case the offset writes are harmless no-ops.

### Readable panics

The four fatal paths (triple fault, EPT violation, EPT misconfiguration,
unhandled exit) previously logged detail to serial and only a short string to
VGA. A desktop usually has no serial port, so they now dump exit reason,
qualification, guest `RIP`, `RSP`, `GPA` and the exit count to the VGA text
buffer as well, and halt. This is the intended way to find out what went wrong on
a first hardware run.
### EPT protection of hypervisor memory — implemented, unverified

`ept_hide_range` clears read/write/execute on the 4KB EPT entries covering the
hypervisor's own pages, so the guest faults on any access instead of being able to
read or overwrite the hypervisor. This is integrity, not concealment: the access
takes an EPT violation and the panic dump names the GPA.

Protected: VMXON region, VMCS region, MSR bitmap, host stack, the relocated image,
and the loader's host CR3/GDT/IDT pages.

Two bugs had to be fixed first, both of which would only have surfaced on
hardware:

- The split pool was a 4MB static array in `vmm/ept.c`, that is, in the image's
  BSS. Splitting a 2MB page allocated the new page table from there, so the EPT
  would have pointed at memory the guest frees at `ExitBootServices`. It is now
  allocated as `EfiRuntimeServicesData` and handed to `ept_set_pool()`.
- `ept_split_2mb_to_4kb` hardcoded memory type WB for all 512 entries instead of
  preserving the parent's, so splitting a single page inside an uncached region
  would have silently made the whole region cacheable.

Deliberately not protected: the EPT's own page tables. `ept_walk` uses an entry's
read bit as its present test, so hiding the GPAs that hold the tables interferes
with the walk that would have to do the hiding. Remaining work.

Splitting also makes `ept_wb_pages`/`ept_uc_pages` (hypercall 3) stale for the
split regions, because those counts are accumulated over 2MB pages at build time.

### AP virtualisation — dependency found, not started

Item 2 has a real ordering problem that is not obvious from the roadmap. An AP's
VMCS host state must point at the relocated image, because that is the only copy
that survives the handoff. But the relocated image is not valid until
`aria_loader_finalize()` copies it, which happens at `ExitBootServices`. An AP
launched before that would jump to uninitialised memory on its first exit, and an
AP cannot fix its own VMCS afterwards because the only correct host state is the
one that is already too late to install.

The fix is to snapshot the image once at the end of setup as well as at the
handoff, so the relocated stub is valid from launch time onward, and to apply the
host state per-VMCS at AP launch instead of only to the BSP at handoff. That is a
prerequisite refactor in `efi/loader.c` and has not been done.
### EPT capability validation — implemented

`ept_build_identity_map_minimal` now reads `IA32_VMX_EPT_VPID_CAP` (MSR 0x48C)
before building and refuses with a named reason if the CPU cannot do 4-level EPT
walks (bit 6), WB memory type (bit 14) or 2MB pages (bit 16). Previously the
capability MSR was never read at all, so an unsupported configuration surfaced as
an opaque VM-entry failure.

The 4-512GB window is mapped with 1GB pages, which needs bit 17. If that bit is
clear those entries are now omitted with a loud message instead of being emitted
as an EPT misconfiguration, and memory above 4GB is left unmapped. That is a
degraded state, not a good one, but it is honest and diagnosable; every CPU with
EPT in practice supports 1GB pages.