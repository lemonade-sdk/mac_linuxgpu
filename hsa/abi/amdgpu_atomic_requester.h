#pragma once
#include <stdint.h>

namespace amdgpu {
namespace atomic_requester {
// Explicit experiment only. This does not qualify the upstream PCIe path,
// change MQD policy or advertise CPU/GPU system atomic support. The runtime
// only validates the driver's snapshot; the PCIe configuration access lives in
// the driver.
constexpr uint16_t kBit=0x40;
enum Field : unsigned { Version,Before,Requested,Observed,Original,Active,RestorePending,Status,Count };
struct Snapshot { uint64_t values[Count]{}; };
inline bool valid(const Snapshot &s) {
    if(s.values[Version]!=1 || s.values[Active]>1 || s.values[RestorePending]>1 ||
       s.values[Status]>UINT32_MAX || (s.values[RestorePending] && !s.values[Active])) return false;
    for(unsigned i=Before;i<=Original;++i)
        if(s.values[i]>UINT16_MAX && s.values[i]!=UINT64_MAX) return false;
    return true;
}
} // namespace atomic_requester
} // namespace amdgpu
