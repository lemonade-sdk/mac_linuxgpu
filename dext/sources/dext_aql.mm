/* AQL metadata follows the host ABI; hardware mapping uses upstream MES. */
#include "dext_aql.h"
#include <rt/compute.h>
#include <rt/queue.h>
#include "../amdgpu/amdgpu_aql_packets.h"
#include <DriverKit/IOLib.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

struct dext_aql_queue {
    rt_compute_ctx *ctx;
    rt_compute_bo *ring, *metadata, *storage, *scratch, *orphan_scratch;
    struct rt_queue_geometry geometry;
    uint32_t slot, doorbell, packets;
    uint64_t storageVA, lastDoorbell;
    uint64_t ringVA, metadataVA;       // for mapping again after a device reset
    amd_queue_t *cpu;
    const hsa_kernel_dispatch_packet_t *packetsCPU;
    bool mapped, retained, published;
    bool resetLost;                    // unmapped by a device reset, not yet restored
    dext_aql_queue *next;              // aql_live
};

// The persistent queues MES maps, for a device reset's hooks, which run on
// the reset domain's thread while the owner queue serves its calls.
// aql_lock guards the list and each listed queue's mapping state. It is
// held across the bounded steps that change that state (an MES map or
// unmap, a storage write through SDMA) and never while waiting for
// anything the reset itself must finish. Made by dext_aql_available,
// before any queue exists; never freed.
static IOLock *aql_lock;
static dext_aql_queue *aql_live;

static void aql_unlink_locked(dext_aql_queue *q)
{
    for (dext_aql_queue **at=&aql_live; *at; at=&(*at)->next)
        if (*at==q) {*at=q->next;q->next=nullptr;return;}
}

struct aql_guard {
    // Without the lock no queue exists (dext_aql_create refuses).
    aql_guard() {if (aql_lock) IOLockLock(aql_lock);}
    ~aql_guard() {if (aql_lock) IOLockUnlock(aql_lock);}
    aql_guard(const aql_guard &)=delete;
    aql_guard &operator=(const aql_guard &)=delete;
};

// The storage block reserves kAQLMQDBytes for the MQD upstream KFD builds.
static int geometry_supported(struct amdgpu_device *adev, struct rt_queue_geometry *geometry)
{
    int r = rt_queue_geometry(adev, geometry);
    if (!r && geometry->mqd_bytes > amdgpu::kAQLMQDBytes) r = -EOPNOTSUPP;
    return r;
}

static struct rt_queue_mqd mqd_layout(uint64_t storage, uint64_t ring, uint32_t packets,
                                      uint64_t metadata)
{
    using namespace amdgpu;
    struct rt_queue_mqd d{};
    d.mqd_address = storage;
    d.ring_address = ring;
    d.ring_bytes = packets * 64;
    d.read_pointer = metadata + offsetof(amd_queue_t, read_dispatch_id);
    d.write_pointer = metadata + offsetof(amd_queue_t, write_dispatch_id);
    d.eop_address = storage + kAQLEOPOffset;
    d.eop_bytes = kAQLEOPBytes;
    return d;
}

int dext_aql_available(rt_compute_ctx *ctx)
{
    struct rt_queue_geometry geometry{};
    if (!aql_lock && !(aql_lock = IOLockAlloc())) return -ENOMEM;
    int r = rt_compute_status(ctx);
    if (!r) r = geometry_supported(rt_compute_device(ctx), &geometry);
    // Fix the HQD/doorbell split with MES and KFD before any queue exists.
    return r ? r : rt_queue_partition(rt_compute_device(ctx), nullptr);
}

int dext_aql_limits(rt_compute_ctx *ctx, struct dext_aql_limits *out)
{
    using namespace amdgpu;
    struct rt_queue_partition partition{};
    if (!out) return -EINVAL;
    *out = {};
    int r = dext_aql_available(ctx);
    if (!r) r = rt_queue_partition(rt_compute_device(ctx), &partition);
    if (r) return r;
    out->max_private_bytes = kAQLMaxPrivateBytes;
    out->min_packets = kAQLQueueMinPackets;
    out->max_packets = kAQLQueueMaxPackets;
    for (unsigned i = 0; i < RT_QUEUE_MASK_WORDS; ++i)
        out->slots += uint32_t(__builtin_popcountll(partition.dext_mask[i]));
    return 0;
}

int dext_aql_uncertain(const dext_aql_queue *q)
{
    if (!q) return 0;
    bool retained;
    {
        aql_guard guard;
        retained=q->retained;
    }
    return retained || rt_compute_status(q->ctx) == -EBUSY;
}

