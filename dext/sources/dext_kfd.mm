/* The runtime's AQL queue ABI over KFD compute sessions (dext_kfd.h). */
#include "dext_kfd.h"
#include "dext_aql.h"
#include <rt/compute.h>
#include <rt/kfd_session.h>
#include <rt/queue.h>
#include "../amdgpu/amdgpu_aql_packets.h"
#include <DriverKit/IOLib.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

struct dext_kfd_client {
    rt_compute_ctx *ctx;
    rt_kfd_session *session;
    struct rt_queue_geometry geometry;
    struct rt_kfd_apertures apertures;
    dext_kfd_queue *queues;
    // The bounded launch's buffers, kept for the client's lifetime.
    rt_kfd_bo *launch_ring, *launch_metadata, *launch_storage;
    bool uncertain;
};

struct dext_kfd_queue {
    dext_kfd_queue *next;
    dext_kfd_client *client;
    rt_kfd_queue *queue;
    rt_kfd_bo *ring, *metadata, *storage, *scratch;
    uint64_t ringVA, metadataVA, storageVA;
    uint32_t packets;
    uint64_t lastDoorbell;
    bool published, retained;
};

static int errno_of(int r) { return r < 0 ? r : -EIO; }

int dext_kfd_supported(rt_compute_ctx *ctx)
{
    auto *adev = rt_compute_device(ctx);
    struct rt_queue_geometry g{};
    if (!adev) return -ENODEV;
    int r = rt_kfd_session_supported(adev);
    return r ? r : rt_queue_device_geometry(adev, &g);
}

int dext_kfd_open(rt_compute_ctx *ctx, int pid, const char *comm, dext_kfd_client **out)
{
    if (!out) return -EINVAL;
    *out = nullptr;
    auto *adev = rt_compute_device(ctx);
    if (!adev) return -ENODEV;
    auto *c = static_cast<dext_kfd_client *>(IOMallocZero(sizeof(dext_kfd_client)));
    if (!c) return -ENOMEM;
    c->ctx = ctx;
    int r = rt_queue_device_geometry(adev, &c->geometry);
    if (!r) r = rt_kfd_session_open(adev, ctx, pid, comm, &c->session);
    if (!r) r = rt_kfd_session_apertures(c->session, &c->apertures);
    if (r) {
        if (c->session && rt_kfd_session_close(c->session)) {
            // An unclosable session stays referenced by this client.
            c->uncertain = true;
            *out = c;
            return r;
        }
        IOFree(c, sizeof(*c));
        return r;
    }
    *out = c;
    return 0;
}

int dext_kfd_uncertain(const dext_kfd_client *c)
{
    return c && (c->uncertain || rt_kfd_session_uncertain(c->session));
}

// Queue records whose KFD queue the session no longer schedules go; one
// still uncertain stays retained. 0 when none is left retained.
static int destroy_queue_records(dext_kfd_client *c)
{
    int kept = 0;
    for (dext_kfd_queue *q = c->queues, *next; q; q = next) {
        next = q->next;
        if (dext_kfd_queue_destroy(q)) kept = 1;
    }
    return kept;
}

int dext_kfd_settle(dext_kfd_client *c, unsigned int wait_ms)
{
    if (!c) return -EINVAL;
    // The session retries its pending copy and every failed queue first;
    // then retained records whose queue it recovered can go.
    (void)rt_kfd_session_settle(c->session, wait_ms);
    int kept = 0;
    for (dext_kfd_queue *q = c->queues, *next; q; q = next) {
        next = q->next;
        if (q->retained && dext_kfd_queue_destroy(q)) kept = 1;
    }
    c->uncertain = kept || rt_kfd_session_uncertain(c->session);
    return c->uncertain ? -EBUSY : 0;
}

int dext_kfd_close(dext_kfd_client *c)
{
    if (!c) return -EINVAL;
    // A dying client's queues and memory, as Linux tears down a process
    // that dies with live queues: every queue record first (the session
    // destroys their KFD queues, recovering any MES does not confirm),
    // then the session, which retries whatever is still uncertain. Each
    // step is bounded; a client whose GPU work never let go is kept.
    int kept = destroy_queue_records(c);
    int r = rt_kfd_session_close(c->session);
    if (kept || r) { c->uncertain = true; return -EBUSY; }
    IOFree(c, sizeof(*c));
    return 0;
}

