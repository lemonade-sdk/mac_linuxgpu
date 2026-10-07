/* Platform queue ownership; MQD contents and MES packets remain upstream.
 *
 * Resource partition of the first MEC (any geometry upstream reports in
 * adev->gfx.mec, up to the upstream bitmap and MES mask sizes):
 *
 *   - amdgpu_mes_init() splits every compute pipe into a low "legacy" range
 *     (DIV_ROUND_UP(num_compute_rings, pipes) queues, mapped by the kernel
 *     with MAP_LEGACY_QUEUE) and the rest (adev->mes.compute_hqd_mask), which
 *     SET_HW_RESOURCES hands to the MES scheduler for KFD and user queues.
 *   - amdgpu_gfx_compute_queue_acquire() marks the kernel compute rings in
 *     adev->gfx.mec_bitmap[0]; KFD receives the complement of that bitmap
 *     as cp_queue_bitmap (amdgpu_amdkfd_device_init()).
 *   - Kernel compute ring ring_id takes doorbell (mec_ring0 + ring_id) << 1
 *     (gfx_v9_0/v10_0/v11_0/v12_0/v12_1/v9_4_3 compute_ring_init) from the
 *     static kernel doorbell page.  KFD (MES mode) draws its doorbells from
 *     separate DOORBELL-domain BOs, and MES kernel doorbells start at
 *     db_start_dw_offset.
 *
 * DriverKit AQL queues behave as additional kernel compute rings: each owns
 * a legacy HQD that neither the MES scheduler nor an upstream kernel ring
 * uses, and the doorbell amdgpu assigns to the kernel ring that would occupy
 * that HQD.  The HQDs are recorded in mec_bitmap (the kernel-queue ownership
 * bitmap) and removed from KFD's cp_queue_bitmap, so every consumer agrees
 * which component owns each queue.  The partition is static for the life of
 * the device, like the upstream kernel compute ring reservation.
 *
 * The MQD is built by the device's own upstream KFD MQD manager (selected per
 * family by device_queue_manager_init()), the code KFD uses for its AQL
 * queues, and is mapped with MES MAP_LEGACY_QUEUE, the path amdgpu uses for
 * its own kernel compute queues when adev->mes.enable_legacy_queue_map is
 * set.  The fields that make a queue a legacy kernel queue rather than a
 * KFD user queue are then taken from amdgpu's own kernel compute queue MQD
 * (adev->mqds[AMDGPU_HW_IP_COMPUTE].init_mqd), see legacy_kernel_queue(). */
#include "amdgpu.h"
#include "amdgpu_mes.h"
#include "amdgpu_hdp.h"
#include "amdgpu_doorbell.h"
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_mqd_manager.h"
#include "v11_structs.h"
#include "v12_structs.h"
#include <rt/queue.h>

_Static_assert(RT_QUEUE_MAX_SLOTS == AMDGPU_MAX_COMPUTE_QUEUES,
               "rt_queue_partition masks must cover mec_bitmap[].queue_bitmap");
_Static_assert(RT_QUEUE_MASK_WORDS * 64 == RT_QUEUE_MAX_SLOTS, "mask words");

/* Every upstream doorbell layout reserves AMDGPU_MAX_COMPUTE_RINGS
 * consecutive kernel compute ring doorbells starting at mec_ring0, which is
 * what (mec_ring0 + ring_id) relies on. */
