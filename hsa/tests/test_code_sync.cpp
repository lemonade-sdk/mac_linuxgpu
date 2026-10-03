// Host unit test: loading a code object while a persistent queue holds the
// device's only AQL queue slot. The driver's code-sync launch (selector 51)
// borrows a slot, so it is refused with OUT_OF_RESOURCES before anything is
// submitted; the loader must then synchronize through an AQL packet on the
// runtime's own queue, and the session must stay healthy.

#include "mac_hsa.h"
#include "signal_kernels.h"
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

// Loads and freezes one executable from an in-memory code object.
static hsa_status_t load(hsa_agent_t gpu, std::span<const uint8_t> object, hsa_executable_t &executable) {
    hsa_code_object_reader_t reader{};
    auto status = hsa_code_object_reader_create_from_memory(object.data(), object.size(), &reader);
    if (status != HSA_STATUS_SUCCESS) return status;
    status = hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr, &executable);
    if (status == HSA_STATUS_SUCCESS)
        status = hsa_executable_load_agent_code_object(executable, gpu, reader, nullptr, nullptr);
    if (status == HSA_STATUS_SUCCESS) status = hsa_executable_freeze(executable, nullptr);
    hsa_code_object_reader_destroy(reader);
    return status;
}

int main() {
    mac_hsa::FakeDeviceConfig config;
    config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 0;
    config.queueSlots = 1;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (one queue slot)");
    const auto gpu = findGPU();
    const auto fake = mac_hsa::fakeConnection();

    // Any code object the agent accepts will do: the runtime's own bundled
    // signal kernels for this ISA.
    mac_hsa::IsaTarget isa;
    CHECK(mac_hsa::resolveIsaTarget(120000, mac_hsa::TargetFeature::Off, mac_hsa::TargetFeature::Any, isa),
          "ISA for the fake GFX12 device");
    mac_hsa::SignalKernelObjects objects;
    CHECK(mac_hsa::selectSignalKernels(isa, objects) && !objects.operations.empty(), "bundled code object");

    // 1. Slot free: the driver's bounded code-sync launch runs.
    hsa_executable_t first{};
    CHECK(load(gpu, objects.operations, first) == HSA_STATUS_SUCCESS, "load with the slot free");
    CHECK(fake->codeSyncCount() == 1 && fake->queueCodeSyncCount() == 0,
          "code-sync used the driver's bounded launch");

    // 2. A persistent queue takes the only slot; loading still succeeds,
    //    synchronized by an AQL packet on that queue.
    hsa_queue_t *queue = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) == HSA_STATUS_SUCCESS,
          "the persistent queue takes the slot");
    CHECK(fake->queueSlotsExhausted(), "every slot is held");
    hsa_executable_t second{};
    CHECK(load(gpu, objects.operations, second) == HSA_STATUS_SUCCESS,
          "load while the queue holds the only slot");
    CHECK(fake->codeSyncCount() == 1 && fake->queueCodeSyncCount() == 1,
          "code-sync ran as an AQL packet on the runtime queue");
    CHECK(hsa_queue_load_write_index_relaxed(queue) == 1 && hsa_queue_load_read_index_relaxed(queue) == 1,
          "the packet was consumed; the queue's indices stay consistent");

    // 3. No session fault: the queue, memory and later loads keep working.
    void *memory = nullptr;
    hsa_amd_memory_pool_t pool{};
    hsa_amd_agent_iterate_memory_pools(gpu, [](hsa_amd_memory_pool_t candidate, void *data) -> hsa_status_t {
        hsa_amd_segment_t segment;
        hsa_amd_memory_pool_get_info(candidate, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment);
        if (segment == HSA_AMD_SEGMENT_GLOBAL) *static_cast<hsa_amd_memory_pool_t *>(data) = candidate;
        return HSA_STATUS_SUCCESS;
    }, &pool);
    CHECK(pool.handle && hsa_amd_memory_pool_allocate(pool, 4096, 0, &memory) == HSA_STATUS_SUCCESS,
          "device memory still allocates");
    if (memory) hsa_amd_memory_pool_free(memory);
    hsa_executable_t third{};
    CHECK(load(gpu, objects.operations, third) == HSA_STATUS_SUCCESS && fake->queueCodeSyncCount() == 2,
          "a further load synchronizes on the queue again");
    CHECK(hsa_queue_destroy(queue) == HSA_STATUS_SUCCESS, "the queue is destroyed cleanly");
    hsa_executable_t fourth{};
    CHECK(load(gpu, objects.operations, fourth) == HSA_STATUS_SUCCESS && fake->codeSyncCount() == 2,
          "with the slot free again, the driver's launch is used");

    for (auto executable : {first, second, third, fourth})
        CHECK(hsa_executable_destroy(executable) == HSA_STATUS_SUCCESS, "executable destroyed");
    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