int dext_kfd_info(dext_kfd_client *c, struct dext_kfd_info *out)
{
    struct rt_kfd_queue_limits limits{};
    if (!c || !out) return -EINVAL;
    *out = {};
    int r = rt_kfd_session_queue_limits(c->session, &limits);
    if (!r) r = rt_kfd_session_window(c->session, &out->window_base, &out->window_size);
    if (r) return r;
    out->pid = rt_kfd_session_pid(c->session);
    out->slots = limits.slots;
    out->gpuvm_base = c->apertures.gpuvm_base;
    out->gpuvm_limit = c->apertures.gpuvm_limit;
    return 0;
}

int dext_kfd_set_window(dext_kfd_client *c, uint64_t base, uint64_t size)
{
    return c ? rt_kfd_session_set_window(c->session, base, size) : -EINVAL;
}

int dext_kfd_event_create(dext_kfd_client *c, uint32_t *id, uint32_t *trigger,
                          uint64_t *mailbox_va)
{
    if (!c || !id || !trigger || !mailbox_va) return -EINVAL;
    struct rt_kfd_event event = {};
    const int r = rt_kfd_event_create(c->session, &event);
    if (r) return r;
    *id = event.id;
    *trigger = event.trigger;
    *mailbox_va = event.mailbox_va;
    return 0;
}

int dext_kfd_event_destroy(dext_kfd_client *c, uint32_t id)
{
    return c ? rt_kfd_event_destroy(c->session, id) : -EINVAL;
}

int dext_kfd_event_set(dext_kfd_client *c, uint32_t id)
{
    return c ? rt_kfd_event_set(c->session, id) : -EINVAL;
}

int dext_kfd_wait_begin(dext_kfd_client *c, const uint32_t *ids, uint32_t count, int all,
                        uint32_t timeout_ms, struct rt_kfd_wait **out)
{
    return c ? rt_kfd_wait_begin(c->session, ids, count, all, timeout_ms, out) : -EINVAL;
}

int dext_kfd_queue_abi(struct dext_aql_limits *out)
{
    using namespace amdgpu;
    if (!out) return -EINVAL;
    *out = {};
    out->max_private_bytes = kAQLMaxPrivateBytes;
    out->min_packets = kAQLQueueMinPackets;
    out->max_packets = kAQLQueueMaxPackets;
    return 0;
}

int dext_kfd_bo_alloc(dext_kfd_client *c, uint64_t size, uint64_t alignment, uint32_t domain,
                      rt_kfd_bo **out, uint64_t *va)
{
    struct rt_kfd_bo_info info{};
    if (!c || !out || !va || (domain != 2 && domain != 3)) return -EINVAL;
    const bool vram = domain == 3;
    int r = rt_kfd_bo_alloc(c->session, size, alignment, vram ? RT_KFD_VRAM : RT_KFD_GTT,
                            vram ? RT_KFD_PLACE_PRIVATE : RT_KFD_PLACE_WINDOW, out);
    if (!r) r = rt_kfd_bo_info(c->session, *out, &info);
    if (!r) *va = info.va;
    return r;
}

int dext_kfd_bo_free(dext_kfd_client *c, rt_kfd_bo *bo)
{
    return c ? rt_kfd_bo_free(c->session, bo) : -EINVAL;
}
int dext_kfd_bo_read(dext_kfd_client *c, rt_kfd_bo *bo, uint64_t offset, void *dst, size_t bytes)
{
    return c ? rt_kfd_bo_read(c->session, bo, offset, dst, bytes) : -EINVAL;
}
int dext_kfd_bo_write(dext_kfd_client *c, rt_kfd_bo *bo, uint64_t offset, const void *src,
                      size_t bytes)
{
    return c ? rt_kfd_bo_write(c->session, bo, offset, src, bytes) : -EINVAL;
}
int dext_kfd_bo_copy(dext_kfd_client *c, rt_kfd_bo *src, uint64_t src_offset,
                     rt_kfd_bo *dst, uint64_t dst_offset, uint64_t bytes)
{
    return c ? rt_kfd_bo_copy(c->session, src, src_offset, dst, dst_offset, bytes) : -EINVAL;
}
int dext_kfd_bo_ranges(dext_kfd_client *c, rt_kfd_bo *bo,
                       int (*fn)(void *, void *, uint64_t), void *arg)
{
    return c ? rt_kfd_bo_cpu_ranges(c->session, bo, fn, arg) : -EINVAL;
}

