// vmm/vmcs.c — VMCS read/write using VMREAD/VMWRITE
#include "vmcs.h"

extern uint8_t __vmx_vmwrite(size_t field, size_t value);
extern uint8_t __vmx_vmread(size_t field, size_t* value);

// Track VMWRITE failures — checked before VMLAUNCH
volatile uint32_t vmcs_write_fail_count = 0;
volatile uint32_t vmcs_write_fail_field = 0;

void vmcs_write16(uint32_t field, uint16_t value) {
    if (__vmx_vmwrite((size_t)field, (size_t)value)) {
        vmcs_write_fail_count++;
        vmcs_write_fail_field = field;
    }
}

void vmcs_write32(uint32_t field, uint32_t value) {
    if (__vmx_vmwrite((size_t)field, (size_t)value)) {
        vmcs_write_fail_count++;
        vmcs_write_fail_field = field;
    }
}

void vmcs_write64(uint32_t field, uint64_t value) {
    if (__vmx_vmwrite((size_t)field, (size_t)value)) {
        vmcs_write_fail_count++;
        vmcs_write_fail_field = field;
    }
}

uint16_t vmcs_read16(uint32_t field) {
    size_t val = 0;
    __vmx_vmread((size_t)field, &val);
    return (uint16_t)val;
}

uint32_t vmcs_read32(uint32_t field) {
    size_t val = 0;
    __vmx_vmread((size_t)field, &val);
    return (uint32_t)val;
}

uint64_t vmcs_read64(uint32_t field) {
    size_t val = 0;
    __vmx_vmread((size_t)field, &val);
    return (uint64_t)val;
}
