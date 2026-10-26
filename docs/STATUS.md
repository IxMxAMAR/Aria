# Aria — implementation status

Per-subsystem status, kept honest on purpose. The vocabulary is deliberate:

- **works** — verified by a real build and a real run on hardware
- **implemented** — the code exists and is reachable from the entry point
- **partial** — some of it exists; the gaps are listed
- **not implemented** — the code does not exist, or exists but is unreachable
- **excluded** — source is present but deliberately not built

Nothing in this document is **works**. Aria has been built and inspected but
never run; see "Not verified" at the end. If a row says "implemented" and you
find it does not work, that is a bug in this document as much as in the code.
Please fix the document.

## Core

| Subsystem | State | Notes |
|---|---|---|
| UEFI application entry + bring-up | implemented | `efi/main.c` |
| VT-x capability check | partial | Tests `CPUID.1:ECX[5]` and `IA32_FEATURE_CONTROL` bit 2 only; never tests bit 1 (VMX outside SMX) |
| VMXON / VMCLEAR / VMPTRLD | implemented | Statuses checked (`efi/vmx_init.c`) |
| VMCS host-state population | implemented | Per-VMCS, so the BSP and every AP each get their own |
| VMCS host-state validation | implemented | `vmcs_validate_host_state()` against SDM 26.2, report-only |
| VMCS guest-state population | implemented | Guest state is the live CPU state (in-place design) |
| VMCS control fields | implemented | See "Control-field notes" |
| VM-entry / VM-exit | implemented | `vmx_do_launch` in `vmm/asm/vmx_asm.asm` |
| Exit dispatcher | implemented | `vmm/vmm.c`; a `switch`, not a table |
| Unhandled-exit behaviour | implemented | Reports to serial *and* VGA, then halts (`ARIA_STRICT_EXITS`) |
| EPT construction | partial | Full identity map: 2 MB pages 0–4 GB, 1 GB pages 4–512 GB |
| EPT capability validation | implemented | Reads `IA32_VMX_EPT_VPID_CAP` before building |
| EPT memory typing | partial | MTRR-derived; see caveats |
| EPT protection of hypervisor memory | implemented | `ept_hide_range` + `ept_protect_own_tables` |
| Hypervisor lifetime across `ExitBootServices` | implemented | `efi/loader.c` |
| SMP / AP virtualisation | implemented | `efi/smp.c` |
| Guest-visible identity | implemented | announce (default) or anonymous, chosen by UEFI load options — see "Anonymous mode" |

## Interception

| Subsystem | State | Notes |
|---|---|---|
| CPUID interception | implemented | Announces Aria; VMX bit reported absent |
| CR access interception | partial | CR4 only: `CR4_GUEST_HOST_MASK` is `CR4_VMXE`, `CR0_GUEST_HOST_MASK` is 0 |
| MSR write interception | implemented | The 8 MSRs VM entry reloads from the VMCS |
| MSR read interception | implemented, unreachable | No read is intercepted, so the handler never runs |
| I/O port interception | not implemented | Unconditional I/O exiting is off; guest has native port I/O |
| Exception interception | not implemented | Exception bitmap is 0 |
| Interrupt / NMI interception | not implemented | Disabled in pin-based controls by design |
| XSETBV interception | implemented | XCR index validated before forwarding |
| INVD interception | implemented | Treated as a no-op with the guest RIP advanced |
| VMX instruction exits | implemented | Reported as `#UD`, matching a CPU without VMX |
| HLT / MWAIT / RDTSC / INVLPG | not implemented | Not intercepted, so the guest executes them natively |
| TSC offsetting / scaling | implemented (offsetting only) | Processor-based control bit 3; TSC scaling is not used |

## Control-field notes

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
- **Processor-based control bit 3 is "Use TSC offsetting"** (not HLT exiting,
  which is bit 7). The numbering was checked against the SDM layout as encoded in
  Linux's `arch/x86/include/asm/vmxfeatures.h`, because the comment in
  `vmx_init.c` and the accounting in `vmm.c` are only consistent if that is the
  bit.
