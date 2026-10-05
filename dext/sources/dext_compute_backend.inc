/* Production selector seam over the probed upstream AMDGPU device. */
#include <limits.h>
#include <linux/errno.h>
#include <rt/compute.h>
#include <rt/dispatch.h>
#include <rt/gart.h>
#include <rt/queue.h>
#include "dext_aql.h"
#include "dext_kfd.h"

#define DEXT_RUNTIME_MAGIC 0x414d444750554142ull
#define DEXT_RUNTIME_ABI 1u
#define DEXT_RUNTIME_BUILD 243u /* 192: KFD compute sessions, QueryInfo tag 12;
                                   243: session calls are async (session_state.h) */
#define DEXT_BO_SLOTS 8192u
/* Queue handles across clients: legacy HQDs plus KFD user queues. */
#define DEXT_QUEUE_SLOTS 1024u
#define DEXT_BO_MEMORY_BASE 0x10000u
#define DEXT_EXPORT_SLOTS 256u
#define DEXT_CLIENT_SLOTS 64u

/* A BO is either a VMID0 kernel BO (object) of the legacy path or a BO of
 * the owner's KFD process (kfd). */
struct production_bo {
    struct rt_compute_bo *object;
    struct rt_kfd_bo *kfd;
    uint64_t handle;
    uint64_t owner;
    uint64_t size;
    uint64_t alignment;
    uint64_t gpu_address;
    void *cpu_address;
    uint32_t domain;
    uint32_t memory_type;
    bool host_visible;
};

struct production_queue {
    struct dext_aql_queue *object;
    struct dext_kfd_queue *kfd;
    uint64_t handle;
    uint64_t owner;
    uint64_t ring_handle;
    uint64_t metadata_handle;
};

/* Compute session mode of a client (QueryInfo tag 12 word 1). */
enum production_mode {
    PRODUCTION_UNDECIDED = 0,
    PRODUCTION_LEGACY = 1,	/* VMID0 BOs, legacy HQDs via MAP_LEGACY_QUEUE */
    PRODUCTION_KFD = 2,	/* a KFD process: GPUVM BOs, MES user queues */
};

struct production_client {
    uint64_t client;
    uint32_t mode;
    bool aware;		/* asked for tag 12: configures its own window */
    struct dext_kfd_client *kfd;
    int pid;
    char comm[32];
};

struct production_export {
    struct rt_compute_bo *object;
    uint64_t token[2];
};

static struct {
    struct rt_compute_ctx *ctx;
    struct rt_compute_properties properties;
    struct production_bo bos[DEXT_BO_SLOTS];
    struct production_queue queues[DEXT_QUEUE_SLOTS];
    struct production_export exports[DEXT_EXPORT_SLOTS];
    struct production_client clients[DEXT_CLIENT_SLOTS];
    uint64_t selected_client;
    uint64_t next_bo_handle;
    uint64_t next_queue_handle;
    uint32_t next_memory_type;
    bool ready;
    bool uncertain;
    /* Every cause of uncertain so far was a KFD client's: closing those
     * clients (which retries their recovery) may clear it. */
    bool uncertain_kfd_only;
    /* The device left the bus: what it may still have used is free. */
    bool removed;
    bool legacy_queues;	/* the queue partition left a legacy HQD */
    bool kfd_disabled;	/* policy: never open KFD sessions */
    /* Whether the context serves AQL or KFD queues, set on the session
     * queue when it opens, so RuntimeBuild answers on any thread without
     * touching the context (dext_compute_runtime_build_cached). */
    bool runtime_available;
} production;

uint64_t dext_compute_select_client(uint64_t client)
{
    uint64_t prior = production.selected_client;
    production.selected_client = client;
    return prior;
}

/* GPU completion uncertain: freeze the device. @kfd: the cause is a KFD
 * client's, which its close may still resolve. */
static void freeze(bool kfd)
{
    if (!production.uncertain) production.uncertain_kfd_only = kfd;
    else if (!kfd) production.uncertain_kfd_only = false;
    production.uncertain = true;
}

static int production_ready(void)
{
    return production.ready && !production.uncertain && production.ctx &&
           rt_compute_status(production.ctx) == 0;
}

static bool bo_live(const struct production_bo *bo)
{
    return bo->object || bo->kfd;
}

static bool queue_live(const struct production_queue *q)
{
    return q->object || q->kfd;
}

/* ---- compute sessions: one record per runtime client ---- */

static struct production_client *client_record(uint64_t client, bool create)
{
    struct production_client *empty = NULL;
    if (!client) return NULL;	/* client zero: internal, legacy */
    for (unsigned i = 0; i < DEXT_CLIENT_SLOTS; ++i) {
        if (production.clients[i].client == client) return &production.clients[i];
        if (!production.clients[i].client && !empty) empty = &production.clients[i];
    }
    if (!create || !empty) return NULL;
    memset(empty, 0, sizeof(*empty));
    empty->client = client;
    return empty;
}

static void runtime_available_update(void)
{
    __atomic_store_n(&production.runtime_available, production.ctx &&
                     (dext_aql_available(production.ctx) == 0 ||
                      (!production.kfd_disabled && dext_kfd_supported(production.ctx) == 0)),
                     __ATOMIC_RELEASE);
}

void dext_compute_set_kfd_policy(bool enabled)
{
    production.kfd_disabled = !enabled;
    runtime_available_update();
}

int dext_compute_client_identity(uint64_t client, int pid, const char *comm)
{
    struct production_client *rec = client_record(client, true);
    if (!rec) return -ENOMEM_L;
    rec->pid = pid;
    memset(rec->comm, 0, sizeof(rec->comm));
    if (comm) {
        size_t n = strlen(comm);
        if (n >= sizeof(rec->comm)) n = sizeof(rec->comm) - 1;
        memcpy(rec->comm, comm, n);
    }
    return 0;
}

/* The selected client's record, deciding its mode on first use once the
 * device is ready: a KFD process when KFD bound a device node scheduled by
 * MES (and policy allows), else the legacy path. A session that cannot be
 * opened falls back to the legacy path; one that cannot be closed again
 * makes the device uncertain. */
static struct production_client *selected_session(void)
{
    struct production_client *rec = client_record(production.selected_client, true);
    if (!rec || rec->mode != PRODUCTION_UNDECIDED || !production_ready()) return rec;
    rec->mode = PRODUCTION_LEGACY;
    if (!production.kfd_disabled && dext_kfd_supported(production.ctx) == 0) {
        struct dext_kfd_client *kfd = NULL;
        int r = dext_kfd_open(production.ctx, rec->pid, rec->comm[0] ? rec->comm : NULL, &kfd);
        if (!r) {
            rec->kfd = kfd;
            rec->mode = PRODUCTION_KFD;
        } else if (kfd) {
            rec->kfd = kfd;	/* retained: it may still own GPU memory */
            freeze(true);
        }
    }
    return rec;
}

static struct dext_kfd_client *selected_kfd(void)
{
    struct production_client *rec = selected_session();
    return rec && rec->mode == PRODUCTION_KFD ? rec->kfd : NULL;
}

static struct dext_kfd_client *owner_kfd(uint64_t owner)
{
    struct production_client *rec = client_record(owner, false);
    return rec && rec->mode == PRODUCTION_KFD ? rec->kfd : NULL;
}

