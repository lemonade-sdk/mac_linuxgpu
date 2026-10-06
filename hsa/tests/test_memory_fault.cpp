// Host unit test: a GPU memory fault of the process's GPU work, as the
// driver reports it from kQueueFaultDriverBuild on (KFD evicted every queue
// of the process). The runtime notices it on the queue service it already
// polls, says it once in ROCr's words, hands HSA_AMD_GPU_MEMORY_FAULT_EVENT
// to the registered system event handler with the address and reason, and
// gives each queue's error callback HSA_STATUS_ERROR_MEMORY_FAULT. A kick
// after the fault gets the same status.

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

static std::atomic<int> callbacks{0};
static std::atomic<unsigned> lastStatus{0};
static std::atomic<int> events{0};
static hsa_amd_gpu_memory_fault_info_t eventInfo{};

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
    CHECK(gpu.handle && fake, "GPU agent on the fake device");
    CHECK(hsa_amd_register_system_event_handler([](const hsa_amd_event_t *event, void *) -> hsa_status_t {
        if (event->event_type == HSA_AMD_GPU_MEMORY_FAULT_EVENT) { eventInfo = event->memory_fault; ++events; }
        return HSA_STATUS_SUCCESS;
    }, nullptr) == HSA_STATUS_SUCCESS, "system event handler registered");

    const auto callback = [](hsa_status_t status, hsa_queue_t *, void *) { lastStatus = status; ++callbacks; };
    hsa_queue_t *a = nullptr, *b = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, callback, nullptr, 0, 0, &a) == HSA_STATUS_SUCCESS &&
          hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, callback, nullptr, 0, 0, &b) == HSA_STATUS_SUCCESS,
          "two queues");
    if (failures) return 1;

    fake->injectMemoryFault(0x123450000ull, HSA_AMD_MEMORY_FAULT_PAGE_NOT_PRESENT);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callbacks.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(callbacks.load() == 2, "each queue's error callback ran once, from the queue service poll");
    CHECK(lastStatus.load() == HSA_STATUS_ERROR_MEMORY_FAULT, "with HSA_STATUS_ERROR_MEMORY_FAULT");
    CHECK(events.load() == 1, "one memory fault event for the process");
    CHECK(eventInfo.agent.handle == gpu.handle && eventInfo.virtual_address == 0x123450000ull &&
          eventInfo.fault_reason_mask == HSA_AMD_MEMORY_FAULT_PAGE_NOT_PRESENT,
          "the event names the agent, the address and the reason");

    // A queue created after the fault gets the same answer on its first kick.
    hsa_queue_t *c = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, callback, nullptr, 0, 0, &c) == HSA_STATUS_SUCCESS,
          "a third queue");
    if (c) {
        callbacks = 0;
        hsa_signal_store_screlease(c->doorbell_signal, 0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!callbacks.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(callbacks.load() == 1 && lastStatus.load() == HSA_STATUS_ERROR_MEMORY_FAULT,
              "its kick reports the fault");
        CHECK(events.load() == 1, "and the event is not repeated");
    }
    for (auto *q : {a, b, c}) if (q) (void)hsa_queue_destroy(q);
    hsa_shut_down();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::fprintf(stderr, "all memory fault checks passed\n");
    return 0;
}