static int scratch_allocate(dext_aql_queue *q, uint32_t laneBytes)
{
    using namespace amdgpu;
    const IPVersion ip{uint8_t(q->geometry.gfx_major), uint8_t(q->geometry.gfx_minor),
                       uint8_t(q->geometry.gfx_revision)};
    const TmpRingLayout tmpring{q->geometry.tmpring.waves_mask, q->geometry.tmpring.waves_shift,
                                q->geometry.tmpring.wave_size_mask,
                                q->geometry.tmpring.wave_size_shift};
    uint32_t aligned=0, waves=0; uint64_t bytes=0;
    if (!aql_scratch_geometry(ip,tmpring,laneBytes,q->geometry.compute_units,
        q->geometry.engines,q->geometry.waves_per_cu,aligned,waves,bytes)) return -22;
    rt_compute_bo *replacement=nullptr;
    for (;waves>=kAQLScratchMinimumWavesPerEngine;waves-=kAQLScratchMinimumWavesPerEngine) {
        bytes=uint64_t(aligned)*64*waves*q->geometry.engines;
        if (bytes<=UINT32_MAX && !rt_compute_bo_alloc(q->ctx,bytes,16384,
                                                     RT_COMPUTE_VRAM,&replacement)) break;
    }
    if (!replacement) return -12;
    struct rt_compute_bo_info info{};
    int r=rt_compute_bo_info(q->ctx,replacement,&info);
    if (r || !aql_scratch_metadata(ip,tmpring,*q->cpu,info.gpu_address,bytes,aligned,
                                  q->geometry.engines,waves)) {
        if (rt_compute_bo_free(q->ctx,replacement)) {
            q->orphan_scratch=replacement;
            q->retained=true;
        }
        return r ? r : -22;
    }
    if (q->scratch) {
        r=rt_compute_bo_free(q->ctx,q->scratch);
        if (r) {
            q->orphan_scratch=replacement;
            q->retained=true;
            return r;
        }
    }
    q->scratch=replacement;
    return 0;
}

