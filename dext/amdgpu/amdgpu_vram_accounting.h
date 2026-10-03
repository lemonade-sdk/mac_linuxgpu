#pragma once
#include <stdint.h>

namespace amdgpu {
namespace vram_accounting {
// QueryInfo (21), tag 5. CPU accounting only, sampled on the allocator queue.
// Never interpret ExcludedBytes or PoolUsed as board-wide firmware occupancy.
//
// Self-contained ABI copy: only the Snapshot struct + valid() (the wire ABI
// the selector carries). The reference's snapshot() helper (which takes
// VRAMBumpAllocator references from amdgpu_vram.h) is dext-internal and is
// provided by the dext's own compute seam, not this header.
constexpr uint64_t kVersion = 1;
constexpr uint64_t kValid = 1;
enum Field : unsigned {
    Version, Flags, UsableBytes, VisibleBytes, ExcludedBytes,
    VisibleCapacity, VisibleUsed, VisibleFree, VisibleLargestSpan, VisibleCount,
    DeviceCapacity, DeviceUsed, DeviceFree, DeviceLargestSpan, DeviceCount,
    Count
};
struct Snapshot { uint64_t values[Count]{}; };
static_assert(sizeof(Snapshot) == 15 * sizeof(uint64_t), "scalar ABI");

inline bool valid(const Snapshot &s) {
    const auto *v = s.values;
    if (v[Version] != kVersion || (v[Flags] & ~kValid)) return false;
    if (!(v[Flags] & kValid)) {
        for (unsigned i = UsableBytes; i < Count; ++i) if (v[i]) return false;
        return true;
    }
    if (!v[UsableBytes] || !v[VisibleBytes] || v[VisibleBytes] > v[UsableBytes] ||
        v[VisibleCapacity] > v[VisibleBytes] ||
        v[DeviceCapacity] > v[UsableBytes] - v[VisibleBytes]) return false;
    for (unsigned pool = 0; pool < 2; ++pool) {
        const unsigned i = pool ? DeviceCapacity : VisibleCapacity;
        if (v[i + 1] > v[i] || v[i + 2] != v[i] - v[i + 1] ||
            v[i + 3] > v[i + 2] || v[i + 4] > v[i + 1] / 16384) return false;
    }
    return v[ExcludedBytes] == v[UsableBytes] - v[VisibleCapacity] - v[DeviceCapacity];
}
} // namespace vram_accounting
} // namespace amdgpu