/* KFD failures that leave GPU completion uncertain freeze the device. */
static int kfd_error(struct dext_kfd_client *c, int r)
{
    if (dext_kfd_uncertain(c)) freeze(true);
    if (production.uncertain) return -EBUSY_L;
    if (r == -ENOMEM || r == -ENOSPC) return -ENOMEM_L;
    if (r == -EINVAL || r == -ERANGE || r == -ENOENT) return -EINVAL_L;
    if (r == -EBUSY) return -EBUSY_L;
    return -ENOTREADY_L;
}

/* Tear a client's KFD process down as Linux tears down a process that dies
 * with live queues. The client is gone, so its queue and BO handles go with
 * it; the dext client owns the queue records and its close destroys them
 * (DESTROY_QUEUE, recovering a queue MES does not confirm removing), then
 * frees the BOs and exits the process. A close that cannot confirm the GPU
 * let go keeps the client, and closing it again retries. */
static int kfd_release(struct production_client *rec)
{
    struct dext_kfd_client *c = rec->kfd;
    for (unsigned i = 0; i < DEXT_QUEUE_SLOTS; ++i) {
        struct production_queue *q = &production.queues[i];
        if (q->kfd && q->owner == rec->client) memset(q, 0, sizeof(*q));
    }
    if (dext_kfd_close(c) != 0) {
        freeze(true);
        return -EBUSY_L;
    }
    for (unsigned i = 0; i < DEXT_BO_SLOTS; ++i)
        if (production.bos[i].kfd && production.bos[i].owner == rec->client)
            memset(&production.bos[i], 0, sizeof(production.bos[i]));
    rec->kfd = NULL;
    rec->mode = PRODUCTION_UNDECIDED;
    return 0;
}

void dext_compute_device_removed(void)
{
    production.removed = true;
    production.ready = false;
}

/* A device frozen only by KFD clients thaws once none of them is uncertain
 * any more: their sessions retry what they could not confirm (a copy that
 * outlived its timeout, a queue MES did not remove) when settled. Every
 * KFD client is settled; true when the device is no longer uncertain. */
static bool thaw_kfd(unsigned int wait_ms)
{
    bool certain = true;
    if (!production.uncertain) return true;
    if (!production.uncertain_kfd_only && !production.removed) return false;
    for (unsigned i = 0; i < DEXT_CLIENT_SLOTS; ++i) {
        struct production_client *rec = &production.clients[i];
        if (rec->kfd && dext_kfd_uncertain(rec->kfd) &&
            dext_kfd_settle(rec->kfd, wait_ms) != 0)
            certain = false;
    }
    if (!certain && !production.removed) return false;
    production.uncertain = false;
    production.uncertain_kfd_only = false;
    return true;
}

/* Structural consistency only: any discovered GC, any geometry upstream's
 * structures can describe. Queue support is decided by the capabilities
 * dext_aql_available() checks, not by the device's IP version or size.
 * cu_bitmap is upstream's per-XCC cu_info.bitmap (one word per shader
 * engine/array pair, one bit per CU), so it bounds both the CU count and
 * the engine/array product. */
static int properties_valid(const struct rt_compute_properties *p)
{
    const uint64_t bitmap_words =
        sizeof(p->cu_bitmap) / sizeof(p->cu_bitmap[0][0]);
    const uint64_t bitmap_bits = sizeof(p->cu_bitmap) * CHAR_BIT;
    return p->pci_chip_id && p->pci_chip_id <= UINT16_MAX &&
           ((p->gfx_ip_version >> 24) & 255) &&
           p->visible_vram_bytes &&
           p->visible_vram_bytes <= p->vram_bytes &&
           p->gart_start && p->gart_bytes && p->host_memory_verified &&
           p->cu_count && p->cu_count <= bitmap_bits &&
           p->shader_engines && p->shader_arrays_per_engine &&
           (uint64_t)p->shader_engines * p->shader_arrays_per_engine <= bitmap_words &&
           p->timestamp_frequency_hz &&
           p->simd_per_cu && p->max_waves_per_simd &&
           (uint64_t)p->simd_per_cu * p->max_waves_per_simd <= UINT32_MAX &&
           (p->wavefront_size == 32 || p->wavefront_size == 64);
}

static struct production_bo *production_bo_lookup(uint64_t handle)
{
    unsigned i;
    for (i = 0; i < DEXT_BO_SLOTS; ++i)
        if (bo_live(&production.bos[i]) && production.bos[i].handle == handle &&
            production.bos[i].owner == production.selected_client)
            return &production.bos[i];
    return NULL;
}

static struct production_queue *production_queue_lookup(uint64_t handle)
{
    unsigned i;
    for (i = 0; i < DEXT_QUEUE_SLOTS; ++i)
        if (queue_live(&production.queues[i]) && production.queues[i].handle == handle &&
            production.queues[i].owner == production.selected_client)
            return &production.queues[i];
    return NULL;
}

/* Each BO slot owns one underlying object reference. An export token itself
 * owns none, matching the reference driver's share lifetime. */
static int production_bo_drop(struct production_bo *bo)
{
    if (bo->kfd) {
        struct dext_kfd_client *c = owner_kfd(bo->owner);
        int r = c ? dext_kfd_bo_free(c, bo->kfd) : -ENOENT;
        if (r) return kfd_error(c, r);
        memset(bo, 0, sizeof(*bo));
        return 0;
    }
    for (unsigned i = 0; i < DEXT_BO_SLOTS; ++i) {
        if (&production.bos[i] != bo && production.bos[i].object == bo->object) {
            memset(bo, 0, sizeof(*bo));
            return 0;
        }
    }
    if (rt_compute_bo_free(production.ctx, bo->object) != 0) {
        freeze(false);
        return -EBUSY_L;
    }
    for (unsigned i = 0; i < DEXT_EXPORT_SLOTS; ++i)
        if (production.exports[i].object == bo->object)
            memset(&production.exports[i], 0, sizeof(production.exports[i]));
    memset(bo, 0, sizeof(*bo));
    return 0;
}

int dext_compute_release_client(uint64_t client)
{
    struct production_client *rec;
    if (!client) return -EINVAL_L;
    rec = client_record(client, false);
    /* A client that dies is torn down as a Linux process that dies: its KFD
     * process is closed even when it is what froze the device, since the
     * close retries what was left uncertain; the device thaws if that was
     * all. Anything else uncertain refuses the release. */
    if (production.uncertain && !production.uncertain_kfd_only && !production.removed)
        return -EBUSY_L;
    /* A KFD client kept after its open failed is closed as well. */
    if (rec && rec->kfd && kfd_release(rec) != 0)
        return -EBUSY_L;
    if (!thaw_kfd(0)) return -EBUSY_L;
    if (rec) memset(rec, 0, sizeof(*rec));
    for (unsigned i = 0; i < DEXT_QUEUE_SLOTS; ++i) {
        struct production_queue *q = &production.queues[i];
        if (!q->object || q->owner != client) continue;
        if (dext_aql_destroy(q->object) != 0 && !production.removed) {
            freeze(false);
            return -EBUSY_L;
        }
        memset(q, 0, sizeof(*q));
    }
    for (unsigned i = 0; i < DEXT_BO_SLOTS; ++i) {
        struct production_bo *bo = &production.bos[i];
        if (bo->object && bo->owner == client && production_bo_drop(bo))
            return -EBUSY_L;
    }
    return 0;
}

static const char *start_failed_step;
static int start_failed_error;

void dext_compute_start_failure(const char **step, int *error)
{
    if (step) *step = start_failed_step;
    if (error) *error = start_failed_error;
}

