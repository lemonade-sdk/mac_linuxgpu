#pragma once
// Device power as the runtime handles it (driver protocol:
// abi/amdgpu_power_abi.h; driver side: dext/sources/power_state.h).
//
// While the driver reports the device suspending, suspended or resuming it
// refuses GPU work with kIOReturnOffline, which transports return as
// kDeviceSuspendedStatus: nothing was submitted, retry after resume. Queue
// doorbells refused that way are held by their queue and replayed once the
// device takes work again, so a generation in flight pauses and continues.
// Once the device lost its memory (a host sleep closed the session) the
// session's calls fail; queue errors and the power API then report
// kDeviceLostStatus, and the client reloads: hsa_shut_down, hsa_init (the
// next initialization probes the device afresh), then its buffers again.
#include "../abi/amdgpu_power_abi.h"
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <array>
#include <cstdint>

namespace mac_hsa {

constexpr hsa_status_t kDeviceSuspendedStatus = hsa_status_t(HSA_STATUS_ERROR_RESOURCE_BUSY);
constexpr hsa_status_t kDeviceLostStatus = HSA_STATUS_ERROR_FATAL;
// The process's GPU work faulted (transport.h MemoryFault): KFD evicted its queues.
constexpr hsa_status_t kMemoryFaultStatus = hsa_status_t(HSA_STATUS_ERROR_MEMORY_FAULT);

struct PowerSnapshot {
    std::array<uint64_t, amdgpu::power::kWords> words{};
    bool valid() const {
        return words[amdgpu::power::Version] == amdgpu::power::kVersion &&
            words[amdgpu::power::State] <= uint64_t(amdgpu::power::PowerState::Lost);
    }
    amdgpu::power::PowerState state() const { return amdgpu::power::PowerState(uint32_t(words[amdgpu::power::State])); }
    uint64_t generation() const { return words[amdgpu::power::Generation]; }
    uint32_t flags() const { return uint32_t(words[amdgpu::power::Flags]); }
    bool lost() const { return state() == amdgpu::power::PowerState::Lost; }
    bool takingWork() const { return state() == amdgpu::power::PowerState::Active; }
};

} // namespace mac_hsa
