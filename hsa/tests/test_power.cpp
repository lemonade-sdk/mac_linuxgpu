// Host unit test: device power (mac_hsa_power.h) against the fake backend's
// power model (the driver's power state: dext/sources/power_state.h).
//
// While the device is suspended, a queue's doorbells wait in the runtime
// (no kick reaches the driver, no queue error) and are rung once the device
// takes work again, either by the queue's service noticing or by
// mac_hsa_agent_resume; new GPU work fails with MAC_HSA_STATUS_SUSPENDED
// and succeeds after resume; a client's prepare holds doorbells back before
// asking the driver; and a device that lost its memory reports
// MAC_HSA_STATUS_DEVICE_LOST to the queue's error callback and the API.

#include "mac_hsa.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

namespace pw = amdgpu::power;

static std::atomic<int> queueErrors{0};
static std::atomic<hsa_status_t> lastQueueError{HSA_STATUS_SUCCESS};
static void onQueueError(hsa_status_t status, hsa_queue_t *, void *) {
    lastQueueError = status;
    ++queueErrors;
}

template<typename F> static bool eventually(F &&condition, int milliseconds = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

int main() {
    if (hsa_init() != HSA_STATUS_SUCCESS) { std::fprintf(stderr, "FAIL: hsa_init\n"); return 1; }
    hsa_agent_t gpu{};
    hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
            type == HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(data) = agent;
        return HSA_STATUS_SUCCESS;
    }, &gpu);
    auto fake = mac_hsa::fakeConnection();
    CHECK(gpu.handle && fake, "GPU agent on the fake backend");
    if (!gpu.handle || !fake) { hsa_shut_down(); return 1; }
    hsa_amd_memory_pool_t pool{};
    hsa_amd_agent_iterate_memory_pools(gpu, [](hsa_amd_memory_pool_t candidate, void *data) -> hsa_status_t {
        hsa_amd_segment_t segment;
        if (hsa_amd_memory_pool_get_info(candidate, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment) ==
                HSA_STATUS_SUCCESS && segment == HSA_AMD_SEGMENT_GLOBAL &&
            !static_cast<hsa_amd_memory_pool_t *>(data)->handle)
            *static_cast<hsa_amd_memory_pool_t *>(data) = candidate;
        return HSA_STATUS_SUCCESS;
    }, &pool);
    CHECK(pool.handle != 0, "GPU memory pool");

    // ---- the initial state ----
    mac_hsa_power_state_t state{};
    CHECK(mac_hsa_agent_get_power_state(gpu, &state, sizeof(state)) == HSA_STATUS_SUCCESS, "get power state");
    CHECK(state.version == 1 && state.state == MAC_HSA_POWER_ACTIVE && state.generation == 1,
          "active, generation 1");
    CHECK(state.flags == MAC_HSA_POWER_FLAG_VRAM_PRESERVED && !state.paused_queues, "VRAM preserved, nothing paused");
    CHECK(mac_hsa_agent_get_power_state(gpu, &state, sizeof(state) - 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT,
          "wrong size rejected");

    hsa_queue_t *queue = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, onQueueError, nullptr, 0, 0, &queue) ==
          HSA_STATUS_SUCCESS && queue, "queue created");
    if (!queue) { hsa_shut_down(); return 1; }
    const uint64_t handle = 1; // the fake's first hardware queue
    hsa_signal_store_relaxed(queue->doorbell_signal, 3);
    CHECK(fake->kickCount(handle) == 1, "a doorbell kicks while active");

    // ---- the device suspended under the runtime (device low power) ----
    fake->setPowerState(pw::PowerState::Suspended);
    hsa_signal_store_relaxed(queue->doorbell_signal, 4);
    hsa_signal_store_relaxed(queue->doorbell_signal, 5);
    CHECK(fake->kickCount(handle) == 1, "doorbells wait while suspended");
    CHECK(queueErrors == 0, "no queue error while suspended");
    CHECK(mac_hsa_agent_get_power_state(gpu, &state, sizeof(state)) == HSA_STATUS_SUCCESS &&
          state.state == MAC_HSA_POWER_SUSPENDED && state.generation == 2, "suspended, generation 2");
    CHECK((state.flags & MAC_HSA_POWER_FLAG_VRAM_PRESERVED) && (state.flags & MAC_HSA_POWER_FLAG_QUIESCED),
          "suspended with VRAM preserved");
    CHECK(state.paused_queues == 1, "one queue holds a doorbell");
    void *buffer = nullptr;
    CHECK(hsa_amd_memory_pool_allocate(pool, 16384, 0, &buffer) == MAC_HSA_STATUS_SUSPENDED && !buffer,
          "an allocation is refused as suspended, retryable");
    hsa_queue_t *second = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &second) ==
          MAC_HSA_STATUS_SUSPENDED && !second, "a queue creation is refused as suspended");
    // The service keeps polling without raising an error.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(queueErrors == 0 && fake->kickCount(handle) == 1, "service paused, still no error");

    // ---- the device takes work again: the service rings what waited ----
    fake->setPowerState(pw::PowerState::Active);
    CHECK(eventually([&] { return fake->kickCount(handle) == 2; }), "the waiting doorbell is rung once");
    CHECK(eventually([&] {
        return mac_hsa_agent_get_power_state(gpu, &state, sizeof(state)) == HSA_STATUS_SUCCESS &&
            state.paused_queues == 0;
    }), "no queue paused after resume");
    CHECK(hsa_amd_memory_pool_allocate(pool, 16384, 0, &buffer) == HSA_STATUS_SUCCESS && buffer,
          "the allocation succeeds after resume");
    if (buffer) hsa_amd_memory_pool_free(buffer);
    hsa_signal_store_relaxed(queue->doorbell_signal, 6);
    CHECK(fake->kickCount(handle) == 3, "new doorbells kick again");

    // ---- a client prepares for low power and resumes ----
    const auto before = std::chrono::steady_clock::now();
    CHECK(mac_hsa_agent_prepare_low_power(gpu, 50, &state, sizeof(state)) == HSA_STATUS_SUCCESS,
          "prepare for low power");
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2), "the drain is bounded");
    CHECK(state.state == MAC_HSA_POWER_SUSPENDED && state.holds == 1 && fake->powerRequests(pw::Prepare) == 1,
          "the driver was asked and holds the device suspended");
    hsa_signal_store_relaxed(queue->doorbell_signal, 7);
    CHECK(fake->kickCount(handle) == 3, "a doorbell after prepare waits");
    CHECK(mac_hsa_agent_resume(gpu, &state, sizeof(state)) == HSA_STATUS_SUCCESS, "resume");
    CHECK(state.state == MAC_HSA_POWER_ACTIVE && !state.holds && fake->powerRequests(pw::Resume) == 1,
          "active again, no hold");
    CHECK(fake->kickCount(handle) == 4, "resume rings the doorbell that waited");

    // ---- waiting for a change ----
    mac_hsa_power_state_t waited{};
    const uint64_t generation = state.generation;
    CHECK(mac_hsa_agent_wait_power_state(gpu, generation, 30, &waited, sizeof(waited)) == HSA_STATUS_SUCCESS &&
          waited.generation == generation, "a wait without a change times out unchanged");
    std::thread changer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fake->setPowerState(pw::PowerState::Suspended);
    });
    CHECK(mac_hsa_agent_wait_power_state(gpu, generation, 2000, &waited, sizeof(waited)) == HSA_STATUS_SUCCESS &&
          waited.generation != generation && waited.state == MAC_HSA_POWER_SUSPENDED, "a wait sees the change");
    changer.join();
    fake->setPowerState(pw::PowerState::Active);

    // ---- the device lost its memory (host sleep closed the session) ----
    fake->setPowerState(pw::PowerState::Lost);
    hsa_signal_store_relaxed(queue->doorbell_signal, 8);
    CHECK(eventually([] { return queueErrors.load() == 1; }), "the queue reports an error once");
    CHECK(lastQueueError == MAC_HSA_STATUS_DEVICE_LOST, "the error is DEVICE_LOST");
    CHECK(mac_hsa_agent_get_power_state(gpu, &state, sizeof(state)) == HSA_STATUS_SUCCESS &&
          state.state == MAC_HSA_POWER_LOST && !(state.flags & MAC_HSA_POWER_FLAG_VRAM_PRESERVED) &&
          state.losses == 1, "lost, VRAM not preserved");
    CHECK(mac_hsa_agent_resume(gpu, &state, sizeof(state)) == MAC_HSA_STATUS_DEVICE_LOST,
          "resume reports DEVICE_LOST");
    CHECK(hsa_queue_destroy(queue) == HSA_STATUS_SUCCESS, "the queue still destroys");

    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
