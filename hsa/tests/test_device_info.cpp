// Host unit test: agent and ISA information come from what the device
// reports. Runs the public HSA API against fake devices of different
// generations (none of them the development board) and checks every value
// against the fake's QueryInfo payloads.

#include "mac_hsa.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
template<class T> static T info(hsa_agent_t agent, uint32_t attribute, hsa_status_t *status = nullptr) {
    T value{};
    const auto result = hsa_agent_get_info(agent, hsa_agent_info_t(attribute), &value);
    if (status) *status = result;
    return value;
}
static std::string text(hsa_agent_t agent, uint32_t attribute) {
    char value[64]{};
    hsa_agent_get_info(agent, hsa_agent_info_t(attribute), value);
    return value;
}
static std::vector<std::string> isaNames(hsa_agent_t agent) {
    std::vector<std::string> names;
    hsa_agent_iterate_isas(agent, [](hsa_isa_t isa, void *data) -> hsa_status_t {
        uint32_t length = 0;
        if (hsa_isa_get_info_alt(isa, HSA_ISA_INFO_NAME_LENGTH, &length) != HSA_STATUS_SUCCESS) return HSA_STATUS_ERROR;
        std::string name(length, '\0');
        if (hsa_isa_get_info_alt(isa, HSA_ISA_INFO_NAME, name.data()) != HSA_STATUS_SUCCESS) return HSA_STATUS_ERROR;
        name.resize(length ? length - 1 : 0); // NAME_LENGTH includes the NUL (ROCr ABI)
        static_cast<std::vector<std::string> *>(data)->push_back(name);
        return HSA_STATUS_SUCCESS;
    }, &names);
    return names;
}

