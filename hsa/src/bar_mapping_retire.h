#pragma once
// A client's own revocation of its mapping of the GPU's BARs, as Linux
// revokes a process's mmap of a device that left (drm_dev_unplug's
// unmap_mapping_range, then ttm_bo_vm_dummy_page on the next fault).
//
// DriverKit gives a driver no way to revoke a mapping it handed a client:
// IOMemoryDescriptor and IOMemoryMap have no redirect or unmap in DriverKit,
// a terminated user client's mappings move to its port and stay mapped
// (IOUserClient::destroyUserReferences), and IOPCIFamily redirects nothing
// when the device goes. So the client does it: when its runtime learns the
// device is going (its gate closed for a removal or the session's close, a
// call answered kIOReturnNoDevice, the driver's notification), it maps
// zero-filled anonymous memory over the BAR range in one step.
//
// mmap with MAP_FIXED replaces the old entry under the task's map lock and
// removes its translations with a broadcast TLB invalidate and a DSB before
// it returns: from then on no thread of the process can store to the BAR
// through that range, however it was preempted. A store or load already
// past its translation completes first. Later stores and loads of the range
// go to the anonymous page (loads read zero), so scattered stores anywhere
// in the client (a memcpy into a VRAM kernarg ring, a Vulkan app's write to
// mapped VRAM, an HDP read back) neither fault nor reach the bus.
//
// Afterwards the range still belongs to the IOKit mapping in the kernel's
// books: IOConnectUnmapMemory64 for it deallocates the anonymous memory
// that now holds the range, so the runtime keeps the range reserved until
// it calls that, and never reuses the address before.
//
// This does not close the window between the link dropping and the
// runtime learning it: it removes every store after that, wherever it is,
// where the gate (doorbell_gate.h) covers only stores inside a bracket.
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>

// 0, or the errno of the failed mmap (the BAR mapping is then unchanged).
static inline int mac_hsa_bar_mapping_retire(void *address, size_t bytes)
{
    void *const replaced = mmap(address, bytes, PROT_READ | PROT_WRITE,
                                MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    return replaced == address ? 0 : (replaced == MAP_FAILED ? errno : EFAULT);
}