int dext_compute_start(struct pci_dev *pdev)
{
    struct rt_compute_ctx *ctx = NULL;
    struct rt_compute_properties properties = {0};
    const char *step = "gart status";
    int r;
    if (!pdev)
        return -EINVAL_L;
    if (production.ctx)
        return production_ready() ? 0 : -EBUSY_L;
    r = rt_compute_open(pdev, &ctx);
    start_failed_step = NULL;
    start_failed_error = 0;
    if (r) {
        start_failed_step = "open";
        start_failed_error = r;
        return -ENOTREADY_L;
    }
    production.ctx = ctx;
    if (rt_gart_status() != 0)
        r = -ENODEV;
    if (!r) {
        uint64_t base = 0, size = 0;
        step = "gart window";
        r = rt_gart_get_window(&base, &size);
        if (!r && (!base || !size)) r = -ENODEV;
    }
    if (!r) {
        step = "host memory check";
        r = rt_compute_verify_host_memory(ctx);
    }
    if (!r) {
        step = "status";
        r = rt_compute_status(ctx);
    }
    if (!r) {
        step = "properties";
        r = rt_compute_properties(ctx, &properties);
    }
    if (!r) {
        struct rt_compute_memory_usage usage;
        step = "memory usage";
        r = rt_compute_memory_usage(ctx, &usage);
        if (!r && (!usage.usable_bytes || !usage.free_bytes)) r = -ENODEV;
    }
    if (!r) {
        uint64_t base = 0, size = 0;
        step = "gart window matches properties";
        r = rt_gart_get_window(&base, &size);
        if (!r && (!properties_valid(&properties) ||
                   properties.gart_start != base ||
                   properties.gart_bytes != size))
            r = -ENODEV;
    }
    /* Runtime queues need either a legacy HQD the partition leaves free or
     * KFD sessions (MES user queues); KFD-backed clients do not use the
     * legacy HQDs at all. */
    production.legacy_queues = false;
    if (!r) {
        const int legacy = dext_aql_available(ctx);
        step = "runtime queues";
        production.legacy_queues = legacy == 0;
        if (legacy && (production.kfd_disabled || dext_kfd_supported(ctx) != 0))
            r = legacy;
    }
    if (r) {
        start_failed_step = step;
        start_failed_error = r;
        if (rt_compute_close(ctx) != 0) {
            freeze(false);
            return -EBUSY_L;
        }
        production.ctx = NULL;
        return -ENOTREADY_L;
    }
    production.properties = properties;
    production.ready = true;
    production.uncertain = false;
    runtime_available_update();
    return 0;
}

int dext_compute_stop(void)
{
    unsigned i;
    if (!production.ctx)
        return 0;
    /* Only KFD clients may have frozen the device for closing them to be
     * worth trying: their closes retry their recovery. */
    if (production.uncertain && !production.uncertain_kfd_only && !production.removed)
        return -EBUSY_L;
    production.ready = false;
    __atomic_store_n(&production.runtime_available, false, __ATOMIC_RELEASE);
    /* KFD processes first: their queues leave MES and their memory is
     * freed before the kernel BOs and the context go. */
    {
        bool kept = false;
        /* Every client is tried, even after one is kept. */
        for (i = 0; i < DEXT_CLIENT_SLOTS; ++i) {
            struct production_client *rec = &production.clients[i];
            if (rec->kfd && kfd_release(rec) != 0) {
                /* A removed device runs nothing: the record goes even
                 * when its close could not finish (its memory leaks). */
                if (production.removed) {
                    rec->kfd = NULL;
                    rec->mode = PRODUCTION_UNDECIDED;
                } else {
                    kept = true;
                }
            }
            if (rec->mode != PRODUCTION_KFD)
                rec->mode = PRODUCTION_UNDECIDED;
        }
        if (kept) return -EBUSY_L;
    }
    /* Every KFD client closed: what froze the device has let go. */
    if (production.uncertain && !thaw_kfd(0))
        return -EBUSY_L;
    for (i = 0; i < DEXT_QUEUE_SLOTS; ++i) {
        struct production_queue *q = &production.queues[i];
        if (!q->object)
            continue;
        if (dext_aql_destroy(q->object) != 0 && !production.removed) {
            freeze(false);
            return -EBUSY_L;
        }
        memset(q, 0, sizeof(*q));
    }
    if (production.removed) production.uncertain = false;
    for (i = 0; i < DEXT_BO_SLOTS; ++i) {
        struct production_bo *bo = &production.bos[i];
        if (!bo->object)
            continue;
        if (production_bo_drop(bo)) return -EBUSY_L;
    }
    if (rt_compute_close(production.ctx) != 0) {
        freeze(false);
        return -EBUSY_L;
    }
    production.ctx = NULL;
    memset(&production.properties, 0, sizeof(production.properties));
    return 0;
}

void dext_compute_set_hooks(dext_compute_open_hook open_hook,
                            dext_compute_gpu_op_hook gpu_op_hook)
{
    (void)open_hook;
    (void)gpu_op_hook;
}
void dext_compute_set_pci_open(bool open) { (void)open; }
void dext_compute_set_stage(enum dext_compute_stage stage) { (void)stage; }
void dext_compute_set_vram(uint64_t base, uint64_t visible, uint64_t real)
{ (void)base; (void)visible; (void)real; }
void dext_compute_set_gfx_version(uint32_t major, uint32_t minor, uint32_t revision)
{ (void)major; (void)minor; (void)revision; }
void dext_compute_set_props(const struct dext_compute_props *props) { (void)props; }
void dext_compute_set_claimed(bool claimed) { (void)claimed; }
void dext_compute_reset(void) { (void)dext_compute_stop(); }

int dext_compute_runtime_build(uint64_t *out)
{
    if (!out)
        return -EINVAL_L;
    out[0] = DEXT_RUNTIME_MAGIC;
    out[1] = DEXT_RUNTIME_ABI;
    out[2] = production_ready() &&
             (dext_aql_available(production.ctx) == 0 ||
              (!production.kfd_disabled && dext_kfd_supported(production.ctx) == 0)) ?
             DEXT_RUNTIME_BUILD : 0;
    return 0;
}

int dext_compute_runtime_build_cached(uint64_t *out)
{
    if (!out)
        return -EINVAL_L;
    out[0] = DEXT_RUNTIME_MAGIC;
    out[1] = DEXT_RUNTIME_ABI;
    /* Flags the session queue publishes: never the context itself, which
     * may be closing on that queue. */
    out[2] = __atomic_load_n(&production.ready, __ATOMIC_ACQUIRE) &&
             !__atomic_load_n(&production.uncertain, __ATOMIC_ACQUIRE) &&
             __atomic_load_n(&production.runtime_available, __ATOMIC_ACQUIRE) ?
             DEXT_RUNTIME_BUILD : 0;
    out[3] = DEXT_RUNTIME_BUILD;
    return 0;
}

int dext_compute_quiescent(void)
{
    return !production.ctx && !production.uncertain;
}

