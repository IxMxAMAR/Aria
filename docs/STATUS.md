# Aria — implementation status

Per-subsystem status, kept honest on purpose. The vocabulary is deliberate:

- **works** — verified by a real build and a real run on hardware
- **implemented** — the code exists and is reachable from the entry point
- **partial** — some of it exists; the gaps are listed
- **not implemented** — the code does not exist, or exists but is unreachable
- **excluded** — source is present but deliberately not built

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
| EPT construction | partial | Full identity map, 0–512 GB, 2 MB pages below 4 GB and 1 GB pages above |
| EPT memory typing | partial | MTRR-derived; see caveats |
| EPT hypervisor-memory protection | **not implemented** | Guest can read/write all VMM memory — see Security |
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
| **Host lifetime management** | No `ExitBootServices` hook. `HOST_CR3` is the firmware's CR3 and `HOST_RIP` points into the application's own image, which is `EfiLoaderCode`/`EfiLoaderData` — memory the OS is free to reuse. The first VM exit after the OS takes over memory can fault in host context. |
| **Nested VMX** | See below. |
| **Interrupt / APIC virtualization** | No virtual interrupt delivery, no TPR shadow, no posted interrupts, no APIC-access page. |
| **Timers / preemption** | No VMX-preemption timer, no interrupt-window exiting. A guest that masks interrupts and spins is never preempted. |
| **Hypervisor memory protection** | VMM pages are inside the guest's identity map, writable. |
| **IOMMU / VT-d** | No DMA remapping; devices DMA against host physical addresses, bypassing the EPT. |
| **Secure Boot signing** | `mok/` holds a self-signed public certificate. There is no private key in the repository and no signing step in any script. MOK is a shim concept and cannot authorize a firmware-launched UEFI application, so the current path is incomplete. |
| **Testing** | No unit tests. CI builds only. |

## Nested VMX (excluded)

`vmm/nested/` is a partial nested-VMX engine — shadow VMCS, shadow EPT, VMX
instruction emulation, L2 exit reflection — totalling roughly 2,800 lines. It is
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
| NASM | Mandatory — `MdePkg/BaseLib` has a `CpuId.nasm` even though all of Aria's own assembly is MASM |
| Invoking `build` | Must be called with `call`. EDK2 puts a `build.bat` wrapper on the path, and calling a batch file from a batch file without `call` transfers control permanently, silently truncating the calling script. |
| CI | `.github/workflows/build.yml` — builds on `windows-latest` |
| Debug output | Always on, including in RELEASE builds; `serial_print`/`log_write` are called directly rather than through the `DBG_*` macros |

### Not verified

**Aria has never been run.** Every claim above is build-level only. No part of
this document reflects an actual `VMLAUNCH` on hardware.

## Code audit

The VMCS layer was audited against the Intel SDM rather than assumed correct.

### Verified correct

- All VMCS field encodings in `vmm/vmcs.h` (~100 constants) match the SDM
  scheme: width in bits 14:13, access type in 11:10, index in 9:1. Includes the
  easy-to-miss ones � `VMCS_LINK_PTR` at 0x2800 as 64-bit *guest* state,
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

- `vmcs_validate_host_state()` was dead code � 95 lines of SDM 26.2 host-state
  checks that nothing called. It now runs at the top of `vmx_launch()`, so a
  host-state violation prints a specific message before the launch attempt. It
  is deliberately report-only: the build has not been run on hardware, and a
  validator false positive must not be able to block a launch that would
  otherwise work.
- The VM-instruction error table printed on launch failure mislabelled codes 7
  and 8 (they are VMREAD/VMWRITE errors, not control/host-state errors),
  invented a meaning for 9, and omitted 28 and 29 � the two codes a broken
  VMLAUNCH actually returns. It now matches SDM Vol 3C Table 30-1.

### Not audited

`ept_init.c` beyond the identity-map construction and MTRR typing, and the
assembly beyond the register-save order against `GUEST_REGS`.