int dext_aql_create(rt_compute_ctx *ctx, rt_compute_bo *ring,
                    rt_compute_bo *metadata, uint32_t packets, dext_aql_queue **out)
{
    using namespace amdgpu;
    if (!out || !ctx || !ring || !metadata || ring==metadata || packets<kAQLQueueMinPackets ||
        packets>kAQLQueueMaxPackets || (packets&(packets-1))) return -22;
    *out=nullptr;
    if (!aql_lock) return -19;    // dext_aql_available has not run
    auto *q=static_cast<dext_aql_queue *>(IOMallocZero(sizeof(dext_aql_queue)));
    if (!q) return -12;
    q->ctx=ctx; q->ring=ring; q->metadata=metadata; q->packets=packets;
    auto *adev=rt_compute_device(ctx);
    struct rt_compute_bo_info ri{},mi{},si{};
    int r=geometry_supported(adev,&q->geometry);
    if (!r) r=rt_compute_bo_info(ctx,ring,&ri);
    if (!r) r=rt_compute_bo_info(ctx,metadata,&mi);
    if (!r && (!ri.shared_descriptor_available || !mi.shared_descriptor_available ||
        ri.size<uint64_t(packets)*64 || mi.size<sizeof(amd_queue_t) ||
        !ri.cpu_address || !mi.cpu_address || (uintptr_t(mi.cpu_address)&63) ||
        (uintptr_t(ri.cpu_address)&(alignof(hsa_kernel_dispatch_packet_t)-1)))) r=-22;
    if (r) {IOFree(q,sizeof(*q));return r;}
    q->cpu=static_cast<amd_queue_t *>(mi.cpu_address);
    q->packetsCPU=static_cast<const hsa_kernel_dispatch_packet_t *>(ri.cpu_address);
    auto &m=*q->cpu;
    if (__atomic_load_n(&m.write_dispatch_id,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&m.read_dispatch_id,__ATOMIC_ACQUIRE) ||
        m.hsa_queue.size!=packets || uintptr_t(m.hsa_queue.base_address)!=ri.gpu_address ||
        m.read_dispatch_id_field_base_byte_offset!=offsetof(amd_queue_t,read_dispatch_id) ||
        m.caps || m.queue_properties!=AMD_QUEUE_PROPERTIES_IS_PTR64 ||
        m.scratch_wave64_lane_byte_size>kAQLMaxPrivateBytes) {IOFree(q,sizeof(*q));return -22;}
    r=rt_queue_reserve(adev,&q->slot,&q->doorbell);
    if (r) {IOFree(q,sizeof(*q));return r;}
    r=rt_compute_bo_alloc(ctx,kAQLStorageBytes,kAQLStorageBytes,RT_COMPUTE_VRAM,&q->storage);
    if (!r) r=rt_compute_bo_info(ctx,q->storage,&si);
    auto *staging=static_cast<uint8_t *>(aligned_alloc(64,kAQLStorageBytes));
    if (staging) memset(staging,0,kAQLStorageBytes);
    if (!r && !staging) r=-12;
    if (!r) {
        const struct rt_queue_mqd layout=mqd_layout(si.gpu_address,ri.gpu_address,packets,
                                                    mi.gpu_address);
        r=rt_queue_build_mqd(adev,q->slot,q->doorbell,&layout,staging,kAQLMQDBytes);
    }
    if (!r) {
        q->storageVA=si.gpu_address;
        q->ringVA=ri.gpu_address;
        q->metadataVA=mi.gpu_address;
        reinterpret_cast<amd_signal_t *>(staging+kAQLInactiveOffset)->kind=AMD_SIGNAL_KIND_USER;
        uint32_t requested=m.scratch_wave64_lane_byte_size;
        m.compute_tmpring_size=0; memset(m.scratch_resource_descriptor,0,sizeof(m.scratch_resource_descriptor));
        m.scratch_backing_memory_location=0; m.scratch_wave64_lane_byte_size=0;
        m.group_segment_aperture_base_hi=kAQLGroupApertureHi;
        m.private_segment_aperture_base_hi=kAQLPrivateApertureHi;
        if (requested) r=scratch_allocate(q,requested);
        m.max_cu_id=q->geometry.compute_units-1; m.max_wave_id=q->geometry.waves_per_cu-1;
        m.queue_inactive_signal.handle=si.gpu_address+kAQLInactiveOffset;
        if (!r) r=rt_compute_bo_write(ctx,q->storage,0,staging,kAQLStorageBytes);
    }
    free(staging);
    if (r) {
        bool held=false;
        if (q->scratch) {
            if (rt_compute_bo_free(ctx,q->scratch)) held=true;
            else q->scratch=nullptr;
        }
        if (q->orphan_scratch) {
            if (rt_compute_bo_free(ctx,q->orphan_scratch)) held=true;
            else q->orphan_scratch=nullptr;
        }
        if (q->storage) {
            if (rt_compute_bo_free(ctx,q->storage)) held=true;
            else q->storage=nullptr;
        }
        if (held) {q->retained=true;*out=q;return r;}
        rt_queue_release(adev,q->slot);IOFree(q,sizeof(*q));return r;
    }
    q->retained=true;
    {
        // Mapped and listed together: a device reset sees every mapped queue.
        aql_guard guard;
        rt_queue_flush(adev);
        r=rt_queue_map(adev,q->slot,q->doorbell,si.gpu_address,
                       mi.gpu_address+offsetof(amd_queue_t,write_dispatch_id));
        if (!r) {q->mapped=true;q->retained=false;q->next=aql_live;aql_live=q;}
    }
    *out=q; // Keep every referenced BO on a missing firmware acknowledgement.
    return r;
}

// A device reset (rt/recovery.h's queue hooks), as upstream halts the GPU:
// every mapped queue goes with MES's state.
void dext_aql_reset_prepare(void)
{
    aql_guard guard;
    for (dext_aql_queue *q=aql_live; q; q=q->next)
        if (q->mapped) {q->mapped=false;q->resetLost=true;}
}

