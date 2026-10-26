# Roadmap

Ordered by what actually blocks the next step, not by what is most interesting.

## 0. Run it — blocking everything

Every item below that is marked implemented is build-verified only. Aria has
never executed a `VMLAUNCH` on hardware. Until it does, the whole tree is a
hypothesis with a linker receipt, and the next most valuable thing anyone can do
is not more code.

What a first run has to answer, in this order:

1. Does `VMLAUNCH` succeed at all, and if not, which VM-instruction error?
   `main.c` prints the SDM Table 30-1 meaning and stalls 30 seconds.
2. Does the first VM exit reach `vm_exit_stub` in the *relocated* image, on the
   host CR3, GDT, IDT and TSS that `efi/loader.c` built?
3. Does the `[+] EPT typing: N WB pages, M UC pages` line show a non-zero WB
   count, i.e. is guest RAM actually cacheable?
4. Do the APs launch, and does the machine survive them returning to the
   firmware's MP dispatch code as guests?
5. Does the guest survive `ExitBootServices`, and does hypercall 1 keep counting
   upward afterwards?

A machine with serial output answers all five; a machine without one still
answers them on the VGA text buffer, which is why the panic paths write there.

## 1. Survive `ExitBootServices` — implemented, unverified

Aria's code originally lived only in the module image, in `EfiLoaderCode`. When
the guest OS called `ExitBootServices` that memory became free to reuse while the
hypervisor's `HOST_RIP` still pointed into it, `HOST_CR3` was the firmware's page
tables, and the host `GDTR`/`IDTR`/`TR` bases pointed at firmware tables the OS
also reused. The first VM exit after the handoff therefore landed in reused
memory and faulted — and those exits are not rare, because the OS executes
`CPUID` within its first few thousand instructions and `CPUID` is intercepted.

`efi/loader.c` now keeps a copy of the whole image in `EfiRuntimeServicesCode`,
builds a host identity map, GDT, TSS and IDT in `EfiRuntimeServicesData`, and
installs them per-VMCS at launch time. No relocation engine was needed: the image
is position independent, which was verified rather than assumed (no `.reloc`
section, no `BASERELOC` directory, `ImageBase` 0, and no absolute pointers into
the image in `.data`).

`ExitBootServices` is still hooked, but only to record and verify the handoff —
see `docs/STATUS.md` for why re-copying the image there was wrong.

## 2. Virtualise the other logical processors — implemented, unverified

Only the BSP executed `VMXON`; every AP ran unvirtualised, so the guest saw
inconsistent behaviour between cores and none of the intercepts applied to AP
workloads.

`efi/smp.c` enumerates processors with `EFI_MP_SERVICES_PROTOCOL` and runs a
per-AP start-up procedure that allocates its own VMXON region, VMCS and host
stack, then does `VMXON` + `VMLAUNCH` in place. APs launch before the BSP so a
failure is reported while the machine is still in a known state.

This needed a real prerequisite, not just plumbing: an AP VMCS must point at the
relocated image, but that image was only valid after the handoff, and a launched
AP cannot repair its own VMCS. So the image is snapshotted at the end of setup and
the host state is applied per-VMCS. This is the riskiest component in the tree
and the first thing to disable if a first boot hangs.

## 3. Give the hypervisor its own memory protection — implemented, unverified

The EPT was a full RWX identity map, so the guest could read and write the
hypervisor's own pages. `ept_hide_range` now clears read/write/execute on the
4 KB entries covering the VMM's pages, and `ept_protect_own_tables` closes the
last gap by hiding the EPT's own page tables and the split pool.

Note the distinction this project draws: *protecting* the hypervisor is
integrity, and a violation is reported loudly. *Hiding* the hypervisor from the
guest is deception, and is out of scope.

## 4. Time — implemented, unverified

`VMCS_TSC_OFFSET` was written as 0 and never used, so a guest reading `RDTSC`
across exits observed time spent inside the hypervisor. The TSC-offsetting
control (processor-based bit 3) is now enabled and `vmm_handle_exit` charges its
own run time to the hypervisor, which keeps guest time monotonic.

Not done: TSC scaling, and `RDTSC` exiting. Neither is needed for a guest that
stays on one host.

## 5. Nested VMX

`vmm/nested/` is ~2,800 lines and excluded from the build. It is not close to
working: the merge table omits `VM_ENTRY_INTR_INFO`, exception error codes and
instruction length; there is no GPR context switch; `shadow_ept_compose_page()`
is never called; and a second `VMPTRLD` discards the first. It needs rewriting
against the working core rather than reviving as-is.

## 6. Interrupts, timers and the rest

In rough order of what a general-purpose guest would notice first:

- Interrupt/APIC virtualisation: virtual interrupt delivery, TPR shadow, posted
  interrupts, APIC-access page. Today the guest gets native interrupts and Aria
  has no say in their delivery.
- The VMX-preemption timer and interrupt-window exiting. A guest that masks
  interrupts and spins is never preempted.
- IO port interception plus an emulation path.
- An exception bitmap for selected vectors.
- EPT watchpoints for memory introspection.
- A guest-visible telemetry buffer (paged, rather than register-only).
- IOMMU / VT-d, so device DMA can be constrained at all.

## 7. Breadth

- A guest-side test image that exercises the hypercall ABI and asserts on the
  results, so a run produces a pass/fail rather than a serial log to read.
- Unit tests for the pure logic — MTRR resolution, EPT walk/split, segment
  descriptor decoding, MSR bitmap construction. None of these need VT-x.

## Explicitly out of scope

Concealment. Aria announces itself through CPUID leaf `0x40000000` and exposes a
documented hypercall ABI; that is deliberate, and a guest can always tell it is
virtualised.

The `anon` load option added in this pass suppresses the *signature* — no vendor
leaf, no hypervisor CPUID bit, `VMCALL` reflected as `#UD` — and that is as far as
this project goes. It is not concealment, and the difference is not a matter of
effort: CPUID always causes a VM exit in non-root operation, so the presence of a
hypervisor is measurable regardless of what the leaf returns. `docs/STATUS.md`
enumerates the remaining tells and the cost of each.

## Progress

- **0 — run it.** Not started. This is the only item that can invalidate
  everything else.
- **1 — surviving `ExitBootServices`.** Implemented in `efi/loader.c`, including
  the per-VMCS host state that item 2 needed. Build-verified only.
- **2 — AP virtualisation.** Implemented in `efi/smp.c`. Build-verified only,
  and the most likely thing to hang a first boot.
- **3 — EPT protection.** Complete at build level: `ept_hide_range` for the VMM
  pages, `ept_protect_own_tables` for the EPT's own tables and the split pool,
  and the split pool moved into runtime memory so the guest cannot free it.
- **4 — TSC offsetting.** Implemented and reachable; the control bit is masked
  off automatically on a CPU that does not allow it.
- **5 — nested VMX.** Not started. Deliberately behind item 0.
- **6, 7 — breadth.** Not started.
- **Identity mode.** Announcing stays the default; `AriaEfi.efi anon` suppresses
  the signature, and the VMX capability MSRs and `IA32_FEATURE_CONTROL` are
  masked to match, so a guest's CPUID and its RDMSRs agree with each other.
  Everything that still gives the hypervisor away — CPUID exit timing above all —
  is listed with a cost in `docs/STATUS.md`.
- Diagnostics, which is not a roadmap item but has absorbed most of the work:
  fatal exits now dump reason, qualification, `RIP`, `RSP`, GPA and exit count to
  VGA as well as serial, because a desktop with no serial has only the screen.
