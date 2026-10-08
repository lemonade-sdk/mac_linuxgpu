// Host unit test: virtual-memory access queries.
//
// hsa_amd_vmem_get_access reports what hsa_amd_vmem_set_access granted a
// mapping: the CPU has what it was last given, and a GPU agent has nothing,
// because set_access never grants a GPU access on this transport.

#include "mac_hsa.h"
#include <hsa/hsa_ext_amd.h>
#include <cstdio>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

namespace {
struct Agents { hsa_agent_t cpu{}, gpu{}; };
hsa_status_t collect(hsa_agent_t agent, void *data) {
    auto *agents = static_cast<Agents *>(data);
    hsa_device_type_t type;
    if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS) return HSA_STATUS_SUCCESS;
    if (type == HSA_DEVICE_TYPE_CPU && !agents->cpu.handle) agents->cpu = agent;
    if (type == HSA_DEVICE_TYPE_GPU && !agents->gpu.handle) agents->gpu = agent;
    return HSA_STATUS_SUCCESS;
}
hsa_status_t firstPool(hsa_amd_memory_pool_t pool, void *data) {
    hsa_amd_segment_t segment;
    if (hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment) == HSA_STATUS_SUCCESS &&
        segment == HSA_AMD_SEGMENT_GLOBAL) {
        *static_cast<hsa_amd_memory_pool_t *>(data) = pool;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}
}

int main() {
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init");
    Agents agents;
    hsa_iterate_agents(collect, &agents);
    CHECK(agents.cpu.handle && agents.gpu.handle, "found CPU and GPU agents");
    hsa_amd_memory_pool_t pool{};
    hsa_amd_agent_iterate_memory_pools(agents.cpu, firstPool, &pool);
    CHECK(pool.handle != 0, "found a CPU global pool");

    const size_t size = size_t(getpagesize()) * 4;
    void *base = nullptr;
    CHECK(hsa_amd_vmem_address_reserve_align(&base, size, 0, 0, 0) == HSA_STATUS_SUCCESS && base,
          "reserve address range");
    hsa_amd_vmem_alloc_handle_t handle{};
    CHECK(hsa_amd_vmem_handle_create(pool, size, MEMORY_TYPE_NONE, 0, &handle) == HSA_STATUS_SUCCESS,
          "create physical handle");
    CHECK(hsa_amd_vmem_map(base, size, 0, handle, 0) == HSA_STATUS_SUCCESS, "map handle");

    hsa_access_permission_t permissions = HSA_ACCESS_PERMISSION_RW;
    CHECK(hsa_amd_vmem_get_access(base, &permissions, agents.cpu) == HSA_STATUS_SUCCESS &&
              permissions == HSA_ACCESS_PERMISSION_NONE, "fresh mapping: CPU has no access");
    hsa_amd_memory_access_desc_t grant{HSA_ACCESS_PERMISSION_RW, agents.cpu};
    CHECK(hsa_amd_vmem_set_access(base, size, &grant, 1) == HSA_STATUS_SUCCESS, "grant CPU read-write");
    CHECK(hsa_amd_vmem_get_access(base, &permissions, agents.cpu) == HSA_STATUS_SUCCESS &&
              permissions == HSA_ACCESS_PERMISSION_RW, "CPU reads back read-write");
    grant.permissions = HSA_ACCESS_PERMISSION_RO;
    CHECK(hsa_amd_vmem_set_access(base, size, &grant, 1) == HSA_STATUS_SUCCESS, "narrow CPU to read-only");
    CHECK(hsa_amd_vmem_get_access(base, &permissions, agents.cpu) == HSA_STATUS_SUCCESS &&
              permissions == HSA_ACCESS_PERMISSION_RO, "CPU reads back read-only");
    permissions = HSA_ACCESS_PERMISSION_RW;
    CHECK(hsa_amd_vmem_get_access(base, &permissions, agents.gpu) == HSA_STATUS_SUCCESS &&
              permissions == HSA_ACCESS_PERMISSION_NONE, "GPU has no access");

    int unmapped = 0;
    CHECK(hsa_amd_vmem_get_access(&unmapped, &permissions, agents.cpu) == HSA_STATUS_ERROR_INVALID_ALLOCATION,
          "unmapped address is INVALID_ALLOCATION");
    CHECK(hsa_amd_vmem_get_access(base, nullptr, agents.cpu) == HSA_STATUS_ERROR_INVALID_ARGUMENT,
          "NULL permissions rejected");
    CHECK(hsa_amd_vmem_get_access(base, &permissions, hsa_agent_t{0x1234}) == HSA_STATUS_ERROR_INVALID_AGENT,
          "unknown agent rejected");

    CHECK(hsa_amd_vmem_unmap(base, size) == HSA_STATUS_SUCCESS, "unmap");
    CHECK(hsa_amd_vmem_handle_release(handle) == HSA_STATUS_SUCCESS, "release handle");
    CHECK(hsa_amd_vmem_address_free(base, size) == HSA_STATUS_SUCCESS, "free address range");
    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
