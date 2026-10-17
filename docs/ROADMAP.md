# Roadmap

Ordered by what actually blocks the next step, not by what is most interesting.

## 1. Survive `ExitBootServices` — blocking

This is the single thing standing between "it launches" and "it runs Windows".

Aria's code lives in the module image, which the firmware loads into
`EfiLoaderCode`. When the guest OS calls `ExitBootServices`, that memory becomes
free for the OS to reuse — and the hypervisor's `HOST_RIP` still points into it.
`HOST_CR3` is likewise the firmware's page tables, which the OS replaces, and the
host `GDTR`/`IDTR`/`TR` bases point at firmware tables the OS also reuses.

The first VM exit after the handoff therefore lands in reused memory and faults.
Those exits are not rare: the OS executes `CPUID` within its first few thousand
instructions, and `CPUID` is intercepted.

What is already safe: the VMCS, VMXON region, EPT, MSR bitmap and host stack are
all allocated as `EfiRuntimeServicesData`, which the OS preserves.

What has to be built — a small loader:

1. Copy the module image (at least `.text`, `.rdata`, `.data`) into
   runtime-allocated pages, applying base relocations.
2. Build a host page table that identity-maps those pages plus the host stack,
   and point `HOST_CR3` at it.
3. Rebuild a host GDT/IDT and the TSS inside the same region and point
   `HOST_GDTR_BASE`, `HOST_IDTR_BASE` and `HOST_TR_BASE` at them.
4. Hook `ExitBootServices` to finalise the handoff, and to fail loudly if the
   relocation did not happen.

Until this exists, Aria is a proof of concept that runs until the guest takes
over the machine. That should be stated plainly rather than implied.

## 2. Virtualise the other logical processors — blocking for a real guest

Only the BSP executes `VMXON`. Every AP runs unvirtualised, so the guest sees
inconsistent behaviour between cores and none of the intercepts apply to AP
workloads.

The shape: enumerate processors with `EFI_MP_SERVICES_PROTOCOL` and run a per-AP
initialisation procedure that allocates its own VMXON region, VMCS and host
stack, then does `VMXON` + `VMLAUNCH` in place. Each AP needs its own per-CPU
state; `g_vmm` being a single global is the first thing to change.

## 3. Give the hypervisor its own memory protection

The EPT is a full RWX identity map, so the guest can read and write the
hypervisor's own pages. Marking the VMM's pages no-access in the EPT and
reporting violations is the standard fix.

Note the distinction this project draws: *protecting* the hypervisor is
integrity, and a violation should be reported loudly. *Hiding* the hypervisor
from the guest is deception, and is out of scope.

## 4. Time

`VMCS_TSC_OFFSET` is written as 0 and never used. A guest reading `RDTSC` across
exits observes time spent inside the hypervisor. Using the TSC-offsetting control
(and/or intercepting `RDTSC`) makes guest time monotonic and stable.

## 5. Nested VMX

`vmm/nested/` is ~2,800 lines and excluded from the build. It is not close to
working: the merge table omits `VM_ENTRY_INTR_INFO`, exception error codes and
instruction length; there is no GPR context switch; `shadow_ept_compose_page()`
is never called; and a second `VMPTRLD` discards the first. It needs rewriting
against the working core rather than reviving as-is.

## 6. Breadth

- IO port interception plus an emulation path
- Exception bitmap for selected vectors
- EPT watchpoints for memory introspection
- A guest-visible telemetry buffer (paged, rather than register-only)

## Explicitly out of scope

Making Aria hard to detect. It announces itself through CPUID leaf `0x40000000`
and exposes a documented hypercall ABI; that is deliberate. A guest can always
tell it is virtualised, which is the honest design and the one worth building.

## Progress

- Item 1 � surviving `ExitBootServices` � is implemented in `efi/loader.c`.
  Build-verified only; not yet observed on hardware. See `docs/STATUS.md`.- Item 4 (TSC offsetting) is implemented: processor-based control bit 3 plus
  per-exit accounting in `vmm_handle_exit`. Build-verified only.
- Fatal exits now dump reason, qualification, RIP, RSP, GPA and exit count to VGA
  as well as serial, because a desktop with no serial has only the screen.
- Item 2 (AP virtualisation) is deliberately not started yet. `vmx_do_launch`
  writes GUEST_RSP/GUEST_RIP from the live context and `vmx_launch` already takes
  a `VMM_STATE*`, so AP bring-up is mechanically straightforward - but it is the
  single most likely thing to hang a machine on first boot, and stacking it on an
  entirely unverified base makes a first hardware failure impossible to isolate.- Item 3 (EPT protection of hypervisor memory) is implemented via
  `ept_hide_range`, with the split pool moved to runtime memory and the split
  preserving the parent memory type. Build-verified only. The EPT own page tables
  are not yet protected.
- Item 2 (AP virtualisation) is blocked on a loader refactor, not on the MP
  protocol: an AP needs the relocated image to be valid at launch time, so the
  image has to be snapshotted at the end of setup as well as at the handoff, and
  the host state applied per-VMCS. See `docs/STATUS.md`.- Item 2 (AP virtualisation) is implemented in `efi/smp.c`, together with the
  loader refactor it required: the image is snapshotted at the end of setup as well
  as at the handoff, and host state is applied per-VMCS. Build-verified only. This
  is the riskiest component in the tree and the first thing to disable if a first
  boot hangs.- Item 3 is now complete at build level: `ept_protect_own_tables` also hides the
  EPT own page tables and the split pool, closing the gap recorded above.