unsigned int dext_kfd_queue_count(dext_kfd_client *c)
{
    unsigned int n = 0;
    for (auto *q = c ? c->queues : nullptr; q; q = q->next) ++n;
    return n;
}

static uint64_t bo_va(dext_kfd_client *c, rt_kfd_bo *bo)
{
    struct rt_kfd_bo_info info{};
    return bo && !rt_kfd_bo_info(c->session, bo, &info) ? info.va : 0;
}

// The device fields of amd_queue_t a KFD process's queue needs: the flat
// apertures KFD programmed in SH_MEM_BASES for this process, the CU and
// wave limits, the inactive signal and no scratch yet.
static void fill_device_fields(dext_kfd_client *c, amd_queue_t &m, uint64_t inactiveSignal)
{
    m.compute_tmpring_size = 0;
    memset(m.scratch_resource_descriptor, 0, sizeof(m.scratch_resource_descriptor));
    m.scratch_backing_memory_location = 0;
    m.scratch_wave64_lane_byte_size = 0;
    m.group_segment_aperture_base_hi = uint32_t(c->apertures.lds_base >> 32);
    m.private_segment_aperture_base_hi = uint32_t(c->apertures.scratch_base >> 32);
    m.max_cu_id = c->geometry.compute_units - 1;
    m.max_wave_id = c->geometry.waves_per_cu - 1;
    m.queue_inactive_signal.handle = inactiveSignal;
}

static int scratch_allocate(dext_kfd_queue *q, amd_queue_t &m, uint32_t laneBytes)
{
    using namespace amdgpu;
    dext_kfd_client *c = q->client;
    const auto &g = c->geometry;
    const IPVersion ip{uint8_t(g.gfx_major), uint8_t(g.gfx_minor), uint8_t(g.gfx_revision)};
    const TmpRingLayout tmpring{g.tmpring.waves_mask, g.tmpring.waves_shift,
                                g.tmpring.wave_size_mask, g.tmpring.wave_size_shift};
    uint32_t aligned = 0, waves = 0;
    uint64_t bytes = 0;
    if (!aql_scratch_geometry(ip, tmpring, laneBytes, g.compute_units, g.engines,
                              g.waves_per_cu, aligned, waves, bytes)) return -EINVAL;
    rt_kfd_bo *replacement = nullptr;
    uint64_t va = 0;
    for (; waves >= kAQLScratchMinimumWavesPerEngine; waves -= kAQLScratchMinimumWavesPerEngine) {
        bytes = uint64_t(aligned) * 64 * waves * g.engines;
        if (bytes <= UINT32_MAX &&
            !dext_kfd_bo_alloc(c, bytes, 16384, 3, &replacement, &va)) break;
        replacement = nullptr;
    }
    if (!replacement) return -ENOMEM;
    amd_queue_t updated = m;
    if (!aql_scratch_metadata(ip, tmpring, updated, va, bytes, aligned, g.engines, waves)) {
        (void)rt_kfd_bo_free(c->session, replacement);
        return -EINVAL;
    }
    // Only the scratch fields change; the runtime owns the rest.
    int r = rt_kfd_bo_write(c->session, q->metadata,
                            offsetof(amd_queue_t, scratch_resource_descriptor),
                            updated.scratch_resource_descriptor,
                            sizeof(updated.scratch_resource_descriptor));
    if (!r) r = rt_kfd_bo_write(c->session, q->metadata,
                                offsetof(amd_queue_t, scratch_backing_memory_location),
                                &updated.scratch_backing_memory_location,
                                sizeof(updated.scratch_backing_memory_location));
    if (!r) r = rt_kfd_bo_write(c->session, q->metadata,
                                offsetof(amd_queue_t, scratch_wave64_lane_byte_size),
                                &updated.scratch_wave64_lane_byte_size,
                                sizeof(updated.scratch_wave64_lane_byte_size));
    if (!r) r = rt_kfd_bo_write(c->session, q->metadata,
                                offsetof(amd_queue_t, compute_tmpring_size),
                                &updated.compute_tmpring_size,
                                sizeof(updated.compute_tmpring_size));
    if (r) {
        (void)rt_kfd_bo_free(c->session, replacement);
        return r;
    }
    m = updated;
    if (q->scratch && rt_kfd_bo_free(c->session, q->scratch)) {
        // The old scratch stays with the process; KFD frees it at exit.
    }
    q->scratch = replacement;
    return 0;
}