// After the device reset, with the schedulers running again: the queue's
// storage block is written again (its MQD, built afresh by upstream KFD's
// manager; the inactive signal; the EOP buffer zeroed: VRAM may be lost)
// and the queue is mapped where its producer is. Packets it had not
// finished were lost with the GPU's state, as a device reset ends every
// kernel ring's unfinished jobs; the reset generation (QueryInfo "LRST")
// tells clients. Under aql_lock.
static int reset_restore_locked(dext_aql_queue *q)
{
    using namespace amdgpu;
    auto *adev=rt_compute_device(q->ctx);
    auto *staging=static_cast<uint8_t *>(aligned_alloc(64,kAQLStorageBytes));
    if (!staging) return -12;
    memset(staging,0,kAQLStorageBytes);
    const uint64_t position=__atomic_load_n(&q->cpu->write_dispatch_id,__ATOMIC_ACQUIRE);
    const struct rt_queue_mqd layout=mqd_layout(q->storageVA,q->ringVA,q->packets,q->metadataVA);
    int r=rt_queue_build_mqd(adev,q->slot,q->doorbell,&layout,staging,kAQLMQDBytes);
    if (!r) r=rt_queue_mqd_set_position(staging,uint64_t(q->packets)*64,position);
    if (!r) {
        reinterpret_cast<amd_signal_t *>(staging+kAQLInactiveOffset)->kind=AMD_SIGNAL_KIND_USER;
        r=rt_compute_bo_write(q->ctx,q->storage,0,staging,kAQLStorageBytes);
    }
    free(staging);
    if (r) return r;
    // The reader starts at the producer: nothing before it will run.
    __atomic_store_n(&q->cpu->read_dispatch_id,position,__ATOMIC_RELEASE);
    q->published=false;
    q->lastDoorbell=0;
    rt_queue_flush(adev);
    return rt_queue_map(adev,q->slot,q->doorbell,q->storageVA,
                        q->metadataVA+offsetof(amd_queue_t,write_dispatch_id));
}

// A queue that cannot be restored is retained (dext_aql_uncertain): its
// owner's next call freezes the session, as after any failed MES call.
int dext_aql_reset_restore(void)
{
    aql_guard guard;
    int failed=0;
    for (dext_aql_queue *q=aql_live; q; q=q->next) {
        if (!q->resetLost || q->retained) continue;
        if (reset_restore_locked(q)) {q->retained=true;++failed;continue;}
        q->resetLost=false;
        q->mapped=true;
    }
    return failed;
}

int dext_aql_kick(dext_aql_queue *q,uint64_t packet)
{
    if (!q) return -19;
    aql_guard guard;
    if (!q->mapped || q->retained) return -19;
    if (packet==UINT64_MAX || packet>=__atomic_load_n(&q->cpu->write_dispatch_id,__ATOMIC_ACQUIRE)) return -22;
    if (q->published && packet<=q->lastDoorbell) return 0;
    int r=rt_queue_kick(rt_compute_device(q->ctx),q->doorbell,packet);
    if (!r) {q->published=true;q->lastDoorbell=packet;}
    return r;
}

int dext_aql_service(dext_aql_queue *q,uint64_t *inactive)
{
    using namespace amdgpu;
    if (!q || !inactive) return -19;
    aql_guard guard;
    if (!q->mapped || q->retained) return -19;
    int r=rt_compute_bo_read(q->ctx,q->storage,kAQLInactiveOffset+8,inactive,8);
    if (r || !*inactive) return r;
    if (!(*inactive&0x401) || (*inactive&~uint64_t(0x401))) return -5;
    uint64_t read=__atomic_load_n(&q->cpu->read_dispatch_id,__ATOMIC_ACQUIRE);
    uint64_t write=__atomic_load_n(&q->cpu->write_dispatch_id,__ATOMIC_ACQUIRE);
    if (write<=read) return -5;
    uint64_t available=write-read<q->packets ? write-read : q->packets;
    uint32_t required=0;
    for (uint64_t i=0;i<available;++i) {
        const auto &packet=q->packetsCPU[(read+i)&(q->packets-1)];
        uint16_t type=__atomic_load_n(&packet.header,__ATOMIC_ACQUIRE)&255;
        if (type==HSA_PACKET_TYPE_INVALID) break;
        if (type==HSA_PACKET_TYPE_KERNEL_DISPATCH && packet.private_segment_size) {
            required=packet.private_segment_size;break;
        }
    }
    if (!required) return -22;
    r=scratch_allocate(q,required);
    if (r) return r;
    rt_queue_flush(rt_compute_device(q->ctx));
    uint64_t zero=0;
    r=rt_compute_bo_write(q->ctx,q->storage,kAQLInactiveOffset+8,&zero,8);
    rt_queue_flush(rt_compute_device(q->ctx));
    if (!r) *inactive=0;
    return r;
}

