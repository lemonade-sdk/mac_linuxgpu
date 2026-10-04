// Host unit test: interrupt-driven signal waits (signal_wait_policy.h).
//
// Against the fake backend serving KFD signal events: a GPU signal carries
// an event in its amd_signal_t; a blocked wait sleeps in the driver until
// the command processor's completion interrupt (the fake's completeSignal)
// or a host-side store sets the event; a lost interrupt ends at the
// backstop; an idle wait wakes once per backstop and never polls; a device
// without events says so and polls, as before.

#include "mac_hsa.h"
#include "transport_fake.h"
#include "signal_state.h"
#include <hsa/hsa_ext_amd.h>
#include <hsa/amd_hsa_signal.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static double msBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}
static constexpr uint64_t kSecond = 1000000000ull;

static hsa_agent_t findAgent(hsa_device_type_t want) {
    struct Find { hsa_device_type_t want; hsa_agent_t agent{}; } find{want};
    hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        auto *f = static_cast<Find *>(data);
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
            type == f->want) f->agent = agent;
        return HSA_STATUS_SUCCESS;
    }, &find);
    return find.agent;
}

// A blocked wait for @signal to reach 0, on its own thread.
struct Waiter {
    std::thread thread;
    Clock::time_point returned;
    hsa_signal_value_t value = -99;
    void start(hsa_signal_t signal, uint64_t timeout = 5 * kSecond) {
        thread = std::thread([this, signal, timeout] {
            value = hsa_signal_wait_scacquire(signal, HSA_SIGNAL_CONDITION_EQ, 0, timeout,
                                              HSA_WAIT_STATE_BLOCKED);
            returned = Clock::now();
        });
    }
};