int dext_kfd_queue_create(dext_kfd_client *c, rt_kfd_bo *ring, rt_kfd_bo *metadata,
                          uint32_t packets, dext_kfd_queue **out)
{
    using namespace amdgpu;
    struct rt_kfd_bo_info ri{}, mi{};
    if (!out || !c || !ring || !metadata || ring == metadata ||
        packets < kAQLQueueMinPackets || packets > kAQLQueueMaxPackets ||
        (packets & (packets - 1))) return -EINVAL;
    *out = nullptr;
    if (dext_kfd_uncertain(c)) return -EBUSY;
    if (rt_kfd_bo_info(c->session, ring, &ri) || rt_kfd_bo_info(c->session, metadata, &mi) ||
        ri.domain != RT_KFD_GTT || mi.domain != RT_KFD_GTT ||
        ri.size < uint64_t(packets) * 64 || mi.size < sizeof(amd_queue_t)) return -EINVAL;
    amd_queue_t m{};
    int r = rt_kfd_bo_read(c->session, metadata, 0, &m, sizeof(m));
    if (r) return r;
    // The runtime's freshly initialized queue (as dext_aql_create checks).
    if (m.write_dispatch_id || m.read_dispatch_id || m.hsa_queue.size != packets ||
        uintptr_t(m.hsa_queue.base_address) != ri.va ||
        m.read_dispatch_id_field_base_byte_offset != offsetof(amd_queue_t, read_dispatch_id) ||
        m.caps || m.queue_properties != AMD_QUEUE_PROPERTIES_IS_PTR64 ||
        m.scratch_wave64_lane_byte_size > kAQLMaxPrivateBytes) return -EINVAL;
    auto *q = static_cast<dext_kfd_queue *>(IOMallocZero(sizeof(dext_kfd_queue)));
    if (!q) return -ENOMEM;
    q->client = c; q->ring = ring; q->metadata = metadata; q->packets = packets;
    q->ringVA = ri.va; q->metadataVA = mi.va;
    // The inactive signal's page: host memory the CP writes and the dext
    // reads, never mapped by the client, so it lives in the private range.
    r = rt_kfd_bo_alloc(c->session, kAQLStorageBytes, kAQLStorageBytes, RT_KFD_GTT,
                        RT_KFD_PLACE_PRIVATE, &q->storage);
    q->storageVA = r ? 0 : bo_va(c, q->storage);
    if (!r && !q->storageVA) r = -EFAULT;
    if (!r) {
        amd_signal_t inactive{};
        inactive.kind = AMD_SIGNAL_KIND_USER;
        r = rt_kfd_bo_write(c->session, q->storage, kAQLInactiveOffset, &inactive, sizeof(inactive));
    }
    if (!r) {
        const uint32_t requested = m.scratch_wave64_lane_byte_size;
        fill_device_fields(c, m, q->storageVA + kAQLInactiveOffset);
        if (requested) r = scratch_allocate(q, m, requested);
    }
    // The fields from compute_tmpring_size through queue_inactive_signal
    // are the dext's; write the updated block back in one piece.
    if (!r) r = rt_kfd_bo_write(c->session, metadata, 0, &m, sizeof(m));
    if (!r) {
        struct rt_kfd_queue_desc desc{};
        desc.ring = ring;
        desc.ring_bytes = packets * 64;
        desc.read_pointer = mi.va + offsetof(amd_queue_t, read_dispatch_id);
        desc.write_pointer = mi.va + offsetof(amd_queue_t, write_dispatch_id);
        r = rt_kfd_queue_create(c->session, &desc, &q->queue);
    }
    if (r) {
        if (q->scratch) (void)rt_kfd_bo_free(c->session, q->scratch);
        if (q->storage) (void)rt_kfd_bo_free(c->session, q->storage);
        IOFree(q, sizeof(*q));
        return errno_of(r);
    }
    q->next = c->queues;
    c->queues = q;
    *out = q;
    return 0;
}

int dext_kfd_queue_kick(dext_kfd_queue *q, uint64_t packet)
{
    if (!q || q->retained) return -ENODEV;
    dext_kfd_client *c = q->client;
    uint64_t write = 0;
    int r = rt_kfd_bo_read(c->session, q->metadata, offsetof(amd_queue_t, write_dispatch_id),
                           &write, sizeof(write));
    if (r) return r;
    if (packet == UINT64_MAX || packet >= write) return -EINVAL;
    if (q->published && packet <= q->lastDoorbell) return 0;
    r = rt_kfd_queue_kick(c->session, q->queue, packet);
    if (!r) { q->published = true; q->lastDoorbell = packet; }
    return r;
}

int dext_kfd_fault(dext_kfd_client *c, uint32_t *flags, uint64_t *va)
{
    struct rt_kfd_fault fault{};
    if (!c || !flags || !va) return -EINVAL;
    *flags = 0;
    *va = 0;
    const int r = rt_kfd_session_fault(c->session, &fault);
    if (r != 1) return r;
    *flags = DEXT_KFD_FAULT_VALID |
             (fault.not_present ? DEXT_KFD_FAULT_NOT_PRESENT : 0) |
             (fault.read_only ? DEXT_KFD_FAULT_READ_ONLY : 0) |
             (fault.no_execute ? DEXT_KFD_FAULT_NO_EXECUTE : 0) |
             (fault.imprecise ? DEXT_KFD_FAULT_IMPRECISE : 0);
    *va = fault.va;
    return 1;
}

int dext_kfd_queue_service(dext_kfd_queue *q, uint64_t *inactive)
{
    using namespace amdgpu;
    if (!q || !inactive || q->retained) return -ENODEV;
    dext_kfd_client *c = q->client;
    *inactive = 0;
    // A GPU memory fault of the process: its queues are off the GPU for
    // good. Reported before anything else, on the poll the runtime makes.
    int r = rt_kfd_session_fault(c->session, nullptr);
    if (r == 1) return -EFAULT;
    if (r) return r;
    r = rt_kfd_bo_read(c->session, q->storage, kAQLInactiveOffset + 8, inactive, 8);
    if (r || !*inactive) return r;
    if (!(*inactive & 0x401) || (*inactive & ~uint64_t(0x401))) return -EIO;
    amd_queue_t m{};
    r = rt_kfd_bo_read(c->session, q->metadata, 0, &m, sizeof(m));
    if (r) return r;
    const uint64_t read = m.read_dispatch_id, write = m.write_dispatch_id;
    if (write <= read) return -EIO;
    const uint64_t available = write - read < q->packets ? write - read : q->packets;
    uint32_t required = 0;
    for (uint64_t i = 0; i < available; ++i) {
        hsa_kernel_dispatch_packet_t packet{};
        r = rt_kfd_bo_read(c->session, q->ring, ((read + i) & (q->packets - 1)) * 64,
                           &packet, sizeof(packet));
        if (r) return r;
        const uint16_t type = packet.header & 255;
        if (type == HSA_PACKET_TYPE_INVALID) break;
        if (type == HSA_PACKET_TYPE_KERNEL_DISPATCH && packet.private_segment_size) {
            required = packet.private_segment_size;
            break;
        }
    }
    if (!required) return -EINVAL;
    r = scratch_allocate(q, m, required);
    if (r) return r;
    const uint64_t zero = 0;
    r = rt_kfd_bo_write(c->session, q->storage, kAQLInactiveOffset + 8, &zero, 8);
    if (!r) *inactive = 0;
    return r;
}

int dext_kfd_queue_destroy(dext_kfd_queue *q)
{
    if (!q) return -EBUSY;
    dext_kfd_client *c = q->client;
    // A retained queue is retried: the session recovers one MES did not
    // confirm removing, or finds it already recovered by a settle.
    int r = rt_kfd_queue_destroy(c->session, q->queue);
    if (r) {
        // MES may still run it: keep the queue and everything it uses.
        q->retained = true;
        c->uncertain = true;
        return r;
    }
    q->retained = false;
    if (q->scratch) (void)rt_kfd_bo_free(c->session, q->scratch);
    if (q->storage) (void)rt_kfd_bo_free(c->session, q->storage);
    for (dext_kfd_queue **link = &c->queues; *link; link = &(*link)->next)
        if (*link == q) { *link = q->next; break; }
    IOFree(q, sizeof(*q));
    return 0;
}

// ---- bounded launches ----

static int launch_buffers(dext_kfd_client *c)
{
    using namespace amdgpu;
    int r = 0;
    // Ring, metadata and storage are separate BOs: KFD requires the ring and
    // the pointer page to start their own mappings.
    if (!c->launch_ring)
        r = rt_kfd_bo_alloc(c->session, kAQLRingBytes, 0, RT_KFD_GTT, RT_KFD_PLACE_PRIVATE,
                            &c->launch_ring);
    if (!r && !c->launch_metadata)
        r = rt_kfd_bo_alloc(c->session, sizeof(amd_queue_t), 0, RT_KFD_GTT,
                            RT_KFD_PLACE_PRIVATE, &c->launch_metadata);
    if (!r && !c->launch_storage)
        r = rt_kfd_bo_alloc(c->session, kAQLStorageBytes, kAQLStorageBytes, RT_KFD_GTT,
                            RT_KFD_PLACE_PRIVATE, &c->launch_storage);
    return r;
}

// One AQL dispatch on a KFD queue of the client's process: build(staging,
// base) writes the 16 KiB storage image (dext_aql's layout) for storage VA
// base; its ring and metadata regions move to their own BOs here.
template <typename Build>
static int bounded_run(dext_kfd_client *c, uint32_t timeoutUS, Build build,
                       int (*before_map)(void *), void *arg, uint64_t out[5], int *uncertain)
{
    using namespace amdgpu;
    if (dext_kfd_uncertain(c)) return -EBUSY;
    int r = launch_buffers(c);
    if (r) return r == -ENOMEM ? -ENOSPC : r;
    const uint64_t storage = bo_va(c, c->launch_storage);
    const uint64_t ring = bo_va(c, c->launch_ring);
    const uint64_t metadata = bo_va(c, c->launch_metadata);
    auto *staging = static_cast<uint8_t *>(aligned_alloc(64, kAQLStorageBytes));
    if (!staging) return -ENOMEM;
    if (!storage || !ring || !metadata || !build(staging, storage)) {
        free(staging);
        return -EINVAL;
    }
    auto &m = *reinterpret_cast<amd_queue_t *>(staging + kAQLMetadataOffset);
    m.hsa_queue.base_address = reinterpret_cast<void *>(ring);
    m.group_segment_aperture_base_hi = uint32_t(c->apertures.lds_base >> 32);
    m.private_segment_aperture_base_hi = uint32_t(c->apertures.scratch_base >> 32);
    out[2] = 1;
    r = rt_kfd_bo_write(c->session, c->launch_storage, 0, staging, kAQLStorageBytes);
    if (!r) r = rt_kfd_bo_write(c->session, c->launch_ring, 0, staging + kAQLRingOffset, kAQLRingBytes);
    if (!r) r = rt_kfd_bo_write(c->session, c->launch_metadata, 0, &m, sizeof(m));
    free(staging);
    if (!r && before_map) r = before_map(arg);
    if (r) { out[0] = uint64_t(-r); return r; }

    struct rt_kfd_queue_desc desc{};
    desc.ring = c->launch_ring;
    desc.ring_bytes = kAQLRingBytes;
    desc.read_pointer = metadata + offsetof(amd_queue_t, read_dispatch_id);
    desc.write_pointer = metadata + offsetof(amd_queue_t, write_dispatch_id);
    rt_kfd_queue *queue = nullptr;
    out[2] = 2;
    r = rt_kfd_queue_create(c->session, &desc, &queue);
    if (r) { out[0] = uint64_t(-r); return r == -ENOSPC ? -ENOSPC : errno_of(r); }
    out[2] = 3;
    const uint64_t writeIndex = 1;
    r = rt_kfd_bo_write(c->session, c->launch_metadata, offsetof(amd_queue_t, write_dispatch_id),
                        &writeIndex, sizeof(writeIndex));
    if (!r) r = rt_kfd_queue_kick(c->session, queue, 0);
    if (!r) {
        const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + uint64_t(timeoutUS) * 1000;
        for (;;) {
            r = rt_kfd_bo_read(c->session, c->launch_storage, kAQLCompletionOffset + 8, &out[1], 8);
            if (!r) r = rt_kfd_bo_read(c->session, c->launch_storage, kAQLInactiveOffset + 8, &out[3], 8);
            if (r) break;
            if (out[3] || (out[1] != 0 && out[1] != 1)) { r = -EIO; break; }
            if (out[1] == 0) break;
            if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) { r = -ETIMEDOUT; break; }
            IOSleep(1);
        }
    }
    // DESTROY_QUEUE preempts the queue off MES whatever it was doing, so
    // the launch buffers are reusable unless that fails.
    out[2] = 4;
    const int destroyed = rt_kfd_queue_destroy(c->session, queue);
    if (destroyed) {
        *uncertain = 1;
        c->uncertain = true;
        out[0] = uint64_t(-destroyed);
        return destroyed;
    }
    if (!r) r = rt_kfd_bo_read(c->session, c->launch_metadata, offsetof(amd_queue_t, read_dispatch_id),
                               &out[4], sizeof(out[4]));
    if (!r && out[4] != 1) r = -EIO;
    if (r) { out[0] = uint64_t(-r); return r; }
    out[2] = 5;
    return 0;
}