int dext_compute_query_info(uint64_t tag, uint64_t *out, int out_cap)
{
    const struct rt_compute_properties *p = &production.properties;
    if (!out)
        return -EINVAL_L;
    if (tag == 4) {
        if (out_cap < 1) return -EINVAL_L;
        out[0] = production_ready() ? DEXT_COMPUTE_STAGE_SDMA_INIT :
                                   DEXT_COMPUTE_STAGE_NONE;
        return 1;
    }
    if (!production_ready())
        return -ENOTREADY_L;
    switch (tag) {
    case 1:
        if (out_cap < 3) return -EINVAL_L;
        out[0] = (p->gfx_ip_version >> 24) & 255;
        out[1] = (p->gfx_ip_version >> 16) & 255;
        out[2] = (p->gfx_ip_version >> 8) & 255;
        return 3;
    case 2:
        if (out_cap < 2) return -EINVAL_L;
        out[0] = p->visible_vram_bytes;
        out[1] = p->vram_bytes;
        return 2;
    case 3:
        if (out_cap < 4) return -EINVAL_L;
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t v = p->ip_versions[i];
            out[i] = ((uint64_t)((v >> 24) & 255) << 16) |
                     ((uint64_t)((v >> 16) & 255) << 8) |
                     ((v >> 8) & 255);
        }
        return 4;
    case 6:
        if (out_cap < 10) return -EINVAL_L;
        out[0] = p->pci_chip_id;
        out[1] = p->pci_revision;
        out[2] = ((uint64_t)p->pci_bus << 8) |
                 ((uint64_t)p->pci_device << 3) | p->pci_function;
        out[3] = p->pci_domain;
        out[4] = p->cu_count;
        out[5] = p->shader_engines;
        out[6] = p->shader_arrays_per_engine;
        out[7] = p->timestamp_frequency_hz;
        out[8] = (uint64_t)p->simd_per_cu * p->max_waves_per_simd;
        out[9] = p->wavefront_size;
        return 10;
    case 10: { /* compute topology, version 1 */
        struct rt_compute_topology t;
        struct dext_aql_limits limits;
        struct dext_kfd_client *kfd;
        if (out_cap < 16) return -EINVAL_L;
        kfd = selected_kfd();
        if (rt_device_topology(rt_compute_device(production.ctx), &t) != 0)
            return -ENOTREADY_L;
        if (kfd) {
            /* KFD user queues: word 7 is KFD's per-process limit. */
            struct dext_kfd_info info;
            if (dext_kfd_info(kfd, &info) != 0 || dext_kfd_queue_abi(&limits) != 0)
                return -ENOTREADY_L;
            limits.slots = info.slots;
        } else if (!production.legacy_queues) {
            /* A legacy client on a device without a free legacy HQD. */
            if (dext_kfd_queue_abi(&limits) != 0) return -ENOTREADY_L;
            limits.slots = 0;
        } else if (dext_aql_limits(production.ctx, &limits) != 0)
            return -ENOTREADY_L;
        memset(out, 0, 16 * sizeof(*out));
        out[0] = 1;
        out[1] = t.gfx_target_version;
        out[2] = t.simd_per_cu;
        out[3] = t.max_waves_per_simd;
        out[4] = t.lds_bytes;
        out[5] = limits.max_private_bytes;
        out[6] = t.scratch_slots_per_cu;
        out[7] = limits.slots;
        out[8] = limits.min_packets | ((uint64_t)limits.max_packets << 32);
        out[9] = t.l1_bytes;
        out[10] = t.l2_bytes;
        out[11] = t.l3_bytes;
        out[12] = t.max_engine_clock_mhz;
        out[13] = (t.xnack & 3) | ((uint64_t)(t.sramecc & 3) << 2);
        out[14] = t.xcc_count;
        return 16;
    }
    case 11: { /* product name, NUL-padded ASCII */
        struct rt_compute_topology t;
        if (out_cap < 8) return -EINVAL_L;
        if (rt_device_topology(rt_compute_device(production.ctx), &t) != 0)
            return -ENOTREADY_L;
        _Static_assert(sizeof(t.product_name) == 8 * sizeof(uint64_t),
                       "product name words");
        memcpy(out, t.product_name, sizeof(t.product_name));
        return 8;
    }
    case 12: { /* compute session, version 1 (asking selects the mode) */
        struct production_client *rec;
        if (out_cap < 8) return -EINVAL_L;
        rec = selected_session();
        if (!rec || rec->mode == PRODUCTION_UNDECIDED) return -ENOTREADY_L;
        memset(out, 0, 8 * sizeof(*out));
        out[0] = 1;
        out[1] = rec->mode;
        rec->aware = true;
        if (rec->mode == PRODUCTION_KFD) {
            struct dext_kfd_info info;
            if (dext_kfd_info(rec->kfd, &info) != 0) return -ENOTREADY_L;
            out[2] = info.slots;
            out[3] = (uint64_t)info.pid;
            out[4] = info.window_base;
            out[5] = info.window_size;
            out[6] = info.gpuvm_base;
            out[7] = info.gpuvm_limit;
        } else {
            struct dext_aql_limits limits;
            uint64_t base = 0, size = 0;
            if (rt_gart_get_window(&base, &size) != 0) return -ENOTREADY_L;
            out[2] = production.legacy_queues &&
                     dext_aql_limits(production.ctx, &limits) == 0 ? limits.slots : 0;
            out[4] = base;
            out[5] = size;
        }
        return 8;
    }
    case 9: {
        struct rt_compute_memory_usage usage;
        if (out_cap < 6) return -EINVAL_L;
        if (rt_compute_memory_usage(production.ctx, &usage) != 0)
            return -ENOTREADY_L;
        out[0] = usage.total_bytes;
        out[1] = usage.usable_bytes;
        out[2] = usage.used_bytes;
        out[3] = usage.free_bytes;
        out[4] = usage.visible_bytes;
        out[5] = usage.visible_used_bytes;
        return 6;
    }
    case 5: /* No upstream per-client VRAM accounting snapshot yet. */
    case 8: /* No raw GC register spec observer yet. */
    default:
        return -ENOTREADY_L;
    }
}

int dext_compute_bo_alloc(uint64_t size, uint32_t domain, uint64_t alignment,
                          uint64_t flags, uint64_t *out_handle,
                          uint64_t *out_gpu_va, uint64_t *out_cpu_addr)
{
    struct production_bo *slot = NULL;
    struct rt_compute_bo *object = NULL;
    struct rt_compute_bo_info info;
    enum rt_compute_domain rt_domain;
    unsigned i;
    int r;
    if (!out_handle || !size || flags || !production_ready())
        return -ENOTREADY_L;
    if (domain == DEXT_COMPUTE_BO_DOMAIN_GTT ||
        domain == DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY)
        rt_domain = RT_COMPUTE_GTT;
    else if (domain == DEXT_COMPUTE_BO_DOMAIN_VRAM ||
             domain == DEXT_COMPUTE_BO_DOMAIN_DEVICE_VRAM)
        rt_domain = RT_COMPUTE_VRAM;
    else
        return -EINVAL_L;
    for (i = 0; i < DEXT_BO_SLOTS; ++i)
        if (!bo_live(&production.bos[i])) { slot = &production.bos[i]; break; }
    if (!slot || production.next_bo_handle == UINT64_MAX)
        return -ENOMEM_L;
    if (!alignment) alignment = 4096;
    {
        struct production_client *rec = selected_session();
        if (rec && rec->mode == PRODUCTION_KFD) {
            struct rt_kfd_bo *kfd_bo = NULL;
            uint64_t va = 0;
            /* A client that never configured a window keeps the host window
             * it already has: the GART window range. */
            if (rt_domain == RT_COMPUTE_GTT && !rec->aware) {
                struct dext_kfd_info info;
                uint64_t base = 0, bytes = 0;
                if (dext_kfd_info(rec->kfd, &info) == 0 && !info.window_base &&
                    rt_gart_get_window(&base, &bytes) == 0)
                    (void)dext_kfd_set_window(rec->kfd, base, bytes);
            }
            r = dext_kfd_bo_alloc(rec->kfd, size, alignment,
                                  rt_domain == RT_COMPUTE_GTT ? 2 : 3, &kfd_bo, &va);
            if (r) return kfd_error(rec->kfd, r);
            slot->kfd = kfd_bo;
            slot->owner = production.selected_client;
            slot->handle = ++production.next_bo_handle;
            slot->size = size;
            slot->alignment = alignment;
            slot->gpu_address = va;
            slot->domain = domain;
            slot->host_visible = rt_domain == RT_COMPUTE_GTT;
            *out_handle = slot->handle;
            if (out_gpu_va) *out_gpu_va = va;
            if (out_cpu_addr) *out_cpu_addr = 0; /* CPU pointers never cross RPC. */
            return 0;
        }
    }
    r = rt_compute_bo_alloc(production.ctx, size, alignment, rt_domain, &object);
    if (r) return r == -ENOMEM ? -ENOMEM_L :
                  (r == -EBUSY ? -EBUSY_L : -ENOTREADY_L);
    slot->object = object;
    slot->owner = production.selected_client;
    r = rt_compute_bo_info(production.ctx, object, &info);
    if (r || !info.gpu_address || info.size < size ||
        (rt_domain == RT_COMPUTE_GTT &&
         (!info.cpu_address || !info.shared_descriptor_available))) {
        if (rt_compute_bo_free(production.ctx, object) != 0)
            freeze(false);
        else
            memset(slot, 0, sizeof(*slot));
        return -ENOTREADY_L;
    }
    slot->handle = ++production.next_bo_handle;
    slot->size = size;
    slot->alignment = alignment;
    slot->gpu_address = info.gpu_address;
    slot->cpu_address = info.cpu_address;
    slot->domain = domain;
    *out_handle = slot->handle;
    if (out_gpu_va) *out_gpu_va = info.gpu_address;
    if (out_cpu_addr) *out_cpu_addr = 0; /* CPU pointers never cross RPC. */
    return 0;
}