- **HLT exiting is deliberately not requested.** If it ever is, `vmm_handle_exit`
  has no `HLT` case and `ARIA_STRICT_EXITS` would halt the machine on the guest's
  first `HLT`, which for Windows is the idle loop.

## Exit handling and diagnostics

The dispatcher handles reasons 0 (exception/NMI), 1 (external interrupt), 2
(triple fault), 10 (CPUID), 13 (INVD), 18 (VMCALL), 19–27 (the VMX instruction
exits, all reflected as `#UD`), 28 (CR access), 31 (MSR read), 32 (MSR write), 48
(EPT violation), 49 (EPT misconfiguration), 50 (INVEPT), 53 (INVVPID), 55
(XSETBV) and 59 (VMFUNC). Everything else is fatal: reason, qualification, guest
`RIP`/`RSP`, GPA and exit count go to serial *and* to the VGA text buffer before
the machine halts. A desktop usually has no serial port, so the VGA copy is the
one that matters.

`aria_exit_reason_counts[65]` counts exits per reason and `aria_exit_log[16]`
keeps the first sixteen in order, so a crash shows the sequence that led to it.
Both are readable from the guest through hypercall 2.

## Hypervisor interface

Aria announces itself rather than trying to hide.

    leaf 0x40000000  vendor "AriaAriaAria", EAX = highest leaf (0x40000002)
    leaf 0x40000001  EAX = ABI version, EBX = feature flags, ECX = max call
    leaf 0x40000002  live VM-exit counter (EDX:EAX)

    call 0  VERSION      RBX = ABI version, RCX = "Aria"
    call 1  EXIT_COUNT   RBX = total VM exits since launch
    call 2  EXIT_REASON  in RCX = reason; RBX = count for that reason
    call 3  EPT_TYPING   RBX = WB pages, RCX = UC pages
    call 4  FINALIZE     internal — issued by the ExitBootServices hook
    call 5  TSC_OFFSET   RBX = accumulated TSC offset (signed)

An unknown call returns RAX = 1. This is the point of the interface: a guest can
confirm the hypervisor is really there, how many exits it has taken, and whether
its RAM is mapped cacheable, rather than trusting the hypervisor's own log.

## Anonymous mode

Selected at load time, not build time:

    AriaEfi.efi          announce (default)
    AriaEfi.efi anon     anonymous

The option is read from `EFI_LOADED_IMAGE_PROTOCOL->LoadOptions` in
`efi/main.c` and lands in `aria_identity` (`vmm/vmm.c`). Anything unrecognised
leaves the announcing default in place, deliberately: concealment should be asked
for, not acquired by mistyping.

This reverses the design principle the README used to state outright, so it is
worth being precise about what it buys.

### What it removes — the signature

| Surface | Announce | Anonymous |
|---|---|---|
| CPUID `0x40000000` | `"AriaAriaAria"`, EAX = 0x40000002 | falls through to the hardware query |
| CPUID `0x40000001` | ABI version, feature flags, max call | falls through |
| CPUID `0x40000002` | live exit counter | falls through |
| CPUID `0x40000003`–`0x4000000F` | zeroed | falls through |
| CPUID.1:ECX[31] | set | cleared |
| `VMCALL` | hypercall ABI | `#UD`, RIP not advanced |

### Closed in this pass

| Was | Now |
|---|---|
| `RDMSR 0x480`–`0x491` returned real capability values while CPUID said VMX was absent | the read is intercepted and raises `#GP`, which is what a processor with no VMX does — those registers do not exist |
| `IA32_FEATURE_CONTROL` bit 2 said VMXON was enabled | the read is intercepted; bits 1 and 2 read as zero, bit 0 (lock) is left as firmware set it |
| `VMCALL(4)` reached `aria_loader_finalize()` | gated on `ARIA_HC_FINALIZE_TOKEN` in RCX, so probing VMCALL cannot reach it |

