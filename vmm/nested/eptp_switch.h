// vmm/nested/eptp_switch.h — VMFUNC EPTP switching list management (spec §7)
#pragma once
#include "nested_vmx.h"

// Read L1's EPTP list, pre-compose shadow EPTs, write-protect list page
int  eptp_switch_setup(NESTED_STATE* ns);

// Rebuild shadow EPTP list after L1 modifies EPTP list page
void eptp_switch_rebuild(NESTED_STATE* ns);