int dext_compute_bo_free(uint64_t handle)
{
    struct production_bo *bo = production_bo_lookup(handle);
    unsigned i;
    if (!bo) return -ENOENT_L;
    if (production.uncertain) return -EBUSY_L;
    for (i = 0; i < DEXT_QUEUE_SLOTS; ++i) {
        const struct production_queue *q = &production.queues[i];
        if (queue_live(q) && q->owner == bo->owner &&
            (q->ring_handle == handle || q->metadata_handle == handle))
            return -EBUSY_L;
    }
    return production_bo_drop(bo);
}

int dext_compute_bo_export(uint64_t handle, uint64_t first, uint64_t second,
                            uint64_t out[3])
{
    if (!out) return -EINVAL_L;
    memset(out, 0, 3 * sizeof(*out));
    if (!production_ready()) return -ENOTREADY_L;
    struct production_bo *bo = production_bo_lookup(handle);
    /* A KFD process's BO lives in its own GPUVM: sharing it needs the
     * dma-buf export/import path, which is not wired yet. */
    if (bo && bo->kfd) return -ENOTREADY_L;
    if (!bo || bo->domain != DEXT_COMPUTE_BO_DOMAIN_DEVICE_VRAM ||
        (!first && !second)) return -EINVAL_L;
    struct production_export *empty = NULL, *published = NULL;
    for (unsigned i = 0; i < DEXT_EXPORT_SLOTS; ++i) {
        struct production_export *entry = &production.exports[i];
        if (entry->object == bo->object) published = entry;
        if (!entry->object && !empty) empty = entry;
    }
    if (!published) {
        for (unsigned i = 0; i < DEXT_EXPORT_SLOTS; ++i)
            if (production.exports[i].object &&
                production.exports[i].token[0] == first &&
                production.exports[i].token[1] == second) return -EINVAL_L;
        if (!empty) return -ENOMEM_L;
        *empty = (struct production_export){bo->object, {first, second}};
        published = empty;
    }
    out[0] = published->token[0]; out[1] = published->token[1]; out[2] = bo->size;
    return 0;
}

int dext_compute_bo_import(uint64_t first, uint64_t second, uint64_t size,
                            uint64_t out[3])
{
    if (!out) return -EINVAL_L;
    memset(out, 0, 3 * sizeof(*out));
    if (!production_ready()) return -ENOTREADY_L;
    if ((!first && !second) || !size) return -EINVAL_L;
    if (selected_kfd()) return -ENOTREADY_L; /* see dext_compute_bo_export */
    struct rt_compute_bo *object = NULL;
    for (unsigned i = 0; i < DEXT_EXPORT_SLOTS; ++i)
        if (production.exports[i].object && production.exports[i].token[0] == first &&
            production.exports[i].token[1] == second) object = production.exports[i].object;
    if (!object) return -ENOENT_L;
    struct production_bo *source = NULL, *empty = NULL;
    for (unsigned i = 0; i < DEXT_BO_SLOTS; ++i) {
        if (production.bos[i].object == object) source = &production.bos[i];
        if (!bo_live(&production.bos[i]) && !empty) empty = &production.bos[i];
    }
    if (!source || source->size != size) return -EINVAL_L;
    if (!empty || production.next_bo_handle == UINT64_MAX) return -ENOMEM_L;
    *empty = *source;
    empty->handle = ++production.next_bo_handle;
    empty->owner = production.selected_client;
    empty->memory_type = 0;
    out[0] = empty->handle; out[1] = empty->gpu_address; out[2] = empty->size;
    return 0;
}

int dext_compute_bo_get_info(uint64_t handle, uint64_t *out)
{
    struct production_bo *bo = production_bo_lookup(handle);
    if (!bo || !out) return -ENOENT_L;
    out[0] = bo->gpu_address;
    out[1] = 0;
    out[2] = bo->size;
    out[3] = bo->alignment;
    out[4] = bo->domain |
             ((uint64_t)(bo->cpu_address != NULL || bo->host_visible) << 8);
    return 0;
}

int dext_compute_bo_map(uint64_t handle, uint64_t *out)
{
    struct production_bo *bo = production_bo_lookup(handle);
    if (!out || !bo) return -ENOENT_L;
    if (!production_ready() || !(bo->cpu_address || bo->host_visible) ||
        (bo->domain != DEXT_COMPUTE_BO_DOMAIN_GTT &&
         bo->domain != DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY))
        return -ENOTREADY_L;
    if (!bo->memory_type) {
        if (production.next_memory_type == UINT32_MAX - DEXT_BO_MEMORY_BASE)
            return -ENOMEM_L;
        bo->memory_type = DEXT_BO_MEMORY_BASE + ++production.next_memory_type;
    }
    out[0] = bo->memory_type;
    out[1] = bo->size;
    return 0;
}

int dext_compute_bo_memory(uint32_t memory_type, void **cpu, uint64_t *size)
{
    if (!cpu || !size || memory_type < DEXT_BO_MEMORY_BASE)
        return -EINVAL_L;
    *cpu = NULL; *size = 0;
    if (!production_ready()) return -ENOTREADY_L;
    for (unsigned i = 0; i < DEXT_BO_SLOTS; ++i) {
        struct production_bo *bo = &production.bos[i];
        if (bo->object && bo->memory_type == memory_type &&
            bo->owner == production.selected_client && bo->cpu_address) {
            *cpu = bo->cpu_address; *size = bo->size; return 0;
        }
        /* A KFD GTT BO's pages are not one allocation: map its ranges. */
        if (bo->kfd && bo->memory_type == memory_type &&
            bo->owner == production.selected_client && bo->host_visible) {
            *size = bo->size;
            return -EAGAIN_L;
        }
    }
    return -ENOENT_L;
}

