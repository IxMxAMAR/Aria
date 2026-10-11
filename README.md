# Aria

A Type-1 hypervisor for x86-64 that loads from UEFI and runs the operating system
as its guest.

Aria is loaded as an EFI application. It enables VMX, builds an EPT and a VMCS
describing the machine it is running on, and executes `VMLAUNCH`. From that point
the CPU is in VMX root operation, and everything that runs afterwards — the
firmware boot manager, then Windows — runs in VMX non-root, under Aria.

Aria is a research and learning project. It is not hardened, and it is not a
security boundary. See [docs/STATUS.md](docs/STATUS.md) for exactly what works and
what does not; the list of unimplemented pieces is currently longer than the list
of implemented ones.

## Design principles

**Transparent, not hidden.** Aria announces itself to the guest through the
hypervisor CPUID leaf and passes real hardware state through. It does not attempt
to conceal its presence from the guest operating system or from software running
inside it. A hypervisor that lies to its guest is a different (and much larger)
project; Aria is not that.

**Fail loudly.** An unhandled VM exit stops with a diagnostic rather than skipping
the instruction. `VM_EXIT_INSN_LEN` is only defined for instruction-caused exits,
so blindly advancing the guest RIP silently corrupts guest state and turns a
five-minute debugging session into a five-day one.

You can verify Aria from inside the guest: CPUID leaf `0x40000000` returns the
vendor string `AriaAriaAria` in `EBX:ECX:EDX`, and leaf `0x40000001` returns the
interface signature `A0.1`. This is set by `ARIA_ANNOUNCE_VENDOR` in
[vmm/vmm.c](vmm/vmm.c).

## Layout

| Path | Contents |
|---|---|
| `efi/` | UEFI application: entry point, VMX bring-up, VMCS programming, EPT construction |
| `vmm/` | Core VMM: exit dispatcher, VMCS accessors, EPT manipulation, exit handlers |
| `vmm/nested/` | Nested VMX engine — Phase 2, not built yet (see STATUS) |
| `shared/` | Structures shared between the hypervisor and guest-facing interfaces |
| `scripts/` | Build and deploy scripts |
| `mok/` | Self-signed certificate for the Secure Boot / MOK path |

## Building

Requirements:

- An EDK2 checkout
- Python 3 with `edk2-basetools` (`pip install edk2-basetools`)
- Visual Studio with the C++ toolset (provides `cl.exe` and `ml64.exe`)

```cmd
git clone --depth 1 --branch edk2-stable202411 https://github.com/tianocore/edk2.git C:\edk2
pip install edk2-basetools
set ARIA_EDK2=C:\edk2
scripts\build_efi.bat
```

The build script exposes this repository to the EDK2 workspace as the `AriaPkg`
package (via a directory junction), so the DSC can reference `AriaPkg/efi/...`.
Output is `Build\Aria\RELEASE_VS2022\X64\AriaEfi.efi`.

## Running

Aria is a UEFI application, so it has to run before the OS is booted.

1. Copy `AriaEfi.efi` to a USB drive.
2. Boot the UEFI Shell from that drive.
3. Run `AriaEfi.efi`.

Aria configures the VMCS for the current processor, executes `VMLAUNCH`, and
returns to the firmware. The firmware's boot manager then loads Windows, which
runs as the guest.

**Use a machine you can afford to hard-hang.** Current limitations are
significant — in particular there is no SMP support and no host lifetime
management, both of which can stop the machine dead. Read
[docs/STATUS.md](docs/STATUS.md) first.

## License

MIT — see [LICENSE](LICENSE).