int dext_aql_destroy(dext_aql_queue *q)
{
    if (!q) return -16;
    aql_guard guard;
    // A queue a device reset unmapped is no longer MES's: nothing to unmap.
    if (q->retained || (!q->mapped && !q->resetLost)) return -16;
    q->retained=true;
    auto *adev=rt_compute_device(q->ctx);
    int r=q->resetLost ? 0 : rt_queue_unmap(adev,q->slot,q->doorbell);
    if (r) return r;
    aql_unlink_locked(q);
    if (q->scratch) {
        r=rt_compute_bo_free(q->ctx,q->scratch);
        if (r) return r;
        q->scratch=nullptr;
    }
    r=rt_compute_bo_free(q->ctx,q->storage);
    if (r) return r;
    q->storage=nullptr;
    rt_queue_release(adev,q->slot);
    IOFree(q,sizeof(*q));
    return 0;
}

struct bounded_retained {
    bounded_retained *next;
    rt_compute_ctx *ctx;
    rt_compute_bo *storage;
    uint32_t slot, doorbell;
};
static bounded_retained *retained_dispatches;

// One owned VMID0 queue for a single dispatch: reserve an HQD, build the
// storage block (build(staging, base, geometry) writes ring, signals,
// metadata and packet), have upstream build the MQD, map, kick, wait for
// the completion signal, unmap. out[] as dext_aql_dispatch_bounded().
// before_map (optional) runs once the queue is fully prepared but not yet
// mapped; its failure releases everything like any pre-map failure.
template <typename Build>
static int bounded_run(rt_compute_ctx *ctx, uint32_t timeoutUS, Build build,
                       int (*before_map)(void *), void *arg,
                       uint64_t out[5], int *uncertain)
{
    using namespace amdgpu;
    int r = rt_compute_status(ctx);
    if (r) return r;
    auto *adev = rt_compute_device(ctx);
    struct rt_queue_geometry geometry{};
    r = geometry_supported(adev, &geometry);
    if (r) return r;

    auto *record = static_cast<bounded_retained *>(IOMallocZero(sizeof(bounded_retained)));
    if (!record) return -ENOMEM;
    record->ctx = ctx;
    r = rt_queue_reserve(adev, &record->slot, &record->doorbell);
    if (r) { IOFree(record, sizeof(*record)); return r; }

    struct rt_compute_bo_info info{};
    uint8_t *staging = nullptr;
    r = rt_compute_bo_alloc(ctx, kAQLStorageBytes, kAQLStorageBytes,
                            RT_COMPUTE_VRAM, &record->storage);
    if (!r) r = rt_compute_bo_info(ctx, record->storage, &info);
    if (!r && (info.domain != RT_COMPUTE_VRAM ||
               info.size < kAQLStorageBytes ||
               (info.gpu_address & (kAQLStorageBytes - 1)))) r = -EINVAL;
    if (!r) {
        staging = static_cast<uint8_t *>(aligned_alloc(64,kAQLStorageBytes));
        if (!staging) r = -ENOMEM;
    }
    if (!r && !build(staging, info.gpu_address, geometry)) r = -EINVAL;
    if (!r) {
        const struct rt_queue_mqd layout =
            mqd_layout(info.gpu_address, info.gpu_address + kAQLRingOffset,
                       kAQLRingBytes / 64, info.gpu_address + kAQLMetadataOffset);
        r = rt_queue_build_mqd(adev, record->slot, record->doorbell, &layout,
                               staging, kAQLMQDBytes);
    }
    if (!r) {
        out[2] = 1;
        r = rt_compute_bo_write(ctx, record->storage, 0, staging,
                                kAQLStorageBytes);
    }
    free(staging);
    if (!r && before_map) r = before_map(arg);
    if (r) goto before_map;

    rt_queue_flush(adev);
    out[2] = 2;
    r = rt_queue_map(adev, record->slot, record->doorbell, info.gpu_address,
                     info.gpu_address + kAQLMetadataOffset +
                     offsetof(amd_queue_t, write_dispatch_id));
    if (r) goto uncertain_exit;
    out[2] = 3;
    {
        const uint64_t writeIndex = 1;
        r = rt_compute_bo_write(ctx, record->storage,
                kAQLMetadataOffset + offsetof(amd_queue_t, write_dispatch_id),
                &writeIndex, sizeof(writeIndex));
    }
    if (r) goto uncertain_exit;
    rt_queue_flush(adev);
    r = rt_queue_kick(adev, record->doorbell, 0);
    if (r) goto uncertain_exit;

    {
        const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        const uint64_t deadline = start + uint64_t(timeoutUS) * 1000;
        do {
            r = rt_compute_bo_read(ctx, record->storage,
                    kAQLCompletionOffset + 8, &out[1], sizeof(out[1]));
            if (r) goto uncertain_exit;
            r = rt_compute_bo_read(ctx, record->storage,
                    kAQLInactiveOffset + 8, &out[3], sizeof(out[3]));
            if (r) goto uncertain_exit;
            if (out[3] || (out[1] != 0 && out[1] != 1)) {
                r = -EIO;
                goto uncertain_exit;
            }
            if (out[1] == 0) break;
            IOSleep(1);
        } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
        if (out[1] != 0) { r = -ETIMEDOUT; goto uncertain_exit; }
    }

    out[2] = 4;
    r = rt_queue_unmap(adev, record->slot, record->doorbell);
    if (r) goto uncertain_exit;
    rt_queue_flush(adev);
    r = rt_compute_bo_read(ctx, record->storage,
            kAQLMetadataOffset + offsetof(amd_queue_t, read_dispatch_id),
            &out[4], sizeof(out[4]));
    if (r || out[4] != 1) {
        if (!r) r = -EIO;
        goto uncertain_exit;
    }
    r = rt_compute_bo_free(ctx, record->storage);
    if (r) goto uncertain_exit;
    rt_queue_release(adev, record->slot);
    IOFree(record, sizeof(*record));
    out[2] = 5;
    return 0;

before_map:
    if (record->storage && rt_compute_bo_free(ctx, record->storage) != 0)
        goto uncertain_exit;
    rt_queue_release(adev, record->slot);
    IOFree(record, sizeof(*record));
    out[0] = uint64_t(-r);
    return r;

uncertain_exit:
    *uncertain = 1;
    out[0] = r < 0 ? uint64_t(-r) : uint64_t(r);
    record->next = retained_dispatches;
    retained_dispatches = record;
    return r;
}

