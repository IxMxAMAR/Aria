// vmm/nested/shadow_ept.h — Shadow EPT lazy composition (spec §6)
#pragma once
#include "nested_vmx.h"

// Initialize a shadow EPT context for a given EPTP index
int  shadow_ept_init(NESTED_STATE* ns, uint32_t eptp_index);

// Compose a single page mapping on demand (lazy composition, spec §6.2)
// Called when L2 triggers an EPT violation on an uncomposed page.
// Returns: 0 = composed successfully (VMRESUME L2),
//          1 = L1's EPT doesn't map it (reflect to L1),
//         -1 = L0 hidden memory (inject #PF)
int  shadow_ept_compose_page(NESTED_STATE* ns, uint32_t eptp_index, uint64_t l2_gpa);

// Invalidate all entries in a shadow EPT (on L1's INVEPT single-context)
void shadow_ept_invalidate(NESTED_STATE* ns, uint32_t eptp_index);

// Invalidate ALL shadow EPTs (on L1's INVEPT global)
void shadow_ept_invalidate_all(NESTED_STATE* ns);

// Handle EPT violation on a hooked page in shadow EPT context
// Translates L2 GPA → HPA, matches against split pages, flips shadow entry
// Returns 1 if handled (hook flip done), 0 if not a hook page
int shadow_ept_handle_hook_violation(NESTED_STATE* ns, uint32_t eptp_index, uint64_t l2_gpa);

// Allocate a page from the shadow EPT pool (64MB, spec §6.5)
void* shadow_ept_alloc_page(NESTED_STATE* ns);