The first two were worth more than the vendor string. "CPUID says no VMX, RDMSR
says here is a VMCS revision ID" is a pair no real processor produces, so masking
one without the other was a *stronger* tell than announcing.

### What it does not remove — the evidence

Each row is a working detector against the current tree, with what it would cost
to close. This is the part that matters: the mode is not a stealth mode, and
calling it one would be dishonest.

| Evidence | Why it survives | Cost to close |
|---|---|---|
| **`CPUID` timing** | CPUID *always* causes a VM exit in non-root operation — there is no control bit for it. Each exit costs three VMCS accesses (read RIP, write RIP, write TSC offset) plus the handler. | Not closable with VMX. Only reducible: shorten the exit path. |
| **`CR4` and MSR write latency** | `CR4_GUEST_HOST_MASK` is `CR4_VMXE` and eight MSRs are write-intercepted, so each write exits and can be timed. | Medium, and it fights correctness: `handle_msr_write` exists precisely because those writes would otherwise be reverted at the next entry. |
| **TSC behaviour** | `VMCS_TSC_OFFSET` freezes the guest TSC while in root context, so RDTSC diverges from any reference clock by roughly the time spent in the hypervisor. The accumulator is shared by every processor: the update is atomic now, so no update is lost, but each core still adds its own time to the same total and then writes that total to its own VMCS, so the correction is over-applied once the APs are live. | Medium: per-CPU offsets (a per-CPU GS block is the usual mechanism) plus TSC scaling. |
| **EPT holes** | Hypervisor pages are no-access in the EPT, so a detector that walks guest-physical RAM takes an EPT violation — and `ARIA_STRICT_EXITS` halts the machine. That is the loudest possible answer. | High, and it trades directly against EPT protection: faking reads of those pages needs an emulation path that does not exist. |
| **Interrupts** | Not a leak. Nothing is virtualised — no virtual interrupt delivery, no TPR shadow, no APIC-access page — so interrupt behaviour and latency are native. | — |

### Cost, measured

Compiling the announcement out entirely was 512 bytes smaller (36,864 against
37,376). Keeping both paths and selecting at runtime costs 512 bytes over the
announce-only build (37,888 against 37,376), because the vendor leaf, the ABI
leaf and the hypercall path are all still in the image. Closing the MSR read leak
and gating `VMCALL(4)` cost nothing measurable: the image is still 37,888 bytes.

The honest summary: anonymous mode is cheap and it defeats a casual `cpuid`
check. It does not defeat a detector that measures CPUID cost, which is the first
thing a serious one does, and it cannot, because VMX requires CPUID to exit. The
reachable goal is "no name, no ABI, no gratuitous tells". The unreachable one is
"invisible".

## Surviving `ExitBootServices`

`efi/loader.c` keeps a copy of the whole image in `EfiRuntimeServicesCode` and
gives the host its own state in `EfiRuntimeServicesData`:

    image copy   EfiRuntimeServicesCode, allocated below 4 GB
    host CR3     identity map, 0-16 GB, 2 MB pages
    host GDT     flat 64-bit code/data plus a TSS descriptor
    host TSS     104-byte, RSP0 = top of the host stack
    host IDT     every vector -> host_fault_halt

No relocation engine is needed. The image is position independent, which was
verified rather than assumed: there is no `.reloc` section and no `BASERELOC`
data directory, `ImageBase` is 0, and a scan of `.data` found no absolute
pointers into the image — the four 8-byte values that initially looked like
pointers sit inside ASCII string literals.

The host state is installed **at launch**, not at the handoff, by
`aria_loader_apply_host_state()` — called for the BSP in `main.c` and for each AP
inside its own start-up procedure. From the first VM exit onward the hypervisor
therefore runs from the relocated copy, on its own page tables, GDT, IDT and TSS.
The host identity map only has to cover the runtime image and the host stack,
both allocated below 4 GB, so nothing on the exit path touches memory above that.