int dext_aql_dispatch_bounded(rt_compute_ctx *ctx,
                              uint64_t descriptorVA, uint64_t kernargVA,
                              const void *request, size_t request_size,
                              uint64_t out[5], int *uncertain)
{
    using namespace amdgpu;
    if (!out || !uncertain || !ctx || !request ||
        request_size != sizeof(AQLDispatchRequest)) return -EINVAL;
    *uncertain = 0;
    out[0] = 0; out[1] = UINT64_MAX; out[2] = 0;
    out[3] = 0; out[4] = 0;
    AQLDispatchRequest launch{};
    memcpy(&launch, request, sizeof(launch));
    if (!aql_dispatch_shape(launch)) return -EINVAL;
    return bounded_run(ctx, launch.timeoutUS,
        [&](uint8_t *staging, uint64_t base, const struct rt_queue_geometry &g) {
            return aql_build_storage(staging, base, descriptorVA, kernargVA, launch,
                                     g.compute_units, g.waves_per_cu);
        }, nullptr, nullptr, out, uncertain);
}

int dext_aql_rsrc1_clamp_ieee(rt_compute_ctx *ctx)
{
    // GFX12 removed COMPUTE_PGM_RSRC1.DX10_CLAMP and IEEE_MODE (bits 21 and
    // 23 are WG_RR_EN and DISABLE_PERF in gc_12_0_0_sh_mask.h); earlier KFD
    // targets keep them, the rule the runtime's hasRsrc1ClampAndIEEE() uses.
    struct rt_compute_topology t{};
    return ctx && !rt_device_topology(rt_compute_device(ctx), &t) &&
           t.gfx_target_version && t.gfx_target_version < 120000;
}

int dext_aql_dispatch_code(rt_compute_ctx *ctx, uint64_t codeVA,
                           const void *request, size_t request_size,
                           int (*before_map)(void *), void *arg,
                           uint64_t out[5], int *uncertain)
{
    using namespace amdgpu;
    if (!out || !uncertain || !ctx || !request ||
        request_size != sizeof(ComputeDispatchRequest)) return -EINVAL;
    *uncertain = 0;
    out[0] = 0; out[1] = UINT64_MAX; out[2] = 0;
    out[3] = 0; out[4] = 0;
    ComputeDispatchRequest raw{};
    memcpy(&raw, request, sizeof(raw));
    AQLCodeLaunch launch{};
    if (!aql_code_launch(raw, codeVA, dext_aql_rsrc1_clamp_ieee(ctx), launch)) return -EINVAL;
    return bounded_run(ctx, raw.timeoutUS,
        [&](uint8_t *staging, uint64_t base, const struct rt_queue_geometry &g) {
            return aql_build_code_storage(staging, base, launch,
                                          g.compute_units, g.waves_per_cu);
        }, before_map, arg, out, uncertain);
}
