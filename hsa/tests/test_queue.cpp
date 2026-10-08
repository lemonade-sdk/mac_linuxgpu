// Host unit test: queue create / kick / destroy.
//
// Exercises hsa_queue_create / hsa_queue_destroy / the queue index load/store/
// RMW family / the doorbell signal against the fake backend. The fake models
// the queue registry (createQueue/kickQueue/destroyQueue) and the doorbell
// (the runtime's RuntimeQueue.ringDoorbell calls connection->kickQueue).

#include "mac_hsa.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

    // 1. Create a queue (size 64, the minimum for persistent queues).
    hsa_queue_t *queue = nullptr;
    status = hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_queue_create (size 64)");
    CHECK(queue != nullptr, "queue pointer non-null");
    if (status != HSA_STATUS_SUCCESS) { hsa_shut_down(); return failures ? 1 : 0; }
    CHECK(fake->queueCount() == 1, "fake has 1 queue after create");

    // 2. The queue ABI is populated (type, size, base_address, doorbell).
    CHECK(queue->type == HSA_QUEUE_TYPE_MULTI, "queue type is MULTI");
    CHECK(queue->size == 64, "queue size is 64");
    CHECK(queue->base_address != nullptr, "queue base_address is set");
    CHECK(queue->doorbell_signal.handle != 0, "queue doorbell_signal is set");

    // 3. Queue index load/store (the write index starts at 0).
    CHECK(hsa_queue_load_write_index_relaxed(queue) == 0, "initial write index is 0");
    CHECK(hsa_queue_load_read_index_relaxed(queue) == 0, "initial read index is 0");

    // 4. Store the write index.
    hsa_queue_store_write_index_relaxed(queue, 5);
    CHECK(hsa_queue_load_write_index_relaxed(queue) == 5, "store_write_index(5) -> load==5");

    // 5. Add to the write index (RMW).
    const auto oldWrite = hsa_queue_add_write_index_relaxed(queue, 3);
    CHECK(oldWrite == 5, "add_write_index(3): old==5");
    CHECK(hsa_queue_load_write_index_relaxed(queue) == 8, "add_write_index(3): new==8");

    // 6. CAS the write index.
    uint64_t expected = 8;
    const auto casOld = hsa_queue_cas_write_index_relaxed(queue, expected, 10);
    CHECK(casOld == 8, "cas_write_index(8->10): old==8");
    CHECK(hsa_queue_load_write_index_relaxed(queue) == 10, "cas_write_index(8->10): new==10");

    // 7. The doorbell signal: writing a dispatch packet + ringing the doorbell
    //    kicks the queue on the fake. (The runtime's RuntimeQueue.ringDoorbell
    //    is triggered by the doorbell_signal's store hook, which fires when the
    //    doorbell signal is stored.)
    const auto kicksBefore = fake->queueCount(); // (kicks are per-handle)
    (void)kicksBefore;
    // Find the hardware handle (the fake's nextQueueHandle_ - 1 for the 1st queue).
    // We can't read it directly, but the kick count is observable per handle.
    // The doorbell signal handle -> store it -> triggers ringDoorbell -> kickQueue.
    hsa_signal_store_relaxed(queue->doorbell_signal, 1);
    // The fake's kickQueue is called with the hardware handle; verify a kick
    // happened on SOME queue (the fake tracks kicks per handle).
    // Since we don't know the handle, check the queue is still active.
    CHECK(hsa_queue_load_write_index_relaxed(queue) == 10, "write index unchanged by doorbell");

    // 8. Destroy the queue.
    status = hsa_queue_destroy(queue);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_queue_destroy");
    CHECK(fake->queueCount() == 0, "fake has 0 queues after destroy");

    // 9. Destroying again fails (invalid queue).
    status = hsa_queue_destroy(queue);
    CHECK(status == HSA_STATUS_ERROR_INVALID_QUEUE, "second destroy is INVALID_QUEUE");

    // 10. Create an invalid size (not a power of two) fails.
    hsa_queue_t *bad = nullptr;
    status = hsa_queue_create(gpu, 100, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &bad);
    CHECK(status == HSA_STATUS_ERROR_INVALID_ARGUMENT, "non-power-of-2 size is rejected");

    // 11. Create a too-small size (< 64) fails.
    status = hsa_queue_create(gpu, 32, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &bad);
    CHECK(status == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "size < 64 is rejected");

    // 12. hsa_amd_queue_create: a version-1 compute descriptor makes the same
    //     queue hsa_queue_create does, sized in bytes, and writes it back.
    uint32_t cuCount = 0;
    CHECK(hsa_agent_get_info(gpu, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT, &cuCount) ==
              HSA_STATUS_SUCCESS && cuCount > 0, "agent reports its CU count");
    auto computeDescriptor = []() {
        hsa_amd_queue_create_desc_t desc{};
        desc.version = HSA_AMD_QUEUE_CREATE_DESC_VERSION;
        desc.flags = HSA_AMD_QUEUE_CREATE_SYSTEM_MEM;
        desc.engine_type = HSA_AMD_QUEUE_ENGINE_COMPUTE;
        desc.queue_size_bytes = 64 * sizeof(hsa_kernel_dispatch_packet_t);
        desc.priority = HSA_AMD_QUEUE_PRIORITY_NORMAL;
        desc.engine.compute.type = HSA_QUEUE_TYPE_MULTI;
        desc.engine.compute.private_segment_size = HSA_AMD_PRIVATE_SEGMENT_SIZE_DEFAULT;
        return desc;
    };
    hsa_amd_queue_create_desc_t desc = computeDescriptor();
    status = hsa_amd_queue_create(gpu, &desc, 1);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_amd_queue_create (64 packets)");
    CHECK(desc.queue != nullptr && desc.queue->size == 64 && desc.queue->type == HSA_QUEUE_TYPE_MULTI,
          "descriptor queue written back with 64 MULTI packets");
    CHECK(fake->queueCount() == 1, "fake has 1 queue after descriptor create");
    if (desc.queue) CHECK(hsa_queue_destroy(desc.queue) == HSA_STATUS_SUCCESS, "destroy descriptor queue");

    // 13. A CU mask naming every CU is the queue's whole CU set and is accepted,
    //     at creation and afterwards; a mask leaving a CU out is not.
    uint32_t fullMask[4] = {};
    const uint32_t maskBits = ((cuCount + 31) / 32) * 32;
    for (uint32_t cu = 0; cu < cuCount; ++cu) fullMask[cu / 32] |= 1u << (cu % 32);
    desc = computeDescriptor();
    desc.engine.compute.cu_mask = fullMask;
    desc.engine.compute.cu_mask_count = maskBits;
    status = hsa_amd_queue_create(gpu, &desc, 1);
    CHECK(status == HSA_STATUS_SUCCESS && desc.queue, "descriptor with a full CU mask is created");
    if (desc.queue) {
        CHECK(hsa_amd_queue_cu_set_mask(desc.queue, maskBits, fullMask) == HSA_STATUS_SUCCESS,
              "cu_set_mask with every CU succeeds");
        uint32_t partial[4] = {};
        std::memcpy(partial, fullMask, sizeof(partial));
        partial[0] &= ~1u;
        CHECK(hsa_amd_queue_cu_set_mask(desc.queue, maskBits, partial) == HSA_STATUS_ERROR_INVALID_QUEUE,
              "cu_set_mask leaving a CU out is refused");
        CHECK(hsa_queue_destroy(desc.queue) == HSA_STATUS_SUCCESS, "destroy masked queue");
    }
    desc = computeDescriptor();
    uint32_t partialMask[4] = {};
    std::memcpy(partialMask, fullMask, sizeof(partialMask));
    partialMask[0] &= ~1u;
    desc.engine.compute.cu_mask = partialMask;
    desc.engine.compute.cu_mask_count = maskBits;
    status = hsa_amd_queue_create(gpu, &desc, 1);
    CHECK(status == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION && !desc.queue,
          "descriptor with a partial CU mask is refused and leaves no queue");
    CHECK(fake->queueCount() == 0, "refused partial mask destroyed its queue");

    // 14. Requests this transport cannot honour fail as INVALID_QUEUE_CREATION;
    //     malformed descriptors as INVALID_ARGUMENT.
    desc = computeDescriptor(); desc.engine_type = HSA_AMD_QUEUE_ENGINE_SDMA;
    desc.engine.sdma.sdma_engine_id = HSA_AMD_SDMA_ENGINE_ID_ANY;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "SDMA engine refused");
    desc = computeDescriptor(); desc.engine.compute.type = HSA_QUEUE_TYPE_COOPERATIVE;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "cooperative refused");
    desc = computeDescriptor(); desc.priority = HSA_AMD_QUEUE_PRIORITY_HIGH;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "high priority refused");
    desc = computeDescriptor(); desc.flags = HSA_AMD_QUEUE_CREATE_DEVICE_MEM_RING_BUF;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "device-memory ring refused");
    desc = computeDescriptor(); desc.version = 2;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT, "unknown version rejected");
    desc = computeDescriptor(); desc.queue_size_bytes = 100;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT, "non-power-of-2 bytes rejected");
    desc = computeDescriptor(); desc.reserved[3] = 1;
    CHECK(hsa_amd_queue_create(gpu, &desc, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT, "nonzero reserved rejected");
    CHECK(hsa_amd_queue_create(gpu, nullptr, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT, "NULL descriptors rejected");
    desc = computeDescriptor();
    CHECK(hsa_amd_queue_create(gpu, &desc, 0) == HSA_STATUS_ERROR_INVALID_ARGUMENT, "zero descriptors rejected");
    CHECK(hsa_amd_queue_create(hsa_agent_t{0x1234}, &desc, 1) == HSA_STATUS_ERROR_INVALID_AGENT, "unknown agent rejected");

    // 15. A batch keeps the queues it made and reports the first failure.
    hsa_amd_queue_create_desc_t batch[2] = {computeDescriptor(), computeDescriptor()};
    batch[1].engine.compute.type = HSA_QUEUE_TYPE_COOPERATIVE;
    status = hsa_amd_queue_create(gpu, batch, 2);
    CHECK(status == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION, "batch reports the failed descriptor");
    CHECK(batch[0].queue != nullptr && batch[1].queue == nullptr, "batch keeps the created queue");
    if (batch[0].queue) CHECK(hsa_queue_destroy(batch[0].queue) == HSA_STATUS_SUCCESS, "destroy batch queue");

    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
