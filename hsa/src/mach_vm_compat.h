#pragma once

// mach_vm_map / mach_vm_deallocate / mach_vm_region on every Apple platform.
// The iOS SDK withholds <mach/mach_vm.h>; on arm64 the vm_map family takes
// the same 64-bit addresses and sizes, so these wrappers are exact.
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IOS
#include <mach/vm_map.h>
namespace mac_hsa {
static_assert(sizeof(vm_address_t) == sizeof(mach_vm_address_t));
inline kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size,
                                 mach_vm_offset_t mask, int flags, mem_entry_name_port_t object,
                                 memory_object_offset_t offset, boolean_t copy, vm_prot_t current,
                                 vm_prot_t maximum, vm_inherit_t inheritance) {
    vm_address_t placed = vm_address_t(*address);
    const auto result = ::vm_map(task, &placed, vm_size_t(size), vm_address_t(mask), flags, object,
                                 vm_offset_t(offset), copy, current, maximum, inheritance);
    *address = placed;
    return result;
}
inline kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size) {
    return ::vm_deallocate(task, vm_address_t(address), vm_size_t(size));
}
inline kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size,
                                    vm_region_flavor_t flavor, vm_region_info_t info,
                                    mach_msg_type_number_t *count, mach_port_t *object) {
    vm_address_t found = vm_address_t(*address);
    vm_size_t bytes = 0;
    const auto result = ::vm_region_64(task, &found, &bytes, flavor, info, count, object);
    *address = found; *size = bytes;
    return result;
}
}
#else
#include <mach/mach_vm.h>
#endif