#define RT_CONSECUTIVE8(p)                                                   \
    _Static_assert(p##1 == p##0 + 1 && p##2 == p##0 + 2 && p##3 == p##0 + 3 && \
                   p##4 == p##0 + 4 && p##5 == p##0 + 5 && p##6 == p##0 + 6 && \
                   p##7 == p##0 + 7 && AMDGPU_MAX_COMPUTE_RINGS == 8,       \
                   #p " is not a consecutive 8-entry range")
RT_CONSECUTIVE8(AMDGPU_DOORBELL_MEC_RING);          /* vi */
RT_CONSECUTIVE8(AMDGPU_DOORBELL64_MEC_RING);        /* vega10, soc15 */
RT_CONSECUTIVE8(AMDGPU_VEGA20_DOORBELL_MEC_RING);   /* vega20, arcturus */
RT_CONSECUTIVE8(AMDGPU_NAVI10_DOORBELL_MEC_RING);   /* nv, soc21, soc24 */
_Static_assert(AMDGPU_DOORBELL_LAYOUT1_MEC_RING_END -
               AMDGPU_DOORBELL_LAYOUT1_MEC_RING_START + 1 == AMDGPU_MAX_COMPUTE_RINGS,
               "aqua_vanjaram MEC ring range");
_Static_assert(AMDGPU_SOC_V1_0_DOORBELL_MEC_RING_END -
               AMDGPU_SOC_V1_0_DOORBELL_MEC_RING_START + 1 == AMDGPU_MAX_COMPUTE_RINGS,
               "soc_v1_0 MEC ring range");

/* The DriverKit default dispatch queue serializes all callers. */
static struct amdgpu_device *owned_device;
static struct rt_queue_partition partition;
static uint32_t pool_doorbells[RT_QUEUE_MAX_SLOTS];
static uint64_t owned_queues[RT_QUEUE_MASK_WORDS];

static void mask_set(uint64_t *mask, unsigned slot)
{ mask[slot / 64] |= 1ULL << (slot % 64); }
static void mask_clear(uint64_t *mask, unsigned slot)
{ mask[slot / 64] &= ~(1ULL << (slot % 64)); }
static int mask_empty(const uint64_t *mask)
{
    for (unsigned i = 0; i < RT_QUEUE_MASK_WORDS; ++i)
        if (mask[i]) return 0;
    return 1;
}

/* Upstream maps its own kernel compute queues through MES exactly when
 * enable_legacy_queue_map is set (amdgpu_gfx_enable_kcq()). */
static int mes_ready(struct amdgpu_device *adev)
{
    return adev && adev->enable_mes && adev->mes.enable_legacy_queue_map &&
           adev->mes.funcs && adev->mes.funcs->map_legacy_queue &&
           adev->mes.funcs->unmap_legacy_queue &&
           adev->mes.ring[AMDGPU_MES_SCHED_PIPE].sched.ready;
}

/* First-MEC geometry, bounded by the upstream structures that describe it:
 * mec_bitmap[].queue_bitmap (AMDGPU_MAX_COMPUTE_QUEUES bits) and
 * mes.compute_hqd_mask[] (AMDGPU_MES_MAX_COMPUTE_PIPES masks of 32 bits). */
static int mec_geometry(struct amdgpu_device *adev, unsigned *pipes, unsigned *queues)
{
    const unsigned p = adev->gfx.mec.num_pipe_per_mec;
    const unsigned q = adev->gfx.mec.num_queue_per_pipe;
    if (!adev->gfx.mec.num_mec || !p || !q ||
        p > ARRAY_SIZE(adev->mes.compute_hqd_mask) ||
        q > sizeof(adev->mes.compute_hqd_mask[0]) * BITS_PER_BYTE ||
        p * q > AMDGPU_MAX_COMPUTE_QUEUES)
        return -EOPNOTSUPP;
    *pipes = p;
    *queues = q;
    return 0;
}

/* KFD's kernel-queue MQD manager for this device's family.  The HIQ
 * manager's init_mqd is KFD's compute init_mqd (AQL format, ring, EOP,
 * pointers, CU masks, atomics) plus the kernel-queue privilege bits
 * (CP_HQD_PQ_CONTROL PRIV_STATE and KMD_QUEUE) that amdgpu also sets for its
 * kernel compute queues (amdgpu_mqd_prop.kernel_queue).  KFD maps that MQD
 * from memory with a firmware MAP_QUEUES (kfd_hiq_load_mqd_kiq()). */
static struct mqd_manager *compute_mqd_manager(struct amdgpu_device *adev)
{
    struct kfd_dev *kfd = adev->kfd.dev;
    struct mqd_manager *mm;
    if (!kfd || !kfd->num_nodes || !kfd->nodes[0] || !kfd->nodes[0]->dqm)
        return NULL;
    mm = kfd->nodes[0]->dqm->mqd_mgrs[KFD_MQD_TYPE_HIQ]; /* legacy-fixup: privilege */
    return mm && mm->dev && mm->init_mqd && mm->update_mqd && mm->mqd_size ?
           mm : NULL;
}

/* amdgpu's MQD builder for its own kernel compute queues, which it maps with
 * MES MAP_LEGACY_QUEUE (amdgpu_gfx_enable_kcq()).  It must describe the same
 * MQD layout as KFD's manager. */
static const struct amdgpu_mqd *kernel_queue_mqd(struct amdgpu_device *adev,
                                                 const struct mqd_manager *mm)
{
    const struct amdgpu_mqd *kq = &adev->mqds[AMDGPU_HW_IP_COMPUTE];
    return kq->init_mqd && kq->mqd_size == mm->mqd_size ? kq : NULL;
}

/* tmpring_gc*.c: COMPUTE_TMPRING_SIZE from each family's gc header. */
extern const struct rt_tmpring_layout rt_tmpring_gc11, rt_tmpring_gc12, rt_tmpring_gc12_1;

/* The gc header of the gfx IP file upstream drives this GC with (GC 11.x
 * gfx_v11_0.c, 12.0.x gfx_v12_0.c, 12.1+ gfx_v12_1.c; the same split
 * device_queue_manager_init() makes for the MQD managers).  Only families
 * with MES legacy-queue mapping reach here. */
static const struct rt_tmpring_layout *tmpring_layout(uint32_t gc)
{
    if (IP_VERSION_MAJ(gc) == 12)
        return gc >= IP_VERSION(12, 1, 0) ? &rt_tmpring_gc12_1 : &rt_tmpring_gc12;
    if (IP_VERSION_MAJ(gc) == 11)
        return &rt_tmpring_gc11;
    return NULL;
}

/* What AQL metadata and scratch need from the device, whoever schedules
 * the queue: discovery's GC version, gfx.config and gfx.cu_info. */
static int device_geometry(struct amdgpu_device *adev, struct rt_queue_geometry *out)
{
    const uint32_t gc = amdgpu_ip_version(adev, GC_HWIP, 0);
    memset(out, 0, sizeof(*out));
    out->gfx_major = IP_VERSION_MAJ(gc);
    out->gfx_minor = IP_VERSION_MIN(gc);
    out->gfx_revision = IP_VERSION_REV(gc);
    out->engines = adev->gfx.config.max_shader_engines;
    out->compute_units = adev->gfx.cu_info.number;
    out->waves_per_cu = adev->gfx.cu_info.max_scratch_slots_per_cu;
    if (tmpring_layout(gc))
        out->tmpring = *tmpring_layout(gc);
    if (!out->engines || !out->compute_units || !out->waves_per_cu)
        return -EINVAL;
    return 0;
}

int rt_queue_device_geometry(struct amdgpu_device *adev, struct rt_queue_geometry *out)
{
    if (!adev || !out)
        return -EINVAL;
    return device_geometry(adev, out);
}

int rt_queue_geometry(struct amdgpu_device *adev, struct rt_queue_geometry *out)
{
    unsigned pipes, queues;
    struct mqd_manager *mm;
    if (!adev || !out || !mes_ready(adev))
        return -ENODEV;
    mm = compute_mqd_manager(adev);
    if (!mm || !kernel_queue_mqd(adev, mm))
        return -ENODEV;
    /* The partition covers XCC 0's MEC and kernel doorbell range only. */
    if (adev->gfx.xcc_mask && NUM_XCC(adev->gfx.xcc_mask) != 1)
        return -EOPNOTSUPP;
    int r = mec_geometry(adev, &pipes, &queues);
    if (r)
        return r;
    r = device_geometry(adev, out);
    if (r)
        return r;
    out->mqd_bytes = AMDGPU_MQD_SIZE_ALIGN(mm->mqd_size);
    return 0;
}

/* An upstream ring (kernel compute ring or KIQ) already programs this HQD. */
static int hqd_has_ring(struct amdgpu_device *adev, unsigned pipe, unsigned queue)
{
    for (unsigned i = 0; i < adev->num_rings && i < AMDGPU_MAX_RINGS; ++i) {
        const struct amdgpu_ring *ring = adev->rings[i];
        if (!ring || !ring->funcs ||
            (ring->funcs->type != AMDGPU_RING_TYPE_COMPUTE &&
             ring->funcs->type != AMDGPU_RING_TYPE_KIQ))
            continue;
        if (ring->me == 1 && ring->pipe == pipe && ring->queue == queue)
            return 1;
    }
    /* KIQ is acquired from a free MEC queue and therefore is absent from
     * mec_bitmap; check it whenever it was initialized. */
    const struct amdgpu_ring *kiq = &adev->gfx.kiq[0].ring;
    return kiq->adev == adev && kiq->me == 1 &&
           kiq->pipe == pipe && kiq->queue == queue;
}

/* 64-bit doorbells span two dwords; any overlap with a ring is a collision. */
static int doorbell_has_ring(struct amdgpu_device *adev, uint32_t db)
{
    for (unsigned i = 0; i < adev->num_rings && i < AMDGPU_MAX_RINGS; ++i) {
        const struct amdgpu_ring *ring = adev->rings[i];
        if (ring && ring->use_doorbell &&
            ring->doorbell_index + 1 >= db && ring->doorbell_index <= db + 1)
            return 1;
    }
    return 0;
}

/* Under amdgpu_gfx_compute_queue_acquire()'s multipipe policy kernel ring i
 * sits at pipe i % pipes, queue i / pipes, and the gfx sw_init loop (queue
 * outer, pipe inner) gives it ring_id i, so HQD (pipe, queue) belongs to the
 * kernel ring numbered queue * pipes + pipe. Verify the device's kernel rings
 * follow that numbering and doorbell assignment before relying on it. */
static int kernel_rings_follow_multipipe(struct amdgpu_device *adev,
                                         unsigned pipes, unsigned queues)
{
    const unsigned n = adev->gfx.num_compute_rings;
    if (n > AMDGPU_MAX_COMPUTE_RINGS || n > pipes * queues)
        return 0;
    for (unsigned i = 0; i < n; ++i) {
        const struct amdgpu_ring *ring = &adev->gfx.compute_ring[i];
        if (ring->me != 1 || ring->pipe != i % pipes ||
            ring->queue != (i / pipes) % queues || !ring->use_doorbell ||
            ring->doorbell_index != (adev->doorbell_index.mec_ring0 + i) << 1)
            return 0;
    }
    return 1;
}

/* The doorbell gfx_vN_0_compute_ring_init() would give the kernel ring
 * occupying HQD (pipe, queue). Ring ids stop below AMDGPU_MAX_COMPUTE_RINGS
 * (num_compute_rings is capped there), which is also the size of every
 * layout's reserved kernel compute ring doorbell range. */
static int kernel_ring_doorbell(struct amdgpu_device *adev, unsigned pipes,
                                unsigned pipe, unsigned queue, uint32_t *out)
{
    const struct amdgpu_doorbell_index *di = &adev->doorbell_index;
    const unsigned ring_id = queue * pipes + pipe;
    if (ring_id >= AMDGPU_MAX_COMPUTE_RINGS)
        return -ENOSPC;
    const uint32_t db = (di->mec_ring0 + ring_id) << 1;
    /* CP_MEC_DOORBELL_RANGE spans kiq..userqueue_end. */
    if (db < di->kiq * 2 || db / 2 > di->userqueue_end ||
        db + 1 >= adev->doorbell.num_kernel_doorbells ||
        (adev->mes.db_start_dw_offset && db + 1 >= adev->mes.db_start_dw_offset) ||
        doorbell_has_ring(adev, db))
        return -ENOSPC;
    *out = db;
    return 0;
}

static void first_mec_mask(struct amdgpu_device *adev, unsigned slots, uint64_t *mask)
{
    memset(mask, 0, sizeof(uint64_t) * RT_QUEUE_MASK_WORDS);
    for (unsigned i = 0; i < slots; ++i)
        if (test_bit(i, adev->gfx.mec_bitmap[0].queue_bitmap))
            mask_set(mask, i);
}

static int partition_applied(struct amdgpu_device *adev, unsigned pipes, unsigned queues)
{
    if (owned_device != adev || partition.pipes != pipes ||
        partition.queues_per_pipe != queues || mask_empty(partition.dext_mask))
        return 0;
    for (unsigned i = 0; i < pipes * queues; ++i)
        if (rt_queue_mask_test(partition.dext_mask, i) &&
            !test_bit(i, adev->gfx.mec_bitmap[0].queue_bitmap))
            return 0;
    return 1;
}

int rt_queue_partition(struct amdgpu_device *adev, struct rt_queue_partition *out)
{
    unsigned pipes, queues;
    if (!adev)
        return -EINVAL;
    int r = mec_geometry(adev, &pipes, &queues);
    if (r)
        return r;
    const unsigned slots = pipes * queues;
    if (!partition_applied(adev, pipes, queues)) {
        static struct rt_queue_partition next;
        static uint32_t doorbells[RT_QUEUE_MAX_SLOTS];
        if (!mask_empty(owned_queues))
            return -EBUSY;
        if (!kernel_rings_follow_multipipe(adev, pipes, queues))
            return -EOPNOTSUPP;
        memset(&next, 0, sizeof(next));
        memset(doorbells, 0, sizeof(doorbells));
        next.pipes = pipes;
        next.queues_per_pipe = queues;
        first_mec_mask(adev, slots, next.kernel_mask);
        for (unsigned slot = 0; slot < slots; ++slot) {
            const unsigned pipe = slot / queues, queue = slot % queues;
            if (adev->mes.compute_hqd_mask[pipe] & (1u << queue)) {
                mask_set(next.mes_mask, slot);
                continue;
            }
            if (rt_queue_mask_test(next.kernel_mask, slot) ||
                hqd_has_ring(adev, pipe, queue) ||
                kernel_ring_doorbell(adev, pipes, pipe, queue, &doorbells[slot]))
                continue;
            mask_set(next.dext_mask, slot);
        }
        if (mask_empty(next.dext_mask))
            return -ENOSPC;
        for (unsigned slot = 0; slot < slots; ++slot) {
            if (!rt_queue_mask_test(next.dext_mask, slot))
                continue;
            set_bit(slot, adev->gfx.mec_bitmap[0].queue_bitmap);
            if (adev->kfd.dev)
                clear_bit(slot, adev->kfd.dev->shared_resources.cp_queue_bitmap);
        }
        owned_device = adev;
        partition = next;
        memcpy(pool_doorbells, doorbells, sizeof(pool_doorbells));
    }
    /* KFD's cp_queue_bitmap is a copy taken at kgd2kfd_device_init(); report
     * what it holds now so callers can verify that no HQD is shared. */
    memset(partition.kfd_mask, 0, sizeof(partition.kfd_mask));
    if (adev->kfd.dev)
        for (unsigned i = 0; i < RT_QUEUE_MAX_SLOTS; ++i)
            if (test_bit(i, adev->kfd.dev->shared_resources.cp_queue_bitmap))
                mask_set(partition.kfd_mask, i);
    memcpy(partition.owned_mask, owned_queues, sizeof(partition.owned_mask));
    memcpy(partition.doorbells, pool_doorbells, sizeof(partition.doorbells));
    if (out)
        *out = partition;
    return 0;
}

int rt_queue_reserve(struct amdgpu_device *adev, uint32_t *slot, uint32_t *doorbell)
{
    struct rt_queue_geometry geometry;
    int r = rt_queue_geometry(adev, &geometry);
    if (r || !slot || !doorbell) return r ? r : -EINVAL;
    if (owned_device && owned_device != adev && !mask_empty(owned_queues))
        return -EBUSY;
    r = rt_queue_partition(adev, NULL);
    if (r) return r;
    for (unsigned i = 0; i < RT_QUEUE_MAX_SLOTS; ++i) {
        if (!rt_queue_mask_test(partition.dext_mask, i) ||
            rt_queue_mask_test(owned_queues, i))
            continue;
        mask_set(owned_queues, i);
        *slot = i; *doorbell = pool_doorbells[i];
        return 0;
    }
    return -ENOSPC;
}

void rt_queue_release(struct amdgpu_device *adev, uint32_t slot)
{
    if (adev == owned_device && slot < RT_QUEUE_MAX_SLOTS)
        mask_clear(owned_queues, slot);
}

static int owned_slot(struct amdgpu_device *adev, uint32_t slot, uint32_t doorbell)
{
    return adev == owned_device && slot < RT_QUEUE_MAX_SLOTS &&
           partition.queues_per_pipe &&
           rt_queue_mask_test(owned_queues, slot) &&
           rt_queue_mask_test(partition.dext_mask, slot) &&
           doorbell == pool_doorbells[slot];
}

static int pow2(uint64_t v) { return v && !(v & (v - 1)); }

/* MES MAP_LEGACY_QUEUE (mes_v11_0/mes_v12_0/mes_v12_1_map_legacy_queue)
 * sends ADD_QUEUE with map_legacy_kq = 1 and only pipe_id, queue_id,
 * doorbell_offset, mqd_addr, wptr_addr and queue_type: no VMID, page table,
 * process or gang context, is_aql_queue, trap handler or context-save area.
 * Every other HQD property comes from the MQD image at mqd_addr.  amdgpu's
 * MQDs for those queues (amdgpu_ring_init_mqd() -> gfx_vN_0_compute_mqd_init)
 * leave cp_hqd_active 0 ("map_queues packet doesn't need activate the
 * queue", amdgpu_ring_to_mqd_prop()), enable the doorbell, carry no CWSR
 * state, do not enable the KFD debugger and are privileged kernel queues.
 *
 * KFD's MQD managers build user-queue MQDs for MES ADD_QUEUE, KIQ
 * MAP_QUEUES (HIQ) or direct HQD load instead.  The differences:
 *   - DOORBELL_EN: KFD leaves it to the loader (kgd_gfx_v11_hqd_load()
 *     sets it in the register); amdgpu's legacy MQDs set it.
 *   - CWSR: the v10+ managers set QSWITCH_MODE whenever kfd->cwsr_enabled,
 *     even for a queue whose properties carry no context-save area (the
 *     v9/vi managers test ctx_save_restore_area_address).  With no area the
 *     context-save registers are zero, but QSWITCH_MODE would still let a
 *     preempting unmap try to save waves to address 0.
 *   - c_queue_debug_en: KFD sets it for its debugger; amdgpu does not.
 *   - PRIV_STATE/KMD_QUEUE: amdgpu sets them for kernel queues, as does
 *     KFD's kernel-queue (HIQ) manager used here, not its user-queue one.
 *   - cp_hqd_active: 0 in both.
 * The first three are taken from amdgpu's kernel-queue MQD here.  Everything
 * that describes the queue itself (AQL format, ring, EOP, pointers,
 * priorities, CU masks, the atomics capability in cp_hqd_hq_status0, which
 * follows amdgpu_amdkfd_have_atomics_support()) stays as KFD built it.
 *
 * Every family with MES legacy-queue mapping (v11, v12, v12.1 compute MQDs)
 * places these fields identically; legacy_kernel_queue() checks at run time
 * that both builders wrote the shared fields where this layout expects. */
#define RT_SAME_MQD_FIELD(f)                                                  \
    _Static_assert(offsetof(struct v11_compute_mqd, f) ==                    \
                   offsetof(struct v12_compute_mqd, f) &&                    \
                   offsetof(struct v12_1_compute_mqd, f) ==                  \
                   offsetof(struct v12_compute_mqd, f), #f)
RT_SAME_MQD_FIELD(header);
RT_SAME_MQD_FIELD(cp_mqd_base_addr_lo);
RT_SAME_MQD_FIELD(cp_mqd_base_addr_hi);
RT_SAME_MQD_FIELD(cp_hqd_active);
RT_SAME_MQD_FIELD(cp_hqd_vmid);
RT_SAME_MQD_FIELD(cp_hqd_persistent_state);
RT_SAME_MQD_FIELD(cp_hqd_pq_base_lo);
RT_SAME_MQD_FIELD(cp_hqd_pq_base_hi);
RT_SAME_MQD_FIELD(cp_hqd_pq_rptr_report_addr_lo);
RT_SAME_MQD_FIELD(cp_hqd_pq_wptr_poll_addr_lo);
RT_SAME_MQD_FIELD(cp_hqd_pq_doorbell_control);
RT_SAME_MQD_FIELD(cp_hqd_pq_control);
RT_SAME_MQD_FIELD(cp_hqd_hq_status0);
RT_SAME_MQD_FIELD(cp_hqd_ctx_save_base_addr_lo);
RT_SAME_MQD_FIELD(cp_hqd_ctx_save_base_addr_hi);
RT_SAME_MQD_FIELD(cp_hqd_ctx_save_control);
RT_SAME_MQD_FIELD(cp_hqd_cntl_stack_offset);
RT_SAME_MQD_FIELD(cp_hqd_cntl_stack_size);
RT_SAME_MQD_FIELD(cp_hqd_wg_state_offset);
RT_SAME_MQD_FIELD(cp_hqd_ctx_save_size);
RT_SAME_MQD_FIELD(cp_hqd_pq_rptr);
RT_SAME_MQD_FIELD(cp_hqd_pq_wptr_lo);
RT_SAME_MQD_FIELD(cp_hqd_pq_wptr_hi);
typedef struct v12_compute_mqd rt_compute_mqd;

/* cp_hqd_hq_status0.C_QUEUE_DEBUG_EN (named in gc_12_0_0_sh_mask.h), which
 * the v11/v12/v12.1 KFD managers set as 1 << 14 for the KFD debugger. */
#define RT_HQ_STATUS0_C_QUEUE_DEBUG_EN (1u << 14)

/* The largest MQD any family above defines; amdgpu's builder writes the
 * reference here (callers are serialized, see owned_device). */
static uint32_t kernel_queue_ref[(sizeof(struct v12_1_compute_mqd) >
                                  sizeof(struct v12_compute_mqd) ?
                                  sizeof(struct v12_1_compute_mqd) :
                                  sizeof(struct v12_compute_mqd)) / 4];
_Static_assert(sizeof(struct v11_compute_mqd) <= sizeof(kernel_queue_ref),
               "reference MQD buffer");

static int legacy_kernel_queue(struct amdgpu_device *adev, const struct amdgpu_mqd *kq,
                               uint32_t doorbell, const struct rt_queue_mqd *d,
                               rt_compute_mqd *m, size_t mqd_size)
{
    rt_compute_mqd *ref = (rt_compute_mqd *)kernel_queue_ref;
    struct amdgpu_mqd_prop prop;
    if (mqd_size > sizeof(kernel_queue_ref) ||
        mqd_size < offsetof(rt_compute_mqd, cp_hqd_ctx_save_size) + 4)
        return -EOPNOTSUPP;

    /* amdgpu_ring_to_mqd_prop() for a normal-priority kernel compute ring
     * on this HQD and doorbell; gfx_vN_0_kcq_init_queue() zeroes first. */
    memset(&prop, 0, sizeof(prop));
    prop.mqd_gpu_addr = d->mqd_address;
    prop.hqd_base_gpu_addr = d->ring_address;
    prop.rptr_gpu_addr = d->read_pointer;
    prop.wptr_gpu_addr = d->write_pointer;
    prop.queue_size = d->ring_bytes;
    prop.eop_gpu_addr = d->eop_address;
    prop.use_doorbell = true;
    prop.doorbell_index = doorbell;
    prop.kernel_queue = true;
    prop.hqd_active = false;
    memset(kernel_queue_ref, 0, sizeof(kernel_queue_ref));
    int r = kq->init_mqd(adev, ref, &prop);
    if (r)
        return r;

    /* Both builders describe the same queue in the same layout. */
    if (ref->header != m->header ||
        ref->cp_mqd_base_addr_lo != (m->cp_mqd_base_addr_lo & ~3u) ||
        ref->cp_mqd_base_addr_hi != m->cp_mqd_base_addr_hi ||
        ref->cp_hqd_pq_base_lo != m->cp_hqd_pq_base_lo ||
        ref->cp_hqd_pq_base_hi != m->cp_hqd_pq_base_hi ||
        ref->cp_hqd_pq_rptr_report_addr_lo != m->cp_hqd_pq_rptr_report_addr_lo ||
        ref->cp_hqd_pq_wptr_poll_addr_lo != m->cp_hqd_pq_wptr_poll_addr_lo ||
        ref->cp_hqd_vmid != m->cp_hqd_vmid ||
        !(ref->cp_hqd_pq_doorbell_control & m->cp_hqd_pq_doorbell_control) ||
        !(ref->cp_hqd_pq_control & m->cp_hqd_pq_control))
        return -EOPNOTSUPP;

    /* MAP_LEGACY_QUEUE activates the HQD itself; KFD's init_mqd (memset)
     * and amdgpu's kernel compute ring MQD both leave it inactive.  The
     * context-save area registers are zero because the queue properties
     * carry no area; only QSWITCH_MODE remains of KFD's CWSR setup. */
    if (m->cp_hqd_active != ref->cp_hqd_active ||
        m->cp_hqd_ctx_save_base_addr_lo || m->cp_hqd_ctx_save_base_addr_hi ||
        m->cp_hqd_ctx_save_size || m->cp_hqd_cntl_stack_size ||
        m->cp_hqd_cntl_stack_offset || m->cp_hqd_wg_state_offset)
        return -EOPNOTSUPP;

    /* Doorbell enable; the AQL DOORBELL_BIF_DROP KFD sets is kept. */
    m->cp_hqd_pq_doorbell_control |= ref->cp_hqd_pq_doorbell_control; /* legacy-fixup: doorbell */
    /* No CWSR preemption: there is no context-save area to save waves to. */
    m->cp_hqd_persistent_state = ref->cp_hqd_persistent_state; /* legacy-fixup: qswitch */
    m->cp_hqd_ctx_save_control = ref->cp_hqd_ctx_save_control;
    /* No debugger on a kernel queue; KFD's atomics capability bit stays. */
    m->cp_hqd_hq_status0 &= ~RT_HQ_STATUS0_C_QUEUE_DEBUG_EN; /* legacy-fixup: debug */
    m->cp_hqd_hq_status0 |= ref->cp_hqd_hq_status0;
    return 0;
}

int rt_queue_build_mqd(struct amdgpu_device *adev, uint32_t slot, uint32_t doorbell,
                       const struct rt_queue_mqd *d, void *mqd, size_t mqd_bytes)
{
    const uint64_t limit = 1ULL << 48;
    const struct amdgpu_mqd *kq;
    struct mqd_manager *mm;
    if (!adev || !d || !mqd || !owned_slot(adev, slot, doorbell))
        return -EINVAL;
    mm = compute_mqd_manager(adev);
    kq = mm ? kernel_queue_mqd(adev, mm) : NULL;
    if (!kq)
        return -ENODEV;
    /* KFD's allocate_mqd() reserves AMDGPU_MQD_SIZE_ALIGN(mqd_size). */
    if (AMDGPU_MQD_SIZE_ALIGN(mm->mqd_size) > mqd_bytes)
        return -EOPNOTSUPP;
    if (!d->mqd_address || (d->mqd_address & 255) || d->mqd_address >= limit ||
        !d->ring_address || (d->ring_address & 255) || !pow2(d->ring_bytes) ||
        d->ring_bytes < 256 || d->ring_address > limit - d->ring_bytes ||
        !d->eop_address || (d->eop_address & 255) || !pow2(d->eop_bytes) ||
        d->eop_bytes < 8 || d->eop_address > limit - d->eop_bytes ||
        !d->read_pointer || (d->read_pointer & 7) || d->read_pointer > limit - 8 ||
        !d->write_pointer || (d->write_pointer & 7) || d->write_pointer > limit - 8)
        return -EINVAL;

    struct queue_properties q = {0};
    q.type = KFD_QUEUE_TYPE_COMPUTE;
    q.format = KFD_QUEUE_FORMAT_AQL;
    q.queue_address = d->ring_address;
    q.queue_size = d->ring_bytes;
    q.queue_percent = 100;
    q.read_ptr = (void __user *)(uintptr_t)d->read_pointer;
    q.write_ptr = (void __user *)(uintptr_t)d->write_pointer;
    q.doorbell_off = doorbell;
    q.eop_ring_buffer_address = d->eop_address;
    q.eop_ring_buffer_size = d->eop_bytes;
    q.vmid = 0; /* legacy kernel queue, like amdgpu's compute rings */
    /* No context-save area: CWSR stays off for this queue (the v9/vi
     * managers test ctx_save_restore_area_address; legacy_kernel_queue()
     * covers the v10+ managers, which do not). */
    q.ctx_save_restore_area_address = 0;
    q.ctx_save_restore_area_size = 0;
    q.ctl_stack_size = 0;

    struct kfd_mem_obj obj = {0};
    void *out = NULL;
    uint64_t gart = 0;
    obj.gpu_addr = d->mqd_address;
    obj.cpu_ptr = mqd;
    mm->init_mqd(mm, &out, &obj, &gart, &q);
    if (out != mqd || gart != d->mqd_address)
        return -EINVAL;
    return legacy_kernel_queue(adev, kq, doorbell, d, mqd, mm->mqd_size);
}

/* Where a mapped queue starts reading: the HQD loads its read and write
 * pointers from the MQD (in dwords; an AQL packet is 16), as the KFD HQD
 * loaders compute them from a write pointer in packets
 * (kgd_gfx_v11_hqd_load's guessed_wptr). */
int rt_queue_mqd_set_position(void *mqd, uint64_t ring_bytes, uint64_t packet)
{
    rt_compute_mqd *m = mqd;
    const uint64_t ring_dwords = ring_bytes / 4, dwords = packet * 16;

    if (!m || !pow2(ring_bytes) || ring_bytes < 256 || packet > (UINT64_MAX >> 4))
        return -EINVAL;
    m->cp_hqd_pq_rptr = (uint32_t)(dwords & (ring_dwords - 1));
    m->cp_hqd_pq_wptr_lo = lower_32_bits(dwords);
    m->cp_hqd_pq_wptr_hi = upper_32_bits(dwords);
    return 0;
}

int rt_queue_map(struct amdgpu_device *adev, uint32_t slot, uint32_t doorbell,
                 uint64_t mqd, uint64_t write_pointer)
{
    if (!mes_ready(adev) || !owned_slot(adev, slot, doorbell)) return -EINVAL;
    struct mes_map_legacy_queue_input in = {0};
    in.queue_type = AMDGPU_RING_TYPE_COMPUTE;
    in.pipe_id = slot / partition.queues_per_pipe;
    in.queue_id = slot % partition.queues_per_pipe;
    in.doorbell_offset = doorbell; in.mqd_addr = mqd; in.wptr_addr = write_pointer;
    amdgpu_mes_lock(&adev->mes);
    int r = adev->mes.funcs->map_legacy_queue(&adev->mes, &in);
    amdgpu_mes_unlock(&adev->mes);
    return r;
}

int rt_queue_unmap(struct amdgpu_device *adev, uint32_t slot, uint32_t doorbell)
{
    if (!mes_ready(adev) || !owned_slot(adev, slot, doorbell)) return -EINVAL;
    struct mes_unmap_legacy_queue_input in = {0};
    in.action = PREEMPT_QUEUES;
    in.queue_type = AMDGPU_RING_TYPE_COMPUTE;
    in.pipe_id = slot / partition.queues_per_pipe;
    in.queue_id = slot % partition.queues_per_pipe;
    in.doorbell_offset = doorbell;
    amdgpu_mes_lock(&adev->mes);
    int r = adev->mes.funcs->unmap_legacy_queue(&adev->mes, &in);
    amdgpu_mes_unlock(&adev->mes);
    return r;
}

void rt_queue_flush(struct amdgpu_device *adev)
{
    mb();
    amdgpu_device_flush_hdp(adev, NULL);
}

int rt_queue_kick(struct amdgpu_device *adev, uint32_t doorbell, uint64_t packet)
{
    if (!adev || doorbell + 1 >= adev->doorbell.num_kernel_doorbells ||
        !adev->doorbell.cpu_addr) return -EINVAL;
    if (adev != owned_device || !mes_ready(adev)) return -ENODEV;
    int owned = 0;
    for (unsigned i = 0; i < RT_QUEUE_MAX_SLOTS; ++i)
        if (rt_queue_mask_test(owned_queues, i) && pool_doorbells[i] == doorbell)
            owned = 1;
    if (!owned) return -EINVAL;
    rt_queue_flush(adev);
    /* writeq routes fake MMIO through DriverKit rather than dereferencing it. */
    writeq(packet, adev->doorbell.cpu_addr + doorbell);
    return 0;
}
