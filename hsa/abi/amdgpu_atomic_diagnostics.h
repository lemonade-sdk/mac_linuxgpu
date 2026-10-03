#pragma once
#include <stdint.h>

namespace amdgpu {
// QueryInfo7 ABI: snapshots only owned shared memory/queue resources. No
// arbitrary MMIO addresses, cache changes or PCI configuration writes.
namespace atomic_diag {
enum Field : unsigned {
    Version, ValidFields, GPUAddress, DMAAddress, PTEVRAMOffset, PTEActual,
    PTEExpected, MQDGPUAddress, MQDBackingHQStatus0, PCIeCapabilityOffset,
    PCIeDeviceCapabilities2, PCIeDeviceControl2, GFXHUBPageTableBase,
    GFXHUBContext0Control, CPUMapOptions, CPUCacheAttributes, Count
};
constexpr uint64_t kVersion = 1;
constexpr uint64_t kPTEValid = 1, kMQDValid = 2, kPCIeValid = 4, kGFXHUBValid = 8;
constexpr uint64_t kKnownFields = 15;
constexpr uint64_t kUnknown = UINT64_MAX;
struct Snapshot { uint64_t values[Count]{}; };
static_assert(sizeof(Snapshot) == 128);

// PTE fields shared by every GMC generation this runtime targets (GFX9
// through GFX12, amdgpu_vm.h): VALID (bit 0), SYSTEM (bit 1) and the 4 KiB
// page address in bits 12-47. The remaining flag bits (snoop, MTYPE, PTE
// format) differ per generation; the driver computes PTEExpected with the
// device's own encoding, so the runtime checks only the shared fields.
constexpr uint64_t kPTESystemValid = 3;
constexpr uint64_t kPTEAddressMask = 0x0000fffffffff000ull;
inline bool snapshot_valid(const Snapshot &s, uint64_t expectedGPU) {
    const auto *v=s.values;
    if (v[Version]!=kVersion || (v[ValidFields]&~kKnownFields) ||
        (v[ValidFields]&(kPTEValid|kMQDValid))!=(kPTEValid|kMQDValid) ||
        v[GPUAddress]!=expectedGPU || (expectedGPU&7) || expectedGPU>=(1ull<<47) ||
        v[DMAAddress]>kPTEAddressMask || (v[DMAAddress]&4095)!=(expectedGPU&4095) ||
        (v[PTEVRAMOffset]&7) || (v[PTEExpected]&kPTESystemValid)!=kPTESystemValid ||
        (v[PTEExpected]&kPTEAddressMask)!=(v[DMAAddress]&~uint64_t(4095)) ||
        !v[MQDGPUAddress] || (v[MQDGPUAddress]&16383) || v[MQDGPUAddress]>=(1ull<<48) ||
        v[MQDBackingHQStatus0]>UINT32_MAX || v[CPUMapOptions]!=0 || v[CPUCacheAttributes]!=kUnknown)
        return false;
    if (v[ValidFields]&kPCIeValid) {
        if (v[PCIeCapabilityOffset]<0x40 || v[PCIeCapabilityOffset]>0xd4 ||
            (v[PCIeCapabilityOffset]&3) || v[PCIeDeviceCapabilities2]>UINT32_MAX ||
            v[PCIeDeviceControl2]>UINT16_MAX) return false;
    } else if (v[PCIeCapabilityOffset]!=kUnknown || v[PCIeDeviceCapabilities2]!=kUnknown ||
               v[PCIeDeviceControl2]!=kUnknown) return false;
    if (v[ValidFields]&kGFXHUBValid) {
        if (v[GFXHUBContext0Control]>UINT32_MAX) return false;
    } else if (v[GFXHUBPageTableBase]!=kUnknown || v[GFXHUBContext0Control]!=kUnknown) return false;
    return true;
}
} // namespace atomic_diag
} // namespace amdgpu