int main() {
    // 1. A wave64 GFX9 data-center part with a full topology report.
    {
        mac_hsa::FakeDeviceConfig config;
        config.gcMajor = 9; config.gcMinor = 4; config.gcRevision = 2; // gfx90a
        config.chipID = 0x0f0f; config.revision = 0x02;
        config.computeUnits = 104; config.shaderEngines = 8; config.arraysPerEngine = 1;
        config.wavefrontSize = 64; config.simdPerCU = 4; config.maxWavesPerSIMD = 8;
        config.timestampFrequency = 25000000;
        config.l1CacheBytes = 16384; config.l2CacheBytes = 8u << 20; config.l3CacheBytes = 0;
        config.queueSlots = 3; config.queueMaxPackets = 2048; config.maxClockMHz = 1700; config.xccCount = 1;
        config.sramecc = mac_hsa::TargetFeature::On;
        config.productName = "Fake Instinct Accelerator";
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (gfx90a fake)");
        const auto gpu = findGPU();
        CHECK(gpu.handle, "GPU agent published");
        CHECK(text(gpu, HSA_AGENT_INFO_NAME) == "gfx90a", "agent name is the processor: " + text(gpu, HSA_AGENT_INFO_NAME));
        CHECK(text(gpu, HSA_AMD_AGENT_INFO_PRODUCT_NAME) == config.productName, "product name is the reported board name");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_CHIP_ID) == 0x0f0f, "chip id");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_ASIC_REVISION) == 0x02, "asic revision");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT) == 104, "compute units");
        // Queues are consumed by the GPU's own command processor, and the GPU
        // is a discrete device: no PM4 emulation, no APU memory property.
        hsa_status_t queried = HSA_STATUS_ERROR;
        CHECK(!info<bool>(gpu, HSA_AMD_AGENT_INFO_PM4_EMULATION, &queried) && queried == HSA_STATUS_SUCCESS,
              "AQL queues are not PM4-emulated");
        uint8_t memoryProperties[8];
        std::memset(memoryProperties, 0xff, sizeof(memoryProperties));
        CHECK(hsa_agent_get_info(gpu, hsa_agent_info_t(HSA_AMD_AGENT_INFO_MEMORY_PROPERTIES), memoryProperties) ==
                  HSA_STATUS_SUCCESS && !(memoryProperties[0] & HSA_AMD_MEMORY_PROPERTY_AGENT_IS_APU),
              "memory properties report a discrete GPU");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_WAVEFRONT_SIZE) == 64, "wavefront size 64");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU) == 4, "SIMDs per CU");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_MAX_WAVES_PER_CU) == 32, "max waves per CU = SIMDs x waves/SIMD");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_NUM_SHADER_ENGINES) == 8, "shader engines");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_NUM_SHADER_ARRAYS_PER_SE) == 1, "shader arrays per SE");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY) == 1700, "max clock");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_NUM_XCC) == 1, "XCC count");
        CHECK(info<uint64_t>(gpu, HSA_AMD_AGENT_INFO_TIMESTAMP_FREQUENCY) == 25000000, "timestamp frequency");
        const auto caches = info<std::array<uint32_t, 4>>(gpu, HSA_AGENT_INFO_CACHE_SIZE);
        CHECK(caches == (std::array<uint32_t, 4>{16384, 8u << 20, 0, 0}), "cache sizes L1/L2/L3");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_QUEUES_MAX) == 3, "queues max = reported queue slots");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_QUEUE_MIN_SIZE) == 64, "queue min size");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_QUEUE_MAX_SIZE) == 2048, "queue max size = reported ring bound");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_FEATURE) == HSA_AGENT_FEATURE_KERNEL_DISPATCH, "kernel dispatch agent");
        // The deprecated uint32 GRID_MAX_SIZE must not write past 4 bytes.
        uint32_t grid[2] = {0, 0xa5a5a5a5u};
        CHECK(hsa_agent_get_info(gpu, HSA_AGENT_INFO_GRID_MAX_SIZE, grid) == HSA_STATUS_SUCCESS &&
              grid[0] == UINT32_MAX && grid[1] == 0xa5a5a5a5u, "GRID_MAX_SIZE writes exactly a uint32_t");
        const auto names = isaNames(gpu);
        CHECK(names == (std::vector<std::string>{"amdgcn-amd-amdhsa--gfx90a:sramecc+:xnack-"}),
              "gfx90a has one ISA (no generic family), with device-reported sramecc");
        hsa_isa_t isa{};
        CHECK(hsa_agent_get_info(gpu, HSA_AGENT_INFO_ISA, &isa) == HSA_STATUS_SUCCESS && isa.handle, "agent ISA handle");
        // Signals run the gfx90a (wave64) signal kernel.
        hsa_signal_t signal{};
        CHECK(hsa_signal_create(5, 0, nullptr, &signal) == HSA_STATUS_SUCCESS, "GPU signal on gfx90a");
        hsa_signal_add_relaxed(signal, 2);
        CHECK(hsa_signal_load_relaxed(signal) == 7, "signal add runs");
        hsa_signal_destroy(signal);
        hsa_shut_down();
    }
    // 2. A wave32 RDNA2 part behind a driver that predates the topology tag.
    {
        mac_hsa::FakeDeviceConfig config;
        config.gcMajor = 10; config.gcMinor = 3; config.gcRevision = 0; // gfx1030
        config.computeUnits = 72; config.wavefrontSize = 32;
        config.topologyReported = false; config.productName.clear();
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (gfx1030 fake, no topology tag)");
        const auto gpu = findGPU();
        CHECK(text(gpu, HSA_AGENT_INFO_NAME) == "gfx1030", "processor from KFD's GC 10.3.0 mapping");
        CHECK(text(gpu, HSA_AMD_AGENT_INFO_PRODUCT_NAME) == "gfx1030", "no board name: processor name, nothing invented");
        CHECK(info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT) == 72, "compute units");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_WAVEFRONT_SIZE) == 32, "wavefront size 32");
        hsa_status_t status = HSA_STATUS_SUCCESS;
        (void)info<uint32_t>(gpu, HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU, &status);
        CHECK(status == HSA_STATUS_ERROR_INVALID_ARGUMENT, "unreported SIMD count declines");
        CHECK(info<uint32_t>(gpu, HSA_AGENT_INFO_QUEUES_MAX) == 1,
              "unreported queue slots: the one slot a ready driver guarantees");
        const auto unreported = info<std::array<uint32_t, 4>>(gpu, HSA_AGENT_INFO_CACHE_SIZE);
        CHECK(unreported == (std::array<uint32_t, 4>{}), "unreported caches are 0 (no information)");
        CHECK(isaNames(gpu) == (std::vector<std::string>{"amdgcn-amd-amdhsa--gfx1030",
                                                         "amdgcn-amd-amdhsa--gfx10-3-generic"}),
              "specific ISA first, then its generic family");
        hsa_signal_t signal{};
        CHECK(hsa_signal_create(1, 0, nullptr, &signal) == HSA_STATUS_SUCCESS, "GPU signal on gfx1030");
        CHECK(hsa_signal_exchange_relaxed(signal, 9) == 1 && hsa_signal_load_relaxed(signal) == 9,
              "signal exchange via the gfx10-3-generic kernel");
        hsa_signal_destroy(signal);
        hsa_shut_down();
    }
    // 3. A driver-reported gfx_target_version wins over the GC mapping, and
    //    a device that names no known ISA is not published.
    {
        mac_hsa::FakeDeviceConfig config;
        config.gcMajor = 11; config.gcMinor = 0; config.gcRevision = 3;
        config.gfxTargetVersion = 110001; // KFD: "compiler version 11.0.1, HW 11.0.3"
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (GC 11.0.3 fake)");
        CHECK(text(findGPU(), HSA_AGENT_INFO_NAME) == "gfx1101", "reported gfx_target_version 110001 -> gfx1101");
        hsa_shut_down();
        config = {};
        config.gcMajor = 13; config.gcMinor = 0; config.gcRevision = 0;
        mac_hsa::setFakeDeviceConfig(config);
        CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init (unknown GC 13.0.0 fake)");
        CHECK(findGPU().handle == 0, "a GPU with no known ISA is not published");
        hsa_shut_down();
    }
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
