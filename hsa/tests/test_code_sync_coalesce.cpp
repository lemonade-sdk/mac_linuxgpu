// Host unit test: code syncs coalesced (driver build kCodeSyncDriverBuild
// on). Loading code objects only marks the connection's code pending; one
// sync runs before the next doorbell of any of its queues, ahead of that
// doorbell, and nothing more until code is loaded again. A program loading
// dozens of code objects (a new LSE pass width loads 55-60) pays one sync.

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

static hsa_status_t load(hsa_agent_t gpu, std::span<const uint8_t> object, hsa_executable_t &executable) {
    hsa_code_object_reader_t reader{};
    auto status = hsa_code_object_reader_create_from_memory(object.data(), object.size(), &reader);
    if (status != HSA_STATUS_SUCCESS) return status;
    status = hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr, &executable);
    if (status == HSA_STATUS_SUCCESS) status = hsa_executable_load_agent_code_object(executable, gpu, reader, nullptr, nullptr);
    if (status == HSA_STATUS_SUCCESS) status = hsa_executable_freeze(executable, nullptr);
    hsa_code_object_reader_destroy(reader);
    return status;
}

int main() {
    mac_hsa::FakeDeviceConfig config;
    config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 0;
    config.build = mac_hsa::kCodeSyncDriverBuild;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init");
    hsa_agent_t gpu{};
    hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
            type == HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(data) = agent;
        return HSA_STATUS_SUCCESS;
    }, &gpu);
    const auto fake = mac_hsa::fakeConnection();
    mac_hsa::IsaTarget isa;
    mac_hsa::SignalKernelObjects objects;
    CHECK(mac_hsa::resolveIsaTarget(120000, mac_hsa::TargetFeature::Off, mac_hsa::TargetFeature::Any, isa) &&
          mac_hsa::selectSignalKernels(isa, objects) && !objects.operations.empty(), "a code object to load");
    if (failures) return 1;

    hsa_queue_t *queue = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) == HSA_STATUS_SUCCESS,
          "a queue");
    const uint64_t before = fake->codeSyncCount();
    hsa_executable_t executables[8]{};
    bool loaded = true;
    for (auto &executable : executables) loaded &= load(gpu, objects.operations, executable) == HSA_STATUS_SUCCESS;
    CHECK(loaded, "eight code objects load");
    CHECK(fake->codeSyncCount() == before && fake->queueCodeSyncCount() == 0, "no sync at load");

    hsa_signal_store_screlease(queue->doorbell_signal, 0);
    CHECK(fake->codeSyncCount() == before + 1, "one sync for all eight, at the next doorbell");
    CHECK(fake->kicksAtLastCodeSync() == 0, "and ahead of that doorbell");
    hsa_signal_store_screlease(queue->doorbell_signal, 0);
    CHECK(fake->codeSyncCount() == before + 1, "a later doorbell with nothing loaded syncs nothing");

    hsa_executable_t more{};
    CHECK(load(gpu, objects.operations, more) == HSA_STATUS_SUCCESS, "one more load");
    hsa_signal_store_screlease(queue->doorbell_signal, 0);
    CHECK(fake->codeSyncCount() == before + 2 && fake->kicksAtLastCodeSync() == 2,
          "its sync runs at the next doorbell, before it");
    CHECK(fake->queueCodeSyncCount() == 0, "no packet was put in the application's queue");

    for (auto executable : executables) hsa_executable_destroy(executable);
    hsa_executable_destroy(more);
    CHECK(hsa_queue_destroy(queue) == HSA_STATUS_SUCCESS, "queue destroyed");
    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
