// The driver's persistent AQL queues across a device reset: the real
// dext/sources/dext_aql.mm against a mock MES and compute context.
//  - prepare/restore: a queue mapped before the reset is mapped again at
//    its producer's position with a rebuilt MQD; a kick between the two is
//    refused, one after it reaches the doorbell;
//  - a queue the reset unmapped and nobody restored (the device wedged) is
//    destroyed without an MES unmap;
//  - a restore that fails leaves the queue retained (uncertain);
//  - the hooks run on another thread while the owner creates, kicks and
//    destroys queues (run under ASan and under TSan).
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <mutex>

#include "../../dext/sources/dext_aql.mm"

struct rt_compute_ctx { int unused; };
struct rt_compute_bo {
    uint64_t size, gpu;
    uint8_t *cpu;
};
static rt_compute_ctx context;
static struct amdgpu_device *const fake_adev = reinterpret_cast<struct amdgpu_device *>(0x1000);
static std::mutex mock_lock;     // the mock MES and BO table
static std::atomic<uint64_t> next_gpu{0x100000};
static std::atomic<unsigned> maps, unmaps, kicks, mqd_builds, storage_writes, frees, live_bos;
static std::atomic<int> map_error;
static std::atomic<uint64_t> last_position;
static bool slot_used[8], slot_mapped[8];

int rt_compute_status(rt_compute_ctx *ctx) { assert(ctx == &context); return 0; }
struct amdgpu_device *rt_compute_device(rt_compute_ctx *ctx) { assert(ctx == &context); return fake_adev; }
int rt_device_topology(struct amdgpu_device *, struct rt_compute_topology *) { return -ENODEV; }

static rt_compute_bo *bo_new(uint64_t size)
{
    auto *bo = new rt_compute_bo;
    bo->size = size;
    bo->gpu = next_gpu.fetch_add((size + 0xffff) & ~0xffffull);
    bo->cpu = static_cast<uint8_t *>(aligned_alloc(64, (size + 63) & ~63ull));
    memset(bo->cpu, 0, size);
    ++live_bos;
    return bo;
}
int rt_compute_bo_alloc(rt_compute_ctx *, uint64_t size, uint64_t, enum rt_compute_domain, rt_compute_bo **out)
{
    *out = bo_new(size);
    return 0;
}
int rt_compute_bo_info(rt_compute_ctx *, rt_compute_bo *bo, struct rt_compute_bo_info *out)
{
    memset(out, 0, sizeof(*out));
    out->size = bo->size;
    out->gpu_address = bo->gpu;
    out->cpu_address = bo->cpu;
    out->shared_descriptor_available = 1;
    return 0;
}
int rt_compute_bo_write(rt_compute_ctx *, rt_compute_bo *bo, uint64_t offset, const void *src, size_t size)
{
    assert(offset + size <= bo->size);
    if (size == amdgpu::kAQLStorageBytes) ++storage_writes;
    usleep(50);   // an SDMA copy takes a while: widen the windows
    memcpy(bo->cpu + offset, src, size);
    return 0;
}
int rt_compute_bo_read(rt_compute_ctx *, rt_compute_bo *bo, uint64_t offset, void *dst, size_t size)
{
    memcpy(dst, bo->cpu + offset, size);
    return 0;
}
int rt_compute_bo_free(rt_compute_ctx *, rt_compute_bo *bo)
{
    free(bo->cpu);
    delete bo;
    ++frees;
    --live_bos;
    return 0;
}