int dext_compute_bo_memory_ranges(uint32_t memory_type,
                                  int (*fn)(void *arg, void *cpu, uint64_t bytes),
                                  void *arg)
{
    if (!fn || memory_type < DEXT_BO_MEMORY_BASE) return -EINVAL_L;
    if (!production_ready()) return -ENOTREADY_L;
    for (unsigned i = 0; i < DEXT_BO_SLOTS; ++i) {
        struct production_bo *bo = &production.bos[i];
        if (!bo->kfd || bo->memory_type != memory_type ||
            bo->owner != production.selected_client || !bo->host_visible)
            continue;
        struct dext_kfd_client *c = owner_kfd(bo->owner);
        int r = c ? dext_kfd_bo_ranges(c, bo->kfd, fn, arg) : -ENOENT;
        return r ? kfd_error(c, r) : 0;
    }
    return -ENOENT_L;
}

int dext_compute_bo_copy(uint64_t source, uint64_t destination,
                         uint64_t source_offset, uint64_t destination_offset,
                         uint32_t bytes)
{
    struct production_bo *src = production_bo_lookup(source);
    struct production_bo *dst = production_bo_lookup(destination);
    struct rt_compute_fence *fence = NULL;
    int r;
    if (!src || !dst || !bytes) return -EINVAL_L;
    if (!production_ready()) return -ENOTREADY_L;
    if (src->kfd || dst->kfd) {
        struct dext_kfd_client *c = owner_kfd(src->owner);
        if (!src->kfd || !dst->kfd || !c) return -EINVAL_L;
        r = dext_kfd_bo_copy(c, src->kfd, source_offset, dst->kfd, destination_offset, bytes);
        return r ? kfd_error(c, r) : 0;
    }
    r = rt_compute_bo_copy(production.ctx, src->object, dst->object,
                           source_offset, destination_offset, bytes, &fence);
    if (!r) r = rt_compute_fence_wait(fence);
    rt_compute_fence_put(fence);
    if (r && rt_compute_status(production.ctx) == -EBUSY)
        freeze(false);
    return r ? (production.uncertain ? -EBUSY_L : -ENOTREADY_L) : 0;
}

int dext_compute_bo_write(uint64_t handle, uint64_t offset,
                          const void *source, size_t bytes)
{
    struct production_bo *bo = production_bo_lookup(handle);
    if (!bo || !source || !bytes) return -EINVAL_L;
    if (!production_ready()) return -ENOTREADY_L;
    if (offset > bo->size || bytes > bo->size - offset) return -EINVAL_L;
    if (bo->kfd) {
        struct dext_kfd_client *c = owner_kfd(bo->owner);
        int r = c ? dext_kfd_bo_write(c, bo->kfd, offset, source, bytes) : -ENOENT;
        return r ? kfd_error(c, r) : 0;
    }
    int r = rt_compute_bo_write(production.ctx, bo->object, offset, source, bytes);
    if (r && rt_compute_status(production.ctx) == -EBUSY)
        freeze(false);
    return r ? (production.uncertain ? -EBUSY_L : -ENOTREADY_L) : 0;
}

int dext_compute_bo_read(uint64_t handle, uint64_t offset,
                         void *destination, size_t bytes)
{
    struct production_bo *bo = production_bo_lookup(handle);
    if (!bo || !destination || !bytes) return -EINVAL_L;
    if (!production_ready()) return -ENOTREADY_L;
    if (offset > bo->size || bytes > bo->size - offset) return -EINVAL_L;
    if (bo->kfd) {
        struct dext_kfd_client *c = owner_kfd(bo->owner);
        int r = c ? dext_kfd_bo_read(c, bo->kfd, offset, destination, bytes) : -ENOENT;
        return r ? kfd_error(c, r) : 0;
    }
    int r = rt_compute_bo_read(production.ctx, bo->object, offset,
                               destination, bytes);
    if (r && rt_compute_status(production.ctx) == -EBUSY)
        freeze(false);
    return r ? (production.uncertain ? -EBUSY_L : -ENOTREADY_L) : 0;
}

/* Selector 51: the cache invalidation/writeback that brackets a launch of
 * freshly written code runs on the kernel compute ring with upstream's
 * per-family ACQUIRE_MEM, just before the launch's AQL queue is mapped. */
struct dispatch_sync {
    uint32_t timeout_us;
    uint64_t fence;
    int submitted;
};
static int dispatch_cache_sync(void *arg)
{
    struct dispatch_sync *sync = (struct dispatch_sync *)arg;
    int r = rt_compute_cache_sync(production.ctx, sync->timeout_us, &sync->fence);
    sync->submitted = sync->fence != 0;
    return r;
}

int dext_compute_rsrc1_clamp_ieee(void)
{
    return production_ready() && dext_aql_rsrc1_clamp_ieee(production.ctx);
}

/* A raw shader launch, run as a one-shot AQL dispatch on a DriverKit HQD
 * (dext_aql_dispatch_code) after an upstream cache sync whose fence
 * sequence is the launch's fence. */
int dext_compute_dispatch(const void *request, size_t request_size, uint64_t *out)
{
    struct rt_dispatch_request wire = {0};
    struct production_bo *code;
    struct dispatch_sync sync = {0};
    uint64_t aql[5];
    int uncertain = 0, r;
    if (!request || !out ||
        (request_size != sizeof(wire) && request_size != 264))
        return -EINVAL_L;
    memcpy(&wire, request, request_size);
    if ((request_size == 264 && wire.version != 1) ||
        (request_size == sizeof(wire) && wire.version != 2))
        return -EINVAL_L;
    if (!production_ready()) return -ENOTREADY_L;
    code = production_bo_lookup(wire.code_handle);
    if (!code || code->domain == DEXT_COMPUTE_BO_DOMAIN_GTT ||
        code->domain == DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY ||
        wire.code_offset > code->size ||
        wire.code_bytes > code->size - wire.code_offset ||
        wire.code_offset > UINT64_MAX - code->gpu_address)
        return -EINVAL_L;
    for (unsigned i = 0; i < 16; ++i)
        if (wire.buffers[i] && !production_bo_lookup(wire.buffers[i]))
            return -ENOENT_L;
    sync.timeout_us = wire.timeout_us;
    if (code->kfd) {
        /* On a short-lived KFD queue of the client's own process. */
        struct dext_kfd_client *c = owner_kfd(code->owner);
        if (!c) return -ENOTREADY_L;
        r = dext_kfd_dispatch_code(c, code->gpu_address + wire.code_offset,
                                   &wire, sizeof(wire), dispatch_cache_sync, &sync,
                                   aql, &uncertain);
        if (dext_kfd_uncertain(c)) uncertain = 1;
    } else {
        if (!production.legacy_queues) return -ENOMEM_L;
        r = dext_aql_dispatch_code(production.ctx, code->gpu_address + wire.code_offset,
                                   &wire, sizeof(wire), dispatch_cache_sync, &sync,
                                   aql, &uncertain);
    }
    /* A submitted cache sync that did not complete may still be running. */
    if (uncertain) freeze(code->kfd != NULL);
    if (r && sync.submitted) freeze(false);
    if (r) {
        if (production.uncertain) return -EBUSY_L;
        if (r == -EINVAL) return -EINVAL_L;
        /* Every DriverKit HQD held: refused before any hardware access. */
        if (r == -ENOSPC) return -ENOMEM_L;
        return -ENOTREADY_L;
    }
    if (!sync.fence) { freeze(false); return -EBUSY_L; }
    out[0] = 0;
    out[1] = sync.fence;
    out[2] = 3;
    return 0;
}