`aria_loader_finalize()` (hypercall 4) is the handoff marker. It does **not**
re-copy the image: the code executing it *is* the relocated copy and has been
accumulating state since the first exit, so copying the original over it would
reset the exit counters and the accumulated TSC offset to their setup-time
values, and would overwrite the `finalized` flag it had just set, defeating its
own idempotence check. It records the handoff, prints `HOST_RIP` and `HOST_CR3`,
and reports loudly if either has left runtime-preserved memory.

The exit stub executes `cli` on entry so the handler cannot be interrupted in
host context; VM entry restores the guest `RFLAGS` from `GUEST_RFLAGS`, so the
guest `IF` is unaffected.

**Unverified.** A hardware run needs to confirm that the image really is position
independent at a nonzero base, that the host CR3 and GDT are accepted on the
first exit, and that no relocated path touches `gBS`/`gST`, which are freed at
the handoff.

## EPT protection of hypervisor memory

`ept_hide_range` clears read/write/execute on the 4 KB EPT entries covering the
hypervisor's own pages, so the guest faults on any access instead of being able
to read or overwrite the hypervisor. This is integrity, not concealment: the
access takes an EPT violation and the panic dump names the GPA.

Protected: VMXON region, VMCS region, MSR bitmap, host stack, the relocated
image, the loader's host CR3/GDT/IDT pages, the EPT's own page tables (PML4,
PDPT, PD and any split PT), and the whole split pool.

`ept_protect_own_tables` runs last, because hiding a page can itself split a
2 MB entry and allocate a page table from the pool. It is safe for two reasons:
hiding a leaf entry for one GPA does not disturb the entries used to walk to any
other GPA, and the CPU walks the EPT through physical addresses rather than
through the EPT, so an active page table can have its own GPA hidden without
affecting the walk.

Two bugs had to be fixed first, both of which would only have surfaced on
hardware:

- The split pool was a 4 MB static array in `vmm/ept.c`, that is, in the image's
  BSS. Splitting a 2 MB page allocated the new page table from there, so the EPT
  would have pointed at memory the guest frees at `ExitBootServices`. It is now
  allocated as `EfiRuntimeServicesData` and handed to `ept_set_pool()`.
- `ept_split_2mb_to_4kb` hardcoded memory type WB for all 512 entries instead of
  preserving the parent's, so splitting a single page inside an uncached region
  would have silently made the whole region cacheable.

Splitting also makes `ept_wb_pages`/`ept_uc_pages` (hypercall 3) stale for the
split regions, because those counts are accumulated over 2 MB pages at build
time.

## Time

`VMCS_TSC_OFFSET` is live. Processor-based control bit 3 ("use TSC offsetting")
is enabled, and `vmm_handle_exit` charges the time it spends in root context to
the hypervisor: the offset accumulates `-(tsc_out - tsc_in)`, so the guest's
`RDTSC` does not advance while the guest is not running. The accumulated value is
readable through hypercall 5.

`vmx_adjust_controls` masks bit 3 off if `IA32_VMX_PROCBASED_CTLS` does not allow
it, in which case the offset writes are harmless no-ops. TSC scaling is not used,
so migrating a running guest to a host with a different TSC frequency is out of
scope.

## AP virtualisation

`efi/smp.c` locates `EFI_MP_SERVICES_PROTOCOL`, allocates a private VMXON region,
VMCS and host stack for each AP, and runs a start-up procedure on every AP that
does `vmx_enable` + `vmx_setup_vmcs` + `aria_loader_apply_host_state` +
`vmx_do_launch` on itself. Each AP keeps executing firmware code but is a guest
from that point on. Every AP inherits the BSP EPT, EPTP, MSR bitmap and control
settings through a `CopyMem` of `VMM_STATE`, so all processors share one EPT.

APs are launched before the BSP, so a failure is reported while the machine is
still in a known state.

Two things made this possible, and both were prerequisites rather than plumbing:

- The host state had to be installed at launch time, not at the handoff. An AP
  VMCS must point at the relocated image, and once an AP has launched it cannot
  repair its own VMCS, because the only correct host state is the one that would
  already be too late to install. `aria_loader_snapshot()` runs at the end of
  setup, and `aria_loader_apply_host_state()` takes a `VMM_STATE*` so it can be
  applied per-VMCS.
- The launch does not go through `vmx_launch()`, which writes to the UEFI console
  and is not safe to call concurrently from several APs. The AP procedure calls
  `vmx_do_launch()` directly and reports on serial.

**Unverified, and the riskiest thing in the tree.** `StartupAllAPs` is called with
a zero timeout; `vmx_setup_vmcs()` still writes to serial, so AP output can
interleave; and an AP returning to the firmware MP dispatch code *as a guest* has
never been observed. If a first hardware run hangs, disabling this is the first
thing to try.

## EPT memory typing

`mtrr_is_wb()` used to return UC for any address not covered by a variable MTRR.
On real firmware `IA32_MTRR_DEF_TYPE` is normally WB and system RAM is
deliberately left uncovered — the variable registers exist to mark MMIO holes UC.
So the whitelist marked every byte of RAM uncached, which is why UEFI services
(USB, GOP) hung: their DMA buffers were mapped uncached.

Uncovered addresses now take the MTRR default type, which is what SDM Vol 3
11.11.2.1 specifies. The builder counts the resulting page types and prints them,
so a hardware run can confirm the fix instead of assuming it:

    [+] EPT typing: N WB pages, M UC pages

A WB count of 0 means the typing is still wrong.

Residual risk: a 1 GB page that spans an MMIO hole lying strictly inside it is
typed from its two endpoints. Firmware normally covers MMIO with UC variable
MTRRs, which the endpoint checks do catch, but this is not a general
MTRR-overlap resolution.

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

## Not implemented

| Gap | Impact |
|---|---|
| **Interrupt / APIC virtualisation** | No virtual interrupt delivery, no TPR shadow, no posted interrupts, no APIC-access page. |
| **Timers / preemption** | No VMX-preemption timer, no interrupt-window exiting. A guest that masks interrupts and spins is never preempted. |
| **IOMMU / VT-d** | No DMA remapping; devices DMA against host physical addresses, bypassing the EPT. |
| **Secure Boot signing** | `mok/` holds a self-signed public certificate. There is no private key in the repository and no signing step in any script. MOK is a shim concept and cannot authorize a firmware-launched UEFI application, so the current path is incomplete. |
| **Testing** | No unit tests and no guest-side test image. CI builds only. |
| **Nested VMX** | See above. |

## Security

Aria is not a security boundary, and as it stands it is not a reliable isolation
layer either. Read this before running it on hardware you care about.

- **Hypervisor memory is protected but not isolated.** VMM pages are no-access in
  the EPT, so a guest write takes an EPT violation and halts with the GPA named.
  That is integrity and diagnosis, not containment: the halt is a denial of
  service, and the exit path still trusts guest state.
- **The host no longer uses the guest's IDT and GDT.** `efi/loader.c` builds a
  private GDT/TSS and an IDT whose every vector halts visibly.
- **No DMA isolation.** No IOMMU, and the guest has native port I/O.
- **Unvalidated guest state could halt the host.** Guest `CR4` writes are masked
  against `CR4_FIXED0`/`CR4_FIXED1`, which removes one path to a failed VM entry;
  the general class of problem remains.

## Build

**Verified.** A clean rebuild (build output deleted, then rebuilt) completes with
no errors and no warnings from Aria's own sources, and produces:

```
AriaEfi.efi   37,888 bytes
PE32+, machine 0x8664, subsystem 10 (EFI Application), entry RVA 0x1020
SHA-256  BA073F4BDAFB9B603DD146E754EF48B358F5968C84CFAAAD32C0C4D6ED541516
```

on Visual Studio 18 Community with MSVC 14.50.35717, against EDK2
`edk2-stable202411`, with NASM 2.16.03. Repeating the build produces a
byte-identical image — checked by deleting `Build\Aria` and rebuilding.

