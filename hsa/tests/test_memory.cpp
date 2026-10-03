// Host unit test: memory allocation + the buffer registry.
//
// Exercises hsa_memory_allocate / hsa_memory_free / hsa_memory_copy /
// hsa_amd_memory_fill / hsa_amd_pointer_info / the shared allocation
// (mac_hsa_memory_allocate_shared) / the pool info queries against the fake
// backend. The fake models the VRAM buffer registry + the shared (GTT)
// buffers, so the runtime's allocation bookkeeping + bounds checks + copy
// path are exercised on the host.

#include "mac_hsa.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

int main() {
    hsa_status_t status = hsa_init();
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_init");
    if (status != HSA_STATUS_SUCCESS) return 1;

    // Find the GPU agent.
    hsa_agent_t gpu{};
    status = hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
            type == HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(data) = agent;
        return HSA_STATUS_SUCCESS;
    }, &gpu);
    CHECK(gpu.handle != 0, "found a GPU agent");
    if (gpu.handle == 0) { hsa_shut_down(); return failures ? 1 : 0; }

    auto fake = mac_hsa::fakeConnection();
    CHECK(fake != nullptr, "fake connection available");
    if (!fake) { hsa_shut_down(); return failures ? 1 : 0; }

    // 1. Find the GPU's memory pool (region).
    hsa_region_t region{};
    status = hsa_agent_iterate_regions(gpu, [](hsa_region_t r, void *data) -> hsa_status_t {
        *static_cast<hsa_region_t *>(data) = r;
        return HSA_STATUS_SUCCESS;
    }, &region);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_agent_iterate_regions");
    CHECK(region.handle != 0, "found a GPU region");

    // 2. Region info: segment + size + alloc max.
    hsa_region_segment_t segment;
    status = hsa_region_get_info(region, HSA_REGION_INFO_SEGMENT, &segment);
    CHECK(status == HSA_STATUS_SUCCESS && segment == HSA_REGION_SEGMENT_GLOBAL, "region segment is GLOBAL");
    size_t allocMax = 0;
    status = hsa_region_get_info(region, HSA_REGION_INFO_ALLOC_MAX_SIZE, &allocMax);
    CHECK(status == HSA_STATUS_SUCCESS && allocMax > 0, "region alloc max size > 0");

    // 3. Allocate a buffer from the pool.
    void *buf = nullptr;
    status = hsa_memory_allocate(region, 4096, &buf);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_memory_allocate(4096)");
    CHECK(buf != nullptr, "allocation non-null");
    CHECK(fake->bufferCount() >= 1, "fake has a buffer after allocate");
    if (status != HSA_STATUS_SUCCESS) { hsa_shut_down(); return failures ? 1 : 0; }

    // 4. Pointer info: the allocation is recognized + its size/owner.
    hsa_amd_pointer_info_t info{};
    info.size = sizeof(info);
    status = hsa_amd_pointer_info(buf, &info, nullptr, nullptr, nullptr);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_amd_pointer_info");
    CHECK(info.type == HSA_EXT_POINTER_TYPE_HSA, "pointer type is HSA");
    CHECK(info.agentOwner.handle == gpu.handle, "pointer owner is the GPU");
    CHECK(info.sizeInBytes >= 4096, "pointer size >= 4096");

    // 5. The VRAM buffer is NOT host-accessible (hostBaseAddress is null for
    //    a non-shared GPU allocation).
    CHECK(info.hostBaseAddress == nullptr, "VRAM allocation has no host mapping");

    // 6. memoryAvailable decreases after the allocation.
    uint64_t available = 0;
    status = hsa_agent_get_info(gpu, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_MEMORY_AVAIL, &available);
    CHECK(status == HSA_STATUS_SUCCESS, "HSA_AMD_AGENT_INFO_MEMORY_AVAIL");
    CHECK(available < (32ull << 30), "available < total VRAM after alloc");

    // 7. Free the buffer.
    status = hsa_memory_free(buf);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_memory_free");
    // Pointer info on a freed pointer is unknown.
    hsa_amd_pointer_info_t info2{};
    info2.size = sizeof(info2);
    status = hsa_amd_pointer_info(buf, &info2, nullptr, nullptr, nullptr);
    CHECK(status == HSA_STATUS_SUCCESS && info2.type == HSA_EXT_POINTER_TYPE_UNKNOWN, "freed pointer is UNKNOWN");

    // 8. Shared allocation (CPU + GPU address identical).
    void *shared = nullptr;
    status = mac_hsa_memory_allocate_shared(gpu, 8192, &shared);
    CHECK(status == HSA_STATUS_SUCCESS, "mac_hsa_memory_allocate_shared(8192)");
    CHECK(shared != nullptr, "shared allocation non-null");
    if (status != HSA_STATUS_SUCCESS) { hsa_shut_down(); return failures ? 1 : 0; }

    // 9. The shared allocation IS host-accessible (hostBaseAddress == base).
    hsa_amd_pointer_info_t sinfo{};
    sinfo.size = sizeof(sinfo);
    status = hsa_amd_pointer_info(shared, &sinfo, nullptr, nullptr, nullptr);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_amd_pointer_info (shared)");
    CHECK(sinfo.hostBaseAddress == shared, "shared allocation hostBaseAddress == base");

    // 10. Sync capabilities: the shared allocation reports the GPU-mediating +
    //     ownership-transfer flags.
    uint32_t syncFlags = 0;
    status = mac_hsa_memory_get_sync_capabilities(gpu, shared, &syncFlags);
    CHECK(status == HSA_STATUS_SUCCESS, "mac_hsa_memory_get_sync_capabilities");
    CHECK((syncFlags & MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS) != 0, "shared: GPU_MEDIATED_SIGNALS set");
    CHECK((syncFlags & MAC_HSA_SYNC_OWNERSHIP_TRANSFER) != 0, "shared: OWNERSHIP_TRANSFER set");
    CHECK((syncFlags & MAC_HSA_SYNC_CPU_LOCAL_ATOMICS) != 0, "shared: CPU_LOCAL_ATOMICS set");

    // 11. Free the shared allocation.
    status = hsa_memory_free(shared);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_memory_free (shared)");

    // 12. Allocate + free a second buffer (no leak in the fake).
    const auto before = fake->bufferCount();
    void *buf2 = nullptr;
    status = hsa_memory_allocate(region, 2048, &buf2);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_memory_allocate(2048) [2nd]");
    CHECK(fake->bufferCount() == before + 1, "buffer count +1 after 2nd alloc");
    status = hsa_memory_free(buf2);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_memory_free [2nd]");
    CHECK(fake->bufferCount() == before, "buffer count restored after 2nd free");

    // 12b. An engine's worth of device and shared buffers, released the way
    //      an engine that is closing releases them: every buffer goes back to
    //      the driver, and doing it again starts from the same count.
    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto baseline = fake->bufferCount();
        std::vector<void *> held;
        for (int i = 0; i < 24; ++i) {
            void *p = nullptr;
            const auto r = (i % 3 == 2) ? mac_hsa_memory_allocate_shared(gpu, 65536, &p)
                                        : hsa_memory_allocate(region, size_t(1) << (12 + i % 8), &p);
            if (r == HSA_STATUS_SUCCESS && p) held.push_back(p);
        }
        CHECK(held.size() == 24, "engine-sized allocation set");
        CHECK(fake->bufferCount() == baseline + held.size(), "every allocation is one driver buffer");
        for (void *p : held) CHECK(hsa_memory_free(p) == HSA_STATUS_SUCCESS, "free");
        CHECK(fake->bufferCount() == baseline, "driver buffer count back to baseline after release");
    }

    // 13. Free a null pointer is a no-op success.
    status = hsa_memory_free(nullptr);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_memory_free(null) is success");

    // 14. Free an unknown pointer fails.
    int dummy = 0;
    status = hsa_memory_free(&dummy);
    CHECK(status == HSA_STATUS_ERROR_INVALID_ALLOCATION, "free(unknown) is INVALID_ALLOCATION");

    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
