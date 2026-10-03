// The driver's device power protocol as the runtime sees it. The driver's
// definition, with the transitions behind each state, is
// dext/sources/power_state.h; scripts/test-power-state.sh checks that the
// two agree.
#pragma once
#include <cstdint>

namespace amdgpu::power {

// QueryInfo tag, PowerControl selector and the snapshot layout.
constexpr uint64_t kQueryTag = 0x4c505752ULL; // "LPWR"
constexpr uint32_t kSelector = 83;
constexpr uint64_t kVersion = 1;
constexpr uint32_t kWords = 12;
enum Word : unsigned {
    Version, State, Generation, Flags, Cause, Error, Holds, Quiesces, Losses,
    LastTransitionMicroseconds, SessionGeneration, Reserved,
};

enum class State : uint32_t { Active = 0, Suspending = 1, Suspended = 2, Resuming = 3, Lost = 4 };

enum Flag : uint32_t {
    VRAMPreserved = 1u << 0,
    SystemSleep   = 1u << 1,
    DeviceLow     = 1u << 2,
    ClientHold    = 1u << 3,
    KFDQuiesced   = 1u << 4,
    SessionClosed = 1u << 5,
    AckPending    = 1u << 6,
    LinkDown      = 1u << 7,
};

enum Op : uint64_t { Query = 0, Prepare = 1, Resume = 2, Wait = 3 };

// IOReturn the driver gives a selector that would put work on the GPU
// while the device is suspending, suspended or resuming (kIOReturnOffline):
// nothing was submitted; retry after resume.
constexpr uint32_t kOfflineIOReturn = 0xe00002d7u;

} // namespace amdgpu::power
