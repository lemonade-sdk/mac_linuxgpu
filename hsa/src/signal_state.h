#pragma once
#include "signal_wait_policy.h"
#include <hsa/hsa.h>
#include <hsa/amd_hsa_signal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mac_hsa {
class Connection;
// AMD's 64-byte signal layout (amd_hsa_signal.h). This backing currently
// uses CPU atomics for host-only signals. A GPU-backed signal's word lives in
// coherent (uncached on the GPU) shared memory: a CPU store reaches it as
// ROCr's plain store does on Linux, and loads are acquire observation. Its
// read-modify-writes go through the GPU atomic domain (gpuAtomic): CPU and
// GPU RMW on one word are not qualified on this platform
// (synchronization_policy.h).
struct alignas(64) SignalABI {
    int64_t kind = 1;
    int64_t value = 0;
    uint64_t eventMailbox = 0;
    uint32_t eventID = 0, reserved1 = 0;
    uint64_t startTimestamp = 0, endTimestamp = 0, queue = 0;
    uint32_t reserved3[2]{};
};
static_assert(sizeof(SignalABI) == sizeof(amd_signal_t));
static_assert(alignof(SignalABI) == alignof(amd_signal_t));
static_assert(offsetof(SignalABI, value) == offsetof(amd_signal_t, value));
static_assert(offsetof(SignalABI, startTimestamp) == offsetof(amd_signal_t, start_ts));
static_assert(offsetof(SignalABI, endTimestamp) == offsetof(amd_signal_t, end_ts));
static_assert(offsetof(SignalABI, queue) == offsetof(amd_signal_t, queue_ptr));
static_assert(std::atomic_ref<int64_t>::is_always_lock_free);
struct Signal {
    SignalABI abi;
    SignalABI *sharedABI = nullptr;
    std::shared_ptr<void> sharedStorage;
    std::weak_ptr<Connection> gpuConnection;
    uint64_t ipcToken[4]{};
    std::function<bool(unsigned,int64_t,int64_t,int64_t &)> gpuAtomic;
    std::function<bool()> gpuHealthy;
    std::function<void(int64_t)> storeHook; // Runtime-owned queue doorbell.
    // An interrupt signal: the KFD signal event in abi's event fields.
    bool hasEvent = false;
    uint32_t eventID = 0;
    // Threads asleep in the driver on that event: a host-side change sets
    // the event only while there are any (notifySignal).
    std::atomic<uint32_t> eventSleepers{0};
    std::atomic<bool> alive{true};
    std::mutex waitMutex;
    std::condition_variable changed;
    SignalABI *address() { return sharedABI ? sharedABI : &abi; }
    struct AtomicValue {
        Signal &signal;
        auto host() const { return std::atomic_ref<int64_t>(signal.address()->value); }
        int64_t load(std::memory_order order=std::memory_order_seq_cst) const {
            if (signal.gpuHealthy && !signal.gpuHealthy()) {
                signal.alive=false;signal.changed.notify_all();
            }
            return host().load(order);
        }
        int64_t gpu(unsigned operation,int64_t value,int64_t compare=0) const {
            int64_t result=INT64_MIN;
            if (!signal.alive.load() || !signal.gpuAtomic(operation,value,compare,result)) {
                signal.alive=false;signal.changed.notify_all();return INT64_MIN;
            }
            return result;
        }
        // A store is no read-modify-write: the CPU writes the coherent word
        // (the GPU reads it uncached, after the release that orders it
        // before the doorbell), with no driver call and no GPU work, as on
        // Linux. A store racing a GPU decrement of the same signal is the
        // application's race there too; RMWs below stay GPU-mediated.
        void store(int64_t value,std::memory_order order=std::memory_order_seq_cst) const {
            if (signal.gpuAtomic && !signal.alive.load()) return;
            host().store(value,order);
        }
        int64_t exchange(int64_t value,std::memory_order order=std::memory_order_seq_cst) const {
            return signal.gpuAtomic ? gpu(7,value) : host().exchange(value,order);
        }
        bool compare_exchange_strong(int64_t &expected,int64_t value,std::memory_order success,std::memory_order failure) const {
            if (!signal.gpuAtomic) return host().compare_exchange_strong(expected,value,success,failure);
            const auto old=gpu(8,value,expected);const bool equal=signal.alive.load() && old==expected;
            expected=old;return equal;
        }
#define MAC_HSA_SIGNAL_FETCH(name,operation) \
        int64_t fetch_##name(int64_t value,std::memory_order order=std::memory_order_seq_cst) const { \
            return signal.gpuAtomic ? gpu(operation,value) : host().fetch_##name(value,order); \
        }
        MAC_HSA_SIGNAL_FETCH(add,2)
        MAC_HSA_SIGNAL_FETCH(sub,3)
        MAC_HSA_SIGNAL_FETCH(and,4)
        MAC_HSA_SIGNAL_FETCH(or,5)
        MAC_HSA_SIGNAL_FETCH(xor,6)
#undef MAC_HSA_SIGNAL_FETCH
    };
    AtomicValue value() { return {*this}; }
};
inline bool signalCondition(int64_t value, hsa_signal_condition_t condition, int64_t compare) {
    switch (condition) {
    case HSA_SIGNAL_CONDITION_EQ: return value == compare;
    case HSA_SIGNAL_CONDITION_NE: return value != compare;
    case HSA_SIGNAL_CONDITION_LT: return value < compare;
    case HSA_SIGNAL_CONDITION_GTE: return value >= compare;
    default: return false;
    }
}
// A signal changed from the host: wake its waiters (the condition variable,
// and the driver's event for an interrupt signal with a sleeper).
void notifySignal(Signal &signal);
// Sleep until something may have changed one of @signals, at most @maxNs.
// @satisfied is re-read once the sleep is armed (a change after that wakes
// the sleep); true skips it. gpu_signals.cpp.
void sleepOnSignals(Signal *const *signals, uint32_t count, uint64_t maxNs,
                    const std::function<bool()> &satisfied);

inline int64_t waitSignal(const std::shared_ptr<Signal> &signal,
                         hsa_signal_condition_t condition, int64_t compare,
                         uint64_t timeout, hsa_wait_state_t hint, std::memory_order order) {
    if (!signal) return 0; // invalid-handle operations have undefined HSA behavior
    const auto start = std::chrono::steady_clock::now();
    bool spun = false;
    const auto done = [&] {
        return signalCondition(signal->value().load(order), condition, compare) || !signal->alive.load();
    };
    for (;;) {
        const auto value = signal->value().load(order);
        if (signalCondition(value, condition, compare) || !signal->alive.load() || !timeout)
            return value;
        const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        if (elapsed >= timeout) return value;
        if (hint == HSA_WAIT_STATE_ACTIVE) { std::this_thread::yield(); continue; }
        if (!spun && signalWaitSpinNs()) {
            // A short spin first, for completions expected within microseconds.
            spun = true;
            const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(
                std::min<uint64_t>(signalWaitSpinNs(), timeout - elapsed));
            while (!done() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
            continue;
        }
        // Never hold the runtime lock while asleep.
        Signal *one = signal.get();
        sleepOnSignals(&one, 1, timeout - elapsed, done);
    }
}
}