int rt_queue_geometry(struct amdgpu_device *, struct rt_queue_geometry *g)
{
    memset(g, 0, sizeof(*g));
    g->gfx_major = 12; g->gfx_minor = 0; g->gfx_revision = 1;
    g->engines = 4; g->compute_units = 64; g->waves_per_cu = 32;
    g->mqd_bytes = 2048;
    return 0;
}
int rt_queue_partition(struct amdgpu_device *, struct rt_queue_partition *) { return 0; }
int rt_queue_reserve(struct amdgpu_device *, uint32_t *slot, uint32_t *doorbell)
{
    std::lock_guard<std::mutex> g(mock_lock);
    for (uint32_t s = 0; s < 8; ++s)
        if (!slot_used[s]) { slot_used[s] = true; *slot = s; *doorbell = 0x40 + 2 * s; return 0; }
    return -ENOSPC;
}
void rt_queue_release(struct amdgpu_device *, uint32_t slot)
{
    std::lock_guard<std::mutex> g(mock_lock);
    assert(slot_used[slot] && !slot_mapped[slot]);
    slot_used[slot] = false;
}
int rt_queue_build_mqd(struct amdgpu_device *, uint32_t slot, uint32_t, const struct rt_queue_mqd *d,
                       void *mqd, size_t bytes)
{
    assert(bytes >= 2048 && d->ring_bytes);
    ++mqd_builds;
    memset(mqd, 0, bytes);
    memcpy(mqd, &slot, sizeof(slot));
    return 0;
}
int rt_queue_mqd_set_position(void *, uint64_t ring_bytes, uint64_t packet)
{
    assert(ring_bytes);
    last_position = packet;
    return 0;
}
// MES: a slot is mapped once; mapping it twice or unmapping a slot it
// does not run is an error, as the firmware reports it.
int rt_queue_map(struct amdgpu_device *, uint32_t slot, uint32_t, uint64_t, uint64_t)
{
    usleep(50);
    std::lock_guard<std::mutex> g(mock_lock);
    if (map_error) return map_error;
    assert(slot_used[slot] && !slot_mapped[slot]);
    slot_mapped[slot] = true;
    ++maps;
    return 0;
}
int rt_queue_unmap(struct amdgpu_device *, uint32_t slot, uint32_t)
{
    std::lock_guard<std::mutex> g(mock_lock);
    assert(slot_mapped[slot]);
    slot_mapped[slot] = false;
    ++unmaps;
    return 0;
}
int rt_queue_kick(struct amdgpu_device *, uint32_t doorbell, uint64_t)
{
    assert(doorbell >= 0x40);
    ++kicks;
    return 0;
}
void rt_queue_flush(struct amdgpu_device *) {}

// The device reset itself: MES forgets every queue.
static void mes_reset()
{
    std::lock_guard<std::mutex> g(mock_lock);
    memset(slot_mapped, 0, sizeof(slot_mapped));
}

struct client_queue {
    rt_compute_bo *ring, *meta;
    dext_aql_queue *q;
};
static amd_queue_t *meta_of(const client_queue &c) { return reinterpret_cast<amd_queue_t *>(c.meta->cpu); }

static client_queue client_create(uint32_t packets)
{
    client_queue c{};
    c.ring = bo_new(uint64_t(packets) * 64);
    c.meta = bo_new(4096);
    amd_queue_t *m = meta_of(c);
    m->hsa_queue.size = packets;
    m->hsa_queue.base_address = reinterpret_cast<void *>(uintptr_t(c.ring->gpu));
    m->read_dispatch_id_field_base_byte_offset = offsetof(amd_queue_t, read_dispatch_id);
    m->queue_properties = AMD_QUEUE_PROPERTIES_IS_PTR64;
    assert(dext_aql_create(&context, c.ring, c.meta, packets, &c.q) == 0 && c.q);
    return c;
}
static void client_destroy(client_queue &c)
{
    assert(dext_aql_destroy(c.q) == 0);
    rt_compute_bo_free(&context, c.ring);
    rt_compute_bo_free(&context, c.meta);
    c = client_queue{};
}

