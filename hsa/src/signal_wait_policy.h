#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace mac_hsa {

// How a blocked wait sleeps (signal_state.h, sleepOnSignals in
// gpu_signals.cpp).
//
// A GPU signal that carries a KFD signal event (an interrupt signal) is
// waited for in the driver: the thread sleeps until the command
// processor's completion interrupt signals the event, a host-side change
// sets it, or the backstop passes. The backstop
// (MAC_HSA_WAIT_BACKSTOP_US, 100 us to 1 s, default 4 ms) re-reads the
// value and re-arms the wait, so a lost interrupt costs at most one
// backstop. A host signal sleeps on its condition variable (every store
// notifies it) under the same backstop. A blocked wait first polls for
// MAC_HSA_WAIT_SPIN_US (0 to 1000, default 200; 0 sleeps at once), as
// ROCr's InterruptSignal waits do (kMaxElapsed, 200 us): a completion
// expected within microseconds is seen as soon as an active wait would
// see it, and only a longer wait pays the interrupt's wake-up.
//
// Only a GPU signal without an event (the legacy session path, a driver
// before build 235; the runtime reports why once) and a wait over mixed
// signals poll, every MAC_HSA_BLOCKED_POLL_US (10 to 1000, default 32).
inline uint64_t microsecondsSetting(const char *setting, uint32_t fallback, uint32_t low, uint32_t high) {
    if (!setting) return uint64_t(fallback) * 1000;
    const std::string_view text(setting);
    uint32_t micros = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), micros);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        micros < low || micros > high) return uint64_t(fallback) * 1000;
    return uint64_t(micros) * 1000;
}

inline uint64_t blockedSignalPollNs(const char *setting) { return microsecondsSetting(setting, 32, 10, 1000); }
inline uint64_t signalWaitBackstopNs(const char *setting) { return microsecondsSetting(setting, 4000, 100, 1000000); }
inline uint64_t signalWaitSpinNs(const char *setting) { return microsecondsSetting(setting, 200, 0, 1000); }

inline uint64_t blockedSignalPollNs() {
    static const uint64_t interval = blockedSignalPollNs(std::getenv("MAC_HSA_BLOCKED_POLL_US"));
    return interval;
}
inline uint64_t signalWaitBackstopNs() {
    static const uint64_t interval = signalWaitBackstopNs(std::getenv("MAC_HSA_WAIT_BACKSTOP_US"));
    return interval;
}
inline uint64_t signalWaitSpinNs() {
    static const uint64_t interval = signalWaitSpinNs(std::getenv("MAC_HSA_WAIT_SPIN_US"));
    return interval;
}

} // namespace mac_hsa
