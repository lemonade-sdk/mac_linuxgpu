// Host unit test: BAR writes (dext/sources/hdp_flush.h) as the runtime
// reports and brackets them, against the fake backend.
//
// A client that opted in (mac_hsa_bar_writes_enable) sees what ROCr reports
// on a large-BAR device: HSA_AMD_AGENT_INFO_HDP_FLUSH, and the CPU agent's
// access to the GPU's coarse VRAM pool as DISALLOWED_BY_DEFAULT, with
// hsa_amd_agents_allow_access mapping one allocation for the CPU at its own
// address. A client that did not opt in sees none of it. Brackets open while
// the gate is, wait while the driver holds it (a power transition), and
// fail once the device is lost, with the mappings retired first.

#include "mac_hsa.h"
#include "transport_fake.h"
#include "../../dext/sources/doorbell_gate.h"
#include <hsa/hsa_ext_amd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

namespace {
struct Agents { hsa_agent_t gpu{}, cpu{}; };
Agents findAgents() {
    Agents found;
    hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        auto *a = static_cast<Agents *>(data);
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS) return HSA_STATUS_SUCCESS;
        if (type == HSA_DEVICE_TYPE_GPU) a->gpu = agent;
        if (type == HSA_DEVICE_TYPE_CPU) a->cpu = agent;
        return HSA_STATUS_SUCCESS;
    }, &found);
    return found;
}
hsa_amd_memory_pool_t vramPool(hsa_agent_t gpu) {
    hsa_amd_memory_pool_t found{};
    hsa_amd_agent_iterate_memory_pools(gpu, [](hsa_amd_memory_pool_t pool, void *data) -> hsa_status_t {
        hsa_amd_memory_pool_location_t location;
        if (hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_LOCATION, &location) == HSA_STATUS_SUCCESS &&
            location == HSA_AMD_MEMORY_POOL_LOCATION_GPU)
            *static_cast<hsa_amd_memory_pool_t *>(data) = pool;
        return HSA_STATUS_SUCCESS;
    }, &found);
    return found;
}
hsa_amd_memory_pool_access_t cpuAccess(hsa_agent_t cpu, hsa_amd_memory_pool_t pool) {
    hsa_amd_memory_pool_access_t access = HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED;
    if (hsa_amd_agent_memory_pool_get_info(cpu, pool, HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access) !=
        HSA_STATUS_SUCCESS)
        return hsa_amd_memory_pool_access_t(~0u);
    return access;
}
}

// A legacy session (or a driver without BAR writes): enable fails, and
// nothing of it is reported.
static void declined() {
    mac_hsa::FakeDeviceConfig config;
    config.sessionMode = mac_hsa::ComputeSessionMode::Legacy;
    config.barWrites = false;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "declined: hsa_init");
    const auto agents = findAgents();
    mac_hsa_bar_writer_t *writer = nullptr;
    CHECK(mac_hsa_bar_writes_enable(agents.gpu, &writer) != HSA_STATUS_SUCCESS && !writer,
          "declined: enable fails without driver support");
    hsa_amd_hdp_flush_t hdp{};
    CHECK(hsa_agent_get_info(agents.gpu, hsa_agent_info_t(HSA_AMD_AGENT_INFO_HDP_FLUSH), &hdp) ==
              HSA_STATUS_ERROR_INVALID_ARGUMENT && !hdp.HDP_MEM_FLUSH_CNTL,
          "declined: no HDP_FLUSH");
    CHECK(cpuAccess(agents.cpu, vramPool(agents.gpu)) == HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED,
          "declined: CPU never accesses the VRAM pool");
    hsa_shut_down();
}