static void restore_check()
{
    client_queue c = client_create(64);
    assert(maps == 1);
    amd_queue_t *m = meta_of(c);
    m->write_dispatch_id = 21;
    m->read_dispatch_id = 5;      // packets 5..20 were in flight
    assert(dext_aql_kick(c.q, 20) == 0 && kicks == 1);
    const unsigned builds = mqd_builds, writes = storage_writes;

    dext_aql_reset_prepare();
    mes_reset();
    assert(dext_aql_kick(c.q, 20) == -19 && kicks == 1);      // not mapped now
    assert(!dext_aql_uncertain(c.q));
    assert(dext_aql_reset_restore() == 0);
    // A fresh MQD at the producer's position; the reader starts there.
    assert(mqd_builds == builds + 1 && storage_writes == writes + 1);
    assert(last_position == 21 && m->read_dispatch_id == 21);
    assert(maps == 2 && !dext_aql_uncertain(c.q));
    m->write_dispatch_id = 22;
    assert(dext_aql_kick(c.q, 21) == 0 && kicks == 2);
    client_destroy(c);
    assert(unmaps == 1);
    puts("PASS AQL reset restore: a queue mapped before a device reset is mapped again at its "
         "producer's position with a rebuilt MQD; kicks in between are refused");
}

static void wedged_destroy_check()
{
    client_queue c = client_create(64);
    const unsigned unmaps_before = unmaps;
    dext_aql_reset_prepare();
    mes_reset();
    // No restore (the reset failed and the device wedged): destroying the
    // queue frees it without asking MES to unmap what it no longer runs.
    client_destroy(c);
    assert(unmaps == unmaps_before);
    // A later reset finds nothing to restore.
    dext_aql_reset_prepare();
    assert(dext_aql_reset_restore() == 0);
    puts("PASS AQL reset wedged: a queue the reset unmapped is destroyed without an MES unmap");
}

static void restore_failure_check()
{
    client_queue c = client_create(64);
    dext_aql_reset_prepare();
    mes_reset();
    map_error = -ETIMEDOUT;
    assert(dext_aql_reset_restore() == 1);
    map_error = 0;
    assert(dext_aql_uncertain(c.q));
    assert(dext_aql_kick(c.q, 0) == -19);
    assert(dext_aql_destroy(c.q) == -16);     // retained: its BOs stay
    // A further reset leaves a retained queue alone.
    dext_aql_reset_prepare();
    assert(dext_aql_reset_restore() == 0 && dext_aql_uncertain(c.q));
    puts("PASS AQL reset failure: a queue that cannot be mapped again stays retained (uncertain)");
}

// The reset hooks on their own thread while the owner creates, kicks and
// destroys queues. Every listed queue is restored exactly once per reset,
// none after its destroy.
static std::atomic<bool> stop;
static void *reset_thread(void *)
{
    while (!stop) {
        dext_aql_reset_prepare();
        mes_reset();
        assert(dext_aql_reset_restore() == 0);
        usleep(200);
    }
    return nullptr;
}

static void concurrency_check()
{
    pthread_t thread;
    const unsigned bos_before = live_bos;
    stop = false;
    assert(pthread_create(&thread, nullptr, reset_thread, nullptr) == 0);
    for (int round = 0; round < 300; ++round) {
        client_queue a = client_create(64), b = client_create(128);
        for (int i = 0; i < 20; ++i) {
            // The client publishes its write index with a release store, as
            // the HSA runtime does; the reset thread reads it concurrently.
            __atomic_store_n(&meta_of(a)->write_dispatch_id, uint64_t(i) + 1,
                             __ATOMIC_RELEASE);
            const int r = dext_aql_kick(a.q, uint64_t(i));
            assert(r == 0 || r == -19);
        }
        client_destroy(a);
        client_destroy(b);
    }
    stop = true;
    pthread_join(thread, nullptr);
    assert(live_bos == bos_before);
    for (int s = 0; s < 8; ++s) assert(!slot_used[s] || s == 0);
    puts("PASS AQL reset concurrency: reset hooks on their own thread against 300 rounds of "
         "create/kick/destroy");
}

int main()
{
    // dext_compute_start makes the lock before any queue exists.
    assert(dext_aql_available(&context) == 0);
    restore_check();
    wedged_destroy_check();
    restore_failure_check();
    concurrency_check();
    return 0;
}