int dext_compute_aql_queue_create(uint64_t ring_handle,
                                  uint64_t metadata_handle, uint64_t packets,
                                  uint64_t *out_status, uint64_t *out_handle)
{
    struct production_bo *ring = production_bo_lookup(ring_handle);
    struct production_bo *meta = production_bo_lookup(metadata_handle);
    struct production_queue *slot = NULL;
    struct dext_aql_queue *queue = NULL;
    struct dext_aql_limits limits;
    int r;
    struct dext_kfd_client *kfd;
    if (!out_status || !out_handle || !ring || !meta || ring == meta ||
        !packets || (packets & (packets - 1)))
        return -EINVAL_L;
    if (!production_ready()) return -ENOTREADY_L;
    kfd = selected_kfd();
    if (kfd ? dext_kfd_queue_abi(&limits) != 0 :
              dext_aql_limits(production.ctx, &limits) != 0) return -ENOTREADY_L;
    if (packets < limits.min_packets || packets > limits.max_packets)
        return -EINVAL_L;
    for (unsigned i = 0; i < DEXT_QUEUE_SLOTS; ++i) {
        struct production_queue *q = &production.queues[i];
        if (queue_live(q) && q->owner == production.selected_client &&
            (q->ring_handle == ring_handle || q->metadata_handle == ring_handle ||
             q->ring_handle == metadata_handle || q->metadata_handle == metadata_handle))
            return -EBUSY_L;
        if (!queue_live(q) && !slot) slot = q;
    }
    if (!slot || production.next_queue_handle == UINT64_MAX) return -ENOMEM_L;
    if (kfd) {
        /* A KFD user queue: KFD's DQM adds it to MES. Its count is bounded
         * by KFD's per-process limit, not by the legacy HQDs. */
        struct dext_kfd_queue *kq = NULL;
        if (!ring->kfd || !meta->kfd) return -EINVAL_L;
        r = dext_kfd_queue_create(kfd, ring->kfd, meta->kfd, (uint32_t)packets, &kq);
        if (r) return kfd_error(kfd, r);
        slot->kfd = kq;
        slot->owner = production.selected_client;
        slot->handle = ++production.next_queue_handle;
        slot->ring_handle = ring_handle;
        slot->metadata_handle = metadata_handle;
        *out_status = 0;
        *out_handle = slot->handle;
        return 0;
    }
    if (ring->kfd || meta->kfd) return -EINVAL_L;
    if (!production.legacy_queues) return -ENOMEM_L; /* no legacy HQD at all */
    r = dext_aql_create(production.ctx, ring->object, meta->object,
                        (uint32_t)packets, &queue);
    if (queue) {
        slot->object = queue;
        slot->owner = production.selected_client;
        slot->handle = ++production.next_queue_handle;
        slot->ring_handle = ring_handle;
        slot->metadata_handle = metadata_handle;
    }
    if (r) {
        if (queue) freeze(false);
        /* Every DriverKit HQD is held: refused before any allocation or
         * hardware access, so nothing is uncertain. */
        if (!queue && r == -ENOSPC) return -ENOMEM_L;
        return queue ? -EBUSY_L : -ENOTREADY_L;
    }
    if (!queue) return -ENOTREADY_L;
    *out_status = 0;
    *out_handle = slot->handle;
    return 0;
}

/* Signal events (selector 86). */
int dext_compute_event(uint32_t op, uint32_t id, uint64_t out[3])
{
    if (!out) return -EINVAL_L;
    out[0] = out[1] = out[2] = 0;
    if (!production_ready()) return -ENOTREADY_L;
    struct dext_kfd_client *c = selected_kfd();
    if (!c) return -ENOTREADY_L;
    int r;
    switch (op) {
    case DEXT_COMPUTE_EVENT_CREATE: {
        uint32_t event = 0, trigger = 0;
        uint64_t mailbox = 0;
        r = dext_kfd_event_create(c, &event, &trigger, &mailbox);
        if (!r) {
            out[0] = event;
            out[1] = trigger;
            out[2] = mailbox;
        }
        break;
    }
    case DEXT_COMPUTE_EVENT_DESTROY:
        r = dext_kfd_event_destroy(c, id);
        break;
    case DEXT_COMPUTE_EVENT_SET:
        r = dext_kfd_event_set(c, id);
        break;
    default:
        return -EINVAL_L;
    }
    if (r == -ENOENT) return -ENOENT_L;
    if (r == -ENOSPC) return -ENOMEM_L;	/* KFD_SIGNAL_EVENT_LIMIT reached */
    return r ? kfd_error(c, r) : 0;
}

/* Selector 87: registered here, on the owner's queue; run off it. */
int dext_compute_event_wait_begin(const uint32_t *ids, uint32_t count, int all,
                                  uint32_t timeout_ms, struct rt_kfd_wait **out)
{
    if (!out) return -EINVAL_L;
    *out = NULL;
    if (!production_ready()) return -ENOTREADY_L;
    struct dext_kfd_client *c = selected_kfd();
    if (!c) return -ENOTREADY_L;
    const int r = dext_kfd_wait_begin(c, ids, count, all, timeout_ms, out);
    if (r == -ENOENT) return -ENOENT_L;
    if (r == -EINVAL) return -EINVAL_L;
    if (r == -EBUSY) return -EBUSY_L;
    return r ? -ENOTREADY_L : 0;
}

int dext_compute_aql_queue_kick(uint64_t handle, uint64_t wptr,
                                uint64_t *out_status)
{
    struct production_queue *q = production_queue_lookup(handle);
    if (!q || !out_status) return -ENOENT_L;
    if (!production_ready()) return -ENOTREADY_L;
    if (q->kfd) {
        struct dext_kfd_client *c = owner_kfd(q->owner);
        int r = dext_kfd_queue_kick(q->kfd, wptr);
        if (r) return kfd_error(c, r) == -EBUSY_L ? -EBUSY_L : -ENOTREADY_L;
        *out_status = 0;
        return 0;
    }
    int r = dext_aql_kick(q->object, wptr);
    if (dext_aql_uncertain(q->object)) {
        freeze(false);
        return -EBUSY_L;
    }
    if (r) return -ENOTREADY_L;
    *out_status = 0;
    return 0;
}

int dext_compute_aql_queue_destroy(uint64_t handle, uint64_t *out_status)
{
    struct production_queue *q = production_queue_lookup(handle);
    if (!q || !out_status) return -ENOENT_L;
    if (production.uncertain) return -EBUSY_L;
    if (q->kfd) {
        /* DESTROY_QUEUE: a removal MES did not confirm even after the
         * session's hung-queue recovery leaves MES running the queue. */
        if (dext_kfd_queue_destroy(q->kfd) != 0) {
            freeze(true);
            return -EBUSY_L;
        }
        memset(q, 0, sizeof(*q));
        *out_status = 0;
        return 0;
    }
    if (dext_aql_destroy(q->object) != 0) {
        freeze(false);
        return -EBUSY_L;
    }
    memset(q, 0, sizeof(*q));
    *out_status = 0;
    return 0;
}

