// Host unit test: the runtime adapts to the number of AQL queue slots the
// device reports. With a single slot, public queues take it and GPU-mediated
// signal operations degrade to the CPU-polled path instead of failing; with
// none, the agent is not a dispatch agent and its signals are host signals.

#include "mac_hsa.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", std::string(msg).c_str()); } \
} while (0)

static hsa_agent_t findGPU() {
    hsa_agent_t gpu{};
    hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
            type == HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(data) = agent;
        return HSA_STATUS_SUCCESS;
    }, &gpu);
    return gpu;
}

int main() {
    // 1. One slot.
    {
        mac_hsa::FakeDeviceConfig config;
        config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 0;
        config.queueSlots = 1;
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (one queue slot)");
        const auto gpu = findGPU();
        const auto fake = mac_hsa::fakeConnection();
        uint32_t queuesMax = 0;
        hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUES_MAX, &queuesMax);
        CHECK(queuesMax == 1, "QUEUES_MAX reports the single slot");

        hsa_signal_t signal{};
        CHECK(hsa_signal_create(10, 0, nullptr, &signal) == HSA_STATUS_SUCCESS, "GPU signal create");
        const auto before = fake->aqlDispatchCount();
        hsa_signal_subtract_relaxed(signal, 3);
        CHECK(hsa_signal_load_relaxed(signal) == 7 && fake->aqlDispatchCount() == before + 1,
              "with the slot free, signal RMW runs on the GPU (bounded launch)");

        hsa_queue_t *queue = nullptr;
        CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) == HSA_STATUS_SUCCESS,
              "the first public queue takes the slot");
        hsa_queue_t *second = nullptr;
        CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &second) ==
              HSA_STATUS_ERROR_OUT_OF_RESOURCES && !second, "a second queue fails cleanly with OUT_OF_RESOURCES");
        CHECK(fake->queueCount() == 1, "only one hardware queue exists");

        const auto held = fake->aqlDispatchCount();
        hsa_signal_add_relaxed(signal, 5);
        CHECK(hsa_signal_load_relaxed(signal) == 12, "signal add while the slot is held (CPU-polled path)");
        CHECK(hsa_signal_cas_relaxed(signal, 12, 1) == 12 && hsa_signal_load_relaxed(signal) == 1,
              "signal cas while the slot is held");
        hsa_signal_store_screlease(signal, 0);
        CHECK(hsa_signal_wait_scacquire(signal, HSA_SIGNAL_CONDITION_EQ, 0, 1000000, HSA_WAIT_STATE_BLOCKED) == 0,
              "store + wait while the slot is held");
        CHECK(fake->aqlDispatchCount() == held, "no GPU launch was attempted while every slot was held");

        // The queue keeps working: its doorbell reaches the driver.
        hsa_signal_store_screlease(queue->doorbell_signal, 0);
        CHECK(hsa_queue_destroy(queue) == HSA_STATUS_SUCCESS, "queue destroy releases the slot");
        hsa_signal_add_relaxed(signal, 4);
        CHECK(hsa_signal_load_relaxed(signal) == 4 && fake->aqlDispatchCount() == held + 1,
              "after release, signal RMW returns to the GPU");
        CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) == HSA_STATUS_SUCCESS,
              "the slot can be taken again");
        hsa_queue_destroy(queue);
        hsa_signal_destroy(signal);
        hsa_shut_down();
    }
    // 2. No slots: not a kernel-dispatch agent; signals stay usable on the host.
    {
        mac_hsa::FakeDeviceConfig config;
        config.queueSlots = 0;
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (no queue slots)");
        const auto gpu = findGPU();
        const auto fake = mac_hsa::fakeConnection();
        uint32_t feature = 1, queuesMax = 1;
        hsa_agent_get_info(gpu, HSA_AGENT_INFO_FEATURE, &feature);
        hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUES_MAX, &queuesMax);
        CHECK(feature == 0 && queuesMax == 0, "no kernel-dispatch feature and QUEUES_MAX 0");
        hsa_queue_t *queue = nullptr;
        CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) ==
              HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "queue creation is refused");
        hsa_signal_t signal{};
        CHECK(hsa_signal_create(2, 0, nullptr, &signal) == HSA_STATUS_SUCCESS, "signal create still succeeds");
        hsa_signal_add_relaxed(signal, 3);
        CHECK(hsa_signal_load_relaxed(signal) == 5 && fake->aqlDispatchCount() == 0,
              "signal operations run on the host, no GPU launch");
        hsa_signal_destroy(signal);
        hsa_shut_down();
    }
    // 3. Several slots: queues up to the reported count, then a clean refusal.
    {
        mac_hsa::FakeDeviceConfig config;
        config.queueSlots = 3;
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (three queue slots)");
        const auto gpu = findGPU();
        hsa_queue_t *queues[4]{};
        int created = 0;
        for (auto &queue : queues)
            if (hsa_queue_create(gpu, 128, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) == HSA_STATUS_SUCCESS)
                ++created;
        CHECK(created == 3 && !queues[3], "exactly the reported three queues are created");
        hsa_queue_t *oversized = nullptr;
        CHECK(hsa_queue_create(gpu, 8192, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &oversized) ==
              HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "a ring beyond the reported maximum is refused");
        hsa_queue_t *lds = nullptr;
        CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, config.groupSegmentBytes + 1, &lds) ==
              HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "group segment beyond the reported LDS size is refused");
        for (auto *queue : queues) if (queue) hsa_queue_destroy(queue);
        hsa_shut_down();
    }
    // 4. A KFD-backed session: the client is a KFD process whose queues are
    // MES user queues, so the slot count is KFD's per-process limit, not the
    // one legacy HQD the partition leaves. HRX's two queues per GPU (created
    // at the agent's maximum ring size with default scratch/LDS) coexist with
    // the GPU signal path.
    {
        mac_hsa::FakeDeviceConfig config;
        config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 1;
        config.sessionMode = mac_hsa::ComputeSessionMode::KFD;
        config.queueSlots = 127;
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (KFD session, 127 queue slots)");
        const auto gpu = findGPU();
        const auto fake = mac_hsa::fakeConnection();
        mac_hsa::DeviceSnapshot snapshot{};
        CHECK(fake->read(snapshot) == HSA_STATUS_SUCCESS &&
              snapshot.sessionMode == mac_hsa::ComputeSessionMode::KFD, "the runtime sees the KFD session");
        uint32_t queuesMax = 0, maxSize = 0;
        hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUES_MAX, &queuesMax);
        hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUE_MAX_SIZE, &maxSize);
        CHECK(queuesMax == 127, "QUEUES_MAX reports KFD's per-process limit");
        hsa_signal_t signal{};
        CHECK(hsa_signal_create(1, 0, nullptr, &signal) == HSA_STATUS_SUCCESS, "GPU signal create");
        hsa_queue_t *queues[2]{};
        for (auto &queue : queues)
            CHECK(hsa_queue_create(gpu, maxSize, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr,
                                   UINT32_MAX, UINT32_MAX, &queue) == HSA_STATUS_SUCCESS && queue,
                  "HRX-style queue creation succeeds");
        CHECK(fake->queueCount() == 2, "two hardware queues exist");
        const auto before = fake->aqlDispatchCount();
        hsa_signal_add_relaxed(signal, 2);
        CHECK(hsa_signal_load_relaxed(signal) == 3 && fake->aqlDispatchCount() == before + 1,
              "with both queues alive, signal RMW still runs on the GPU");
        hsa_queue_t *third = nullptr;
        CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &third) ==
              HSA_STATUS_SUCCESS && third, "and further queues fit");
        for (auto *queue : queues) CHECK(hsa_queue_destroy(queue) == HSA_STATUS_SUCCESS, "queue destroy");
        CHECK(hsa_queue_destroy(third) == HSA_STATUS_SUCCESS, "third queue destroy");
        hsa_signal_destroy(signal);
        hsa_shut_down();
    }
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