static void enabled() {
    mac_hsa::FakeDeviceConfig config;
    config.sessionMode = mac_hsa::ComputeSessionMode::KFD;
    config.barWrites = true;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init");
    const auto agents = findAgents();
    auto fake = mac_hsa::fakeConnection();
    const auto pool = vramPool(agents.gpu);
    CHECK(agents.gpu.handle && agents.cpu.handle && pool.handle && fake, "agents, VRAM pool, fake");

    // Before the client opts in: as a client that never does.
    hsa_amd_hdp_flush_t hdp{};
    CHECK(hsa_agent_get_info(agents.gpu, hsa_agent_info_t(HSA_AMD_AGENT_INFO_HDP_FLUSH), &hdp) ==
              HSA_STATUS_ERROR_INVALID_ARGUMENT, "no HDP_FLUSH before enable");
    CHECK(cpuAccess(agents.cpu, pool) == HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED,
          "CPU access NEVER_ALLOWED before enable");
    void *early = nullptr;
    CHECK(hsa_amd_memory_pool_allocate(pool, 65536, 0, &early) == HSA_STATUS_SUCCESS, "allocation before enable");
    hsa_agent_t both[2] = {agents.cpu, agents.gpu};
    CHECK(hsa_amd_agents_allow_access(2, both, nullptr, early) == HSA_STATUS_ERROR_INVALID_ARGUMENT,
          "the CPU is not granted VRAM before enable");

    mac_hsa_bar_writer_t *writer = nullptr, *again = nullptr;
    CHECK(mac_hsa_bar_writes_enable(agents.gpu, &writer) == HSA_STATUS_SUCCESS && writer, "enable");
    CHECK(mac_hsa_bar_writes_enable(agents.gpu, &again) == HSA_STATUS_SUCCESS && again == writer,
          "enable again: the same writer");
    CHECK(mac_hsa_bar_writes_enable(agents.cpu, &again) == HSA_STATUS_ERROR_INVALID_AGENT, "a CPU agent has none");

    // What ROCr reports on a large-BAR device.
    CHECK(hsa_agent_get_info(agents.gpu, hsa_agent_info_t(HSA_AMD_AGENT_INFO_HDP_FLUSH), &hdp) ==
              HSA_STATUS_SUCCESS && hdp.HDP_MEM_FLUSH_CNTL && hdp.HDP_REG_FLUSH_CNTL &&
              hdp.HDP_REG_FLUSH_CNTL == hdp.HDP_MEM_FLUSH_CNTL + 1,
          "HDP_FLUSH: MEM_FLUSH_CNTL and REG_FLUSH_CNTL, four bytes apart (KFD_MMIO_REMAP_*)");
    CHECK(cpuAccess(agents.cpu, pool) == HSA_AMD_MEMORY_POOL_ACCESS_DISALLOWED_BY_DEFAULT,
          "CPU access to the VRAM pool DISALLOWED_BY_DEFAULT");
    CHECK(cpuAccess(agents.gpu, pool) == HSA_AMD_MEMORY_POOL_ACCESS_ALLOWED_BY_DEFAULT,
          "the GPU's own access unchanged");
    bool all = true;
    CHECK(hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL, &all) ==
              HSA_STATUS_SUCCESS && !all, "the VRAM pool is not accessible by all");
    CHECK(hsa_amd_agents_allow_access(2, both, nullptr, early) == HSA_STATUS_ERROR_INVALID_ARGUMENT,
          "an allocation from before enable still cannot be granted");

    // A pool allocation: no CPU mapping until the CPU is granted it.
    void *ring = nullptr, *weights = nullptr;
    CHECK(hsa_amd_memory_pool_allocate(pool, 1 << 20, 0, &ring) == HSA_STATUS_SUCCESS && ring, "kernarg ring");
    CHECK(hsa_amd_memory_pool_allocate(pool, 4 << 20, 0, &weights) == HSA_STATUS_SUCCESS && weights, "weights");
    hsa_amd_pointer_info_t info{};
    info.size = sizeof(info);
    CHECK(hsa_amd_pointer_info(ring, &info, nullptr, nullptr, nullptr) == HSA_STATUS_SUCCESS &&
              !info.hostBaseAddress, "no host mapping before allow_access");
    CHECK(hsa_amd_agents_allow_access(1, &agents.gpu, nullptr, weights) == HSA_STATUS_SUCCESS &&
              fake->cpuMappedCount() == 0, "a GPU-only grant maps nothing for the CPU");
    CHECK(hsa_amd_agents_allow_access(2, both, nullptr, ring) == HSA_STATUS_SUCCESS, "allow_access(CPU, GPU)");
    CHECK(fake->cpuMappedCount() == 1, "exactly the granted allocation is CPU-mapped");
    CHECK(hsa_amd_agents_allow_access(2, both, nullptr, ring) == HSA_STATUS_SUCCESS &&
              fake->cpuMappedCount() == 1, "granting again maps nothing more");
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    uint32_t count = 0;
    hsa_agent_t *accessible = nullptr;
    CHECK(hsa_amd_pointer_info(ring, &info, malloc, &count, &accessible) == HSA_STATUS_SUCCESS &&
              info.hostBaseAddress == ring && info.agentBaseAddress == ring,
          "the CPU maps it at its own address");
    CHECK(count == 2, "accessible by the GPU and the CPU");
    free(accessible);

    // A submission's stores, bracketed: kernel arguments into the ring, the
    // HDP flush stored and read back.
    const auto gate = fake->gate();
    CHECK(mac_hsa_bar_write_begin(writer) == HSA_STATUS_SUCCESS && gate->busy == 1, "begin");
    CHECK(mac_hsa_bar_write_begin(writer) == HSA_STATUS_SUCCESS && gate->busy == 2, "brackets nest");
    std::memset(ring, 0x5a, 256);
    *hdp.HDP_MEM_FLUSH_CNTL = 1u;
    (void)*hdp.HDP_MEM_FLUSH_CNTL;
    mac_hsa_bar_write_end(writer);
    mac_hsa_bar_write_end(writer);
    CHECK(gate->busy == 0 && *hdp.HDP_MEM_FLUSH_CNTL == 1u, "end");

    // Held for a power transition: begin waits until the gate opens again.
    fake->setPowerState(amdgpu::power::PowerState::Suspended);
    CHECK(!gate->open && !mlg_doorbell_gate_retired(gate), "suspended: the gate is held");
    std::atomic<bool> opened{false};
    std::thread waiter([&] {
        const auto status = mac_hsa_bar_write_begin(writer);
        opened = status == HSA_STATUS_SUCCESS;
        if (opened) mac_hsa_bar_write_end(writer);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!opened.load(), "begin waits while the gate is held");
    fake->setPowerState(amdgpu::power::PowerState::Active);
    waiter.join();
    CHECK(opened.load() && gate->busy == 0, "begin opens once the device takes work again");

    // Lost: the gate retired; begin fails with the mappings retired first.
    fake->setPowerState(amdgpu::power::PowerState::Lost, false);
    CHECK(mlg_doorbell_gate_retired(gate), "lost: the gate is retired");
    CHECK(mac_hsa_bar_write_begin(writer) == HSA_STATUS_ERROR_FATAL && gate->busy == 0,
          "begin fails once the device is lost, opening no bracket");
    CHECK(fake->barMappingsRetired() && fake->cpuMappedCount() == 0, "the mappings were retired before it returned");
    hsa_shut_down();
}

int main() {
    declined();
    enabled();
    std::fprintf(stderr, "%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