int dext_compute_aql_queue_service(uint64_t handle, uint64_t *out_status,
                                   uint64_t *out_inactive)
{
    struct production_queue *q = production_queue_lookup(handle);
    uint64_t inactive = 0;
    if (!q || !out_status || !out_inactive) return -ENOENT_L;
    if (!production_ready()) return -ENOTREADY_L;
    if (q->kfd) {
        struct dext_kfd_client *c = owner_kfd(q->owner);
        int r = dext_kfd_queue_service(q->kfd, &inactive);
        if (r) return kfd_error(c, r) == -EBUSY_L ? -EBUSY_L : -ENOTREADY_L;
        *out_status = 0;
        *out_inactive = inactive;
        return 0;
    }
    int r = dext_aql_service(q->object, &inactive);
    if (dext_aql_uncertain(q->object)) {
        freeze(false);
        return -EBUSY_L;
    }
    if (r) return -ENOTREADY_L;
    *out_status = 0;
    *out_inactive = inactive;
    return 0;
}

int dext_compute_aql_dispatch(const void *request, size_t request_size,
                              uint64_t *out)
{
    struct {
        uint32_t version, flags;
        uint64_t code_handle, descriptor_offset;
        uint64_t kernarg_handle, kernarg_offset, kernarg_bytes;
        uint32_t groups[3], threads[3];
        uint32_t timeout_us, reserved;
        uint64_t buffers[16];
    } wire;
    struct production_bo *descriptor, *kernarg;
    uint64_t descriptor_va, kernarg_va;
    uint64_t threads = 1;
    int uncertain = 0, r;
    if (!request || !out || request_size != sizeof(wire))
        return -EINVAL_L;
    memcpy(&wire, request, sizeof(wire));
    if (wire.version != 1 || wire.flags || wire.reserved ||
        !wire.code_handle || !wire.kernarg_handle ||
        (wire.descriptor_offset & 63) || (wire.kernarg_offset & 15) ||
        wire.kernarg_bytes > 4u * 1024u * 1024u ||
        !wire.timeout_us || wire.timeout_us > 1000000)
        return -EINVAL_L;
    for (unsigned i = 0; i < 3; ++i) {
        if (!wire.groups[i] || !wire.threads[i] || wire.threads[i] > 1024 ||
            (uint64_t)wire.groups[i] * wire.threads[i] > UINT32_MAX)
            return -EINVAL_L;
        threads *= wire.threads[i];
        if (threads > 1024) return -EINVAL_L;
    }
    if (!production_ready()) return -ENOTREADY_L;
    descriptor = production_bo_lookup(wire.code_handle);
    kernarg = production_bo_lookup(wire.kernarg_handle);
    if (!descriptor || !kernarg ||
        descriptor->domain == DEXT_COMPUTE_BO_DOMAIN_GTT ||
        descriptor->domain == DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY ||
        kernarg->domain == DEXT_COMPUTE_BO_DOMAIN_GTT ||
        kernarg->domain == DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY ||
        wire.descriptor_offset > descriptor->size ||
        64 > descriptor->size - wire.descriptor_offset ||
        wire.kernarg_offset > kernarg->size ||
        wire.kernarg_bytes > kernarg->size - wire.kernarg_offset ||
        wire.descriptor_offset > UINT64_MAX - descriptor->gpu_address ||
        wire.kernarg_offset > UINT64_MAX - kernarg->gpu_address)
        return -EINVAL_L;
    for (unsigned i = 0; i < 16; ++i)
        if (wire.buffers[i] && !production_bo_lookup(wire.buffers[i]))
            return -ENOENT_L;
    descriptor_va = descriptor->gpu_address + wire.descriptor_offset;
    kernarg_va = kernarg->gpu_address + wire.kernarg_offset;
    if (descriptor->kfd || kernarg->kfd) {
        struct dext_kfd_client *c = owner_kfd(descriptor->owner);
        if (!descriptor->kfd || !kernarg->kfd || !c) return -EINVAL_L;
        r = dext_kfd_dispatch_bounded(c, descriptor_va, kernarg_va, request,
                                      request_size, out, &uncertain);
        if (dext_kfd_uncertain(c)) uncertain = 1;
    } else {
        if (!production.legacy_queues) return -ENOMEM_L;
        r = dext_aql_dispatch_bounded(production.ctx, descriptor_va, kernarg_va,
                                      request, request_size, out, &uncertain);
    }
    if (uncertain) freeze(descriptor->kfd != NULL);
    if (r) {
        if (production.uncertain) return -EBUSY_L;
        if (r == -EINVAL || r == -ERANGE) return -EINVAL_L;
        /* No free DriverKit HQD: refused before reserving one or touching
         * hardware (selector 55 reports kIOReturnNoResources). */
        if (r == -ENOSPC) return -ENOMEM_L;
        return -ENOTREADY_L;
    }
    return 0;
}

int dext_compute_host_window(uint64_t configure, uint64_t *out)
{
    uint64_t base = 0, size = 0;
    int r;
    if (!out) return -EINVAL_L;
    if (production_ready()) {
        /* A KFD-aware client's own window inside its process's GPUVM: query
         * reports it (base 0 until set) and the size to reserve; a nonzero
         * value sets it, once, before any shared buffer exists. */
        struct production_client *rec = selected_session();
        if (rec && rec->mode == PRODUCTION_KFD && rec->aware) {
            struct dext_kfd_info info;
            if (configure) {
                r = dext_kfd_set_window(rec->kfd, configure, 0);
                if (r) return r == -EBUSY ? -EBUSY_L : -EINVAL_L;
            }
            if (dext_kfd_info(rec->kfd, &info) != 0) return -ENOTREADY_L;
            out[0] = info.window_base;
            out[1] = info.window_size;
            out[2] = 1; /* shared buffers are coherent host pages, no GART probe */
            return 0;
        }
    }
    if (configure) {
        if (production.ctx) return -EBUSY_L;
        r = rt_gart_set_window(configure);
        if (r) return r == -EBUSY ? -EBUSY_L : -EINVAL_L;
    }
    r = rt_gart_get_window(&base, &size);
    if (r) return -ENOTREADY_L;
    out[0] = base;
    out[1] = size;
    out[2] = production_ready() && rt_gart_status() == 0 &&
             production.properties.gart_start == base &&
             production.properties.gart_bytes == size &&
             production.properties.host_memory_verified;
    return 0;
}

int dext_compute_shutdown(uint64_t *out)
{
    if (!out) return -EINVAL_L;
    int r = dext_compute_stop();
    out[0] = r ? 1 : 0;
    out[1] = r ? 2 : 6;
    return r;
}

int dext_compute_get_rebar(uint64_t bar, uint64_t *out)
{ (void)bar; (void)out; return -ENOTREADY_L; }
int dext_compute_host_mem_test(uint64_t size, uint64_t *out)
{ (void)size; (void)out; return -ENOTREADY_L; }
int dext_compute_wait_fence(uint64_t handle, uint64_t timeout_ns,
                            uint64_t *out_status)
{ (void)handle; (void)timeout_ns; (void)out_status; return -ENOTREADY_L; }
int dext_compute_submit_ib(uint64_t handle, uint64_t *out_fence)
{ (void)handle; (void)out_fence; return -ENOTREADY_L; }
int dext_compute_cs_create(uint32_t ip_type, uint32_t ip_instance,
                           uint64_t *out_handle)
{ (void)ip_type; (void)ip_instance; (void)out_handle; return -ENOTREADY_L; }
int dext_compute_cs_write_dwords(uint64_t handle, const uint32_t *dwords,
                                 uint32_t count, uint32_t *out_written)
{ (void)handle; (void)dwords; (void)count; (void)out_written; return -ENOTREADY_L; }
int dext_compute_cs_destroy(uint64_t handle)
{ (void)handle; return -ENOTREADY_L; }
