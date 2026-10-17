// efi/smp.h — virtualise every logical processor
#pragma once

#include <Uefi.h>

// Locate EFI_MP_SERVICES_PROTOCOL and allocate a private VMXON region, VMCS and
// host stack for each application processor. Call after the BSP's VMCS is set up
// and before any launch, so each AP inherits the BSP's EPT and MSR bitmap.
EFI_STATUS aria_smp_init(void);

// Run the per-AP start-up procedure on every AP, bringing up VMX on each and
// launching it in place. Each AP keeps executing firmware code but does so as a
// guest from here on.
EFI_STATUS aria_smp_launch_aps(void);

UINTN aria_smp_ap_count(void);
UINTN aria_smp_ap_launched(void);