The only output in the log that says "warning" is EDK2's own command line passing
`/GS` and `/GL` twice (`D9025: overriding '/GS' with '/GS-'`, and the same for
`/GL`) on every module in the tree, Aria's included. It is not a diagnostic about
this code and it appears identically for unmodified MdePkg libraries.

An earlier revision of this document recorded a 26,624-byte image with SHA-256
`2C80…5376`. That was true of the tree before EPT protection, the loader and AP
bring-up landed, and was left in place through three commits that changed the
image. The numbers above are the ones that match the current source; regenerate
them rather than trusting a remembered hash.

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

**Aria has never been run.** Every claim above is build-level or
source-inspection only. No part of this document reflects an actual `VMLAUNCH` on
hardware, and several of the risks listed here can only be settled by one.

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
  and `VMCS_MSR_BITMAP` is programmed; the bitmap intercepts writes to the eight
  MSRs VM entry reloads from the VMCS and lets every read and every other MSR run
  natively. Secondary controls enable EPT and VPID. Pin-based requests no
  external-interrupt or NMI exiting, so the guest keeps its own interrupts.
- `VMCS_VMCS_LINK_PTR` is 0xFFFFFFFFFFFFFFFF, required when VMCS shadowing is
  off.
- `exit_stub.asm` pushes fifteen registers plus one 8-byte pad, which is exactly
  the `GUEST_REGS` layout in `vmm/vmm.h` including the unused `rsp` slot, and
  leaves `rsp` 16-byte aligned at the `call` so the x64 ABI is satisfied.
  `handle_cr_access` knows the `rsp` slot is not a real guest `RSP` and reads it
  from the VMCS instead.
- `handle_msr_write()` validates `IA32_EFER` before writing it: it runs in host
  context, so an unvalidated value would `#GP` the hypervisor rather than the
  guest. A guest attempt to clear `LMA` or set a reserved bit is reflected back
  as a guest `#GP`, and the instruction is not advanced.

### Fixed in earlier passes

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

### Fixed in the previous pass

- `aria_loader_finalize()` re-copied the whole image at the handoff. Because the
  relocated copy is the running hypervisor from the first VM exit onward, that
  overwrote live state with a snapshot frozen at setup: exit counters and the
  accumulated TSC offset jumped backwards, and the `finalized` flag it had just
  set was overwritten by the copy, so the idempotence check never held. It now
  records the handoff and verifies `HOST_RIP`/`HOST_CR3` instead of copying.
- The `ExitBootServices` hook guarded on `g_loader.finalized` in *its* image, but
  `aria_loader_finalize()` runs in the relocated copy and updates a different
  object, so the guard was dead and every retry re-issued the hypercall. The flag
  now lives on the hook's side.
- `main.c` logged `"[*] EPT hiding SKIPPED (debug)"` at a point where hiding had
  not yet happened and was then performed a few dozen lines later, so the log
  claimed the opposite of what the code did.

### Fixed in this pass

- The exit counters and the TSC-offset accumulator were plain read-modify-writes
  on globals that every processor touches once the APs are launched, so
  concurrent exits lost updates. They go through `aria_atomic_add64`
  (`lock xadd`) now. `aria_exit_count` was also 32-bit while being handed to the
  guest as `EDX:EAX` (CPUID `0x40000002`) and as a 64-bit hypercall return, so it
  wrapped silently at 4G exits; it is 64-bit.
- In anonymous mode the VMX capability MSRs and `IA32_FEATURE_CONTROL` answered
  with real values while CPUID reported VMX absent. Reads of `0x480`–`0x491` now
  raise `#GP` and `IA32_FEATURE_CONTROL` reads back with the VMXON bits cleared.
- `ARIA_HC_FINALIZE` was reachable by any guest that issued `VMCALL(4)`; it now
  requires `ARIA_HC_FINALIZE_TOKEN` in RCX, in every identity mode.

### Not audited

`ept_init.c` beyond the identity-map construction and MTRR typing, and the
assembly beyond the register-save order against `GUEST_REGS`.