int dext_kfd_dispatch_bounded(dext_kfd_client *c, uint64_t descriptorVA, uint64_t kernargVA,
                              const void *request, size_t request_size,
                              uint64_t out[5], int *uncertain)
{
    using namespace amdgpu;
    if (!out || !uncertain || !c || !request || request_size != sizeof(AQLDispatchRequest))
        return -EINVAL;
    *uncertain = 0;
    out[0] = 0; out[1] = UINT64_MAX; out[2] = 0; out[3] = 0; out[4] = 0;
    AQLDispatchRequest launch{};
    memcpy(&launch, request, sizeof(launch));
    if (!aql_dispatch_shape(launch)) return -EINVAL;
    const auto &g = c->geometry;
    return bounded_run(c, launch.timeoutUS,
        [&](uint8_t *staging, uint64_t base) {
            return aql_build_storage(staging, base, descriptorVA, kernargVA, launch,
                                     g.compute_units, g.waves_per_cu);
        }, nullptr, nullptr, out, uncertain);
}

int dext_kfd_dispatch_code(dext_kfd_client *c, uint64_t codeVA, const void *request,
                           size_t request_size, int (*before_map)(void *), void *arg,
                           uint64_t out[5], int *uncertain)
{
    using namespace amdgpu;
    if (!out || !uncertain || !c || !request || request_size != sizeof(ComputeDispatchRequest))
        return -EINVAL;
    *uncertain = 0;
    out[0] = 0; out[1] = UINT64_MAX; out[2] = 0; out[3] = 0; out[4] = 0;
    ComputeDispatchRequest raw{};
    memcpy(&raw, request, sizeof(raw));
    struct rt_compute_topology t{};
    const bool clampIEEE = !rt_device_topology(rt_compute_device(c->ctx), &t) &&
        t.gfx_target_version && t.gfx_target_version < 120000;
    AQLCodeLaunch launch{};
    if (!aql_code_launch(raw, codeVA, clampIEEE, launch)) return -EINVAL;
    const auto &g = c->geometry;
    return bounded_run(c, raw.timeoutUS,
        [&](uint8_t *staging, uint64_t base) {
            return aql_build_code_storage(staging, base, launch, g.compute_units, g.waves_per_cu);
        }, before_map, arg, out, uncertain);
}