int main() {
    // The backstop long enough that a wake inside it is the interrupt's.
    setenv("MAC_HSA_WAIT_BACKSTOP_US", "200000", 1);
    mac_hsa::FakeDeviceConfig config;
    config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 1;
    config.sessionMode = mac_hsa::ComputeSessionMode::KFD;
    config.signalEvents = true;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (KFD session with signal events)");
    const auto fake = mac_hsa::fakeConnection();

    hsa_signal_t signal{};
    CHECK(hsa_signal_create(1, 0, nullptr, &signal) == HSA_STATUS_SUCCESS, "GPU signal create");
    const auto *abi = reinterpret_cast<const amd_signal_t *>(signal.handle);
    CHECK(abi->event_mailbox_ptr && abi->event_id, "the signal is an interrupt signal (mailbox, event id)");
    CHECK(fake->eventStats().created == 1, "one KFD signal event per signal");

    // 1. The completion interrupt wakes the waiter.
    {
        Waiter w;
        w.start(signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto before = fake->eventStats();
        const auto fired = Clock::now();
        fake->completeSignal(signal.handle);
        w.thread.join();
        const double latency = msBetween(fired, w.returned);
        const auto after = fake->eventStats();
        std::fprintf(stderr, "  completion interrupt woke the waiter in %.2f ms (%llu driver waits)\n",
                     latency, (unsigned long long)(after.waits - before.waits + 1));
        CHECK(w.value == 0 && latency < 50, "the interrupt, not the 200 ms backstop, ended the wait");
        CHECK(after.interrupts == before.interrupts + 1 &&
              *reinterpret_cast<const uint64_t *>(abi->event_mailbox_ptr) == abi->event_id,
              "the CP wrote the mailbox and raised the interrupt");
    }

    // 2. A lost interrupt: the backstop re-reads the value.
    {
        hsa_signal_store_screlease(signal, 1);
        fake->dropInterrupts(true);
        Waiter w;
        w.start(signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto completed = Clock::now();
        fake->completeSignal(signal.handle);
        w.thread.join();
        fake->dropInterrupts(false);
        const double latency = msBetween(completed, w.returned);
        std::fprintf(stderr, "  lost interrupt: the backstop found the completion after %.1f ms\n", latency);
        CHECK(w.value == 0 && latency < 400 && fake->eventStats().dropped == 1,
              "a dropped interrupt costs at most one backstop");
    }

    // 3. An idle wait sleeps a backstop at a time and never polls.
    {
        hsa_signal_store_screlease(signal, 1);
        const auto before = fake->eventStats();
        const auto start = Clock::now();
        const auto value = hsa_signal_wait_scacquire(signal, HSA_SIGNAL_CONDITION_EQ, 0, kSecond,
                                                     HSA_WAIT_STATE_BLOCKED);
        const double took = msSince(start);
        const auto after = fake->eventStats();
        const auto sleeps = after.waits - before.waits;
        std::fprintf(stderr, "  idle 1 s wait: %llu driver sleeps (%llu timed out), %.0f ms\n",
                     (unsigned long long)sleeps, (unsigned long long)(after.timedOutWaits - before.timedOutWaits),
                     took);
        CHECK(value == 1 && took >= 990, "the wait timed out at its timeout");
        CHECK(sleeps >= 4 && sleeps <= 7, "about one sleep per 200 ms backstop (a 32 us poll would be ~31000)");
    }

    // 4. A host store wakes a sleeper through the event.
    {
        Waiter w;
        w.start(signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto sets = fake->eventStats().sets;
        const auto stored = Clock::now();
        hsa_signal_store_screlease(signal, 0);
        w.thread.join();
        const double latency = msBetween(stored, w.returned);
        CHECK(w.value == 0 && latency < 50 && fake->eventStats().sets == sets + 1,
              "hsa_signal_store sets the event of a signal with a sleeper");
        const auto quiet = fake->eventStats().sets;
        hsa_signal_store_screlease(signal, 5);
        CHECK(fake->eventStats().sets == quiet, "a store with no sleeper makes no driver call");
    }

    // 5. wait_any over two interrupt signals: the second one's completion.
    {
        hsa_signal_t other{};
        CHECK(hsa_signal_create(1, 0, nullptr, &other) == HSA_STATUS_SUCCESS, "second GPU signal");
        hsa_signal_store_screlease(signal, 1);
        hsa_signal_t both[2] = {signal, other};
        hsa_signal_condition_t conds[2] = {HSA_SIGNAL_CONDITION_EQ, HSA_SIGNAL_CONDITION_EQ};
        hsa_signal_value_t vals[2] = {0, 0};
        uint32_t index = UINT32_MAX;
        Clock::time_point returned;
        std::thread t([&] {
            index = hsa_amd_signal_wait_any(2, both, conds, vals, 5 * kSecond, HSA_WAIT_STATE_BLOCKED, nullptr);
            returned = Clock::now();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto fired = Clock::now();
        fake->completeSignal(other.handle);
        t.join();
        CHECK(index == 1 && msBetween(fired, returned) < 50,
              "wait_any sleeps on both events and wakes for the second");
        hsa_signal_destroy(other);
    }

    // 6. Destroying a signal under a wait ends the wait.
    {
        hsa_signal_t doomed{};
        CHECK(hsa_signal_create(1, 0, nullptr, &doomed) == HSA_STATUS_SUCCESS, "signal to destroy");
        Waiter w;
        w.start(doomed);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto destroyed = Clock::now();
        hsa_signal_destroy(doomed);
        w.thread.join();
        CHECK(msBetween(destroyed, w.returned) < 50,
              "the waiter returned when the signal was destroyed");
    }

    // 7. A host (CPU-only) signal: the store's notification wakes it.
    {
        const auto cpu = findAgent(HSA_DEVICE_TYPE_CPU);
        hsa_signal_t host{};
        CHECK(hsa_signal_create(1, 1, &cpu, &host) == HSA_STATUS_SUCCESS, "host signal create");
        CHECK(!reinterpret_cast<const amd_signal_t *>(host.handle)->event_mailbox_ptr, "a host signal has no event");
        Waiter w;
        w.start(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto stored = Clock::now();
        hsa_signal_store_relaxed(host, 0);
        w.thread.join();
        CHECK(w.value == 0 && msBetween(stored, w.returned) < 50,
              "a store wakes a host-signal waiter at once");
        hsa_signal_store_relaxed(host, 1);
        Waiter s;
        s.start(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto silent = Clock::now();
        hsa_signal_silent_store_relaxed(host, 0);
        s.thread.join();
        CHECK(s.value == 0 && msBetween(silent, s.returned) < 400,
              "a silent store is found at the backstop");
        hsa_signal_destroy(host);
    }

    hsa_signal_destroy(signal);
    CHECK(fake->eventStats().live == 0, "every event was destroyed with its signal");
    CHECK(hsa_shut_down() == HSA_STATUS_SUCCESS, "hsa_shut_down");

    // 8. No signal events (the legacy path): the signal has none, the
    // runtime says so, and waits still complete.
    {
        mac_hsa::FakeDeviceConfig legacy;
        legacy.gcMajor = 12; legacy.gcMinor = 0; legacy.gcRevision = 1;
        mac_hsa::setFakeDeviceConfig(legacy);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (legacy session)");
        const auto fake2 = mac_hsa::fakeConnection();
        hsa_signal_t plain{};
        CHECK(hsa_signal_create(1, 0, nullptr, &plain) == HSA_STATUS_SUCCESS, "GPU signal create (legacy)");
        CHECK(!reinterpret_cast<const amd_signal_t *>(plain.handle)->event_mailbox_ptr,
              "no event without driver support");
        Waiter w;
        w.start(plain);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fake2->completeSignal(plain.handle);
        w.thread.join();
        CHECK(w.value == 0, "the polling wait sees the completion");
        hsa_signal_destroy(plain);
        CHECK(hsa_shut_down() == HSA_STATUS_SUCCESS, "hsa_shut_down (legacy)");
    }

    std::fprintf(stderr, failures ? "signal wait: %d FAILED\n" : "signal wait: all passed\n", failures);
    return failures ? 1 : 0;
}
