/* linuxu shim: dext_compute — the COMPUTE selector seam (T-dext-userclient-seam).
 *
 * The userspace HSA runtime (hsa/ libhsa-runtime64.dylib) talks to the dext
 * over the IOKit user-client selector RPC (MacLinuxGPUUserClient::ExternalMethod).
 * The COMPUTE selectors (RuntimeBuild=43, QueryInfo=21, BO*=16-18/36, AQLQueue*=56-59,
 * AQLDispatch=55, HostWindow=54, ShutdownGPU=42, HostMemoryTest=44,
 * WaitFence=20, SubmitIB=19, CS*=37-39, ...) all route to this seam.
 *
 * The DriverKit build routes operations to the probed upstream AMDGPU device
 * through dext_compute_backend.inc. The host build retains the in-memory
 * selector model for isolated host checks.
 *
 * The argument layouts are defined by the selector bodies in
 * MacLinuxGPUXcode.mm (the ExternalMethod dispatch), NOT here — this file is
 * the C API those bodies call.
 *
 * Hard constraint: this file compiles in BOTH the host build (LINUXU_DEXT
 * undefined) and the dext build (LINUXU_DEXT defined).  It must NOT include
 * DriverKit headers.  The #else host branch of dext_main.mm stays
 * byte-identical (this is a NEW file, not an edit of dext_main.mm).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---- error codes (match the linuxu convention, dext_compute.h) ---- */
#include "dext_compute.h"

#ifdef LINUXU_DEXT
#include "dext_compute_backend.inc"
#else

/* Feature gates must reflect implemented GPU operations.  The DriverKit
 * path does not yet initialize the GPU, so advertise no optional compute
 * feature generation to the ported HSA runtime. */
#define MACLINUXGPU_RUNTIME_BUILD  0u
#define MACLINUXGPU_RUNTIME_MAGIC  0x414D444750554142ull /* "AMDGPUAB" */
#define MACLINUXGPU_RUNTIME_ABI    1u

/* ---- BO table ---- */
#define DEXT_COMPUTE_MAX_BO  256
struct dext_compute_bo {
    bool     in_use;
    uint64_t handle;       /* the wire handle (== index+1) */
    uint64_t size;
    uint64_t alignment;
    uint32_t domain;       /* dext_compute_bo_domain */
    uint64_t byte_offset;  /* GTT-legacy: offset within the client DMA buffer */
    uint64_t gpu_va;       /* VRAM: the GPU virtual address */
    uint64_t vram_offset;  /* VRAM: gpu_va - vram_start */
    void    *cpu_addr;     /* GTT/legacy: the mapped CPU pointer (0 if unmapped) */
    uint64_t generation;   /* client-wide generation counter */
};

/* ---- persistent AQL queue table (the reference's b.aqlQueues) ---- */
#define DEXT_COMPUTE_MAX_AQL  8
struct dext_compute_aql_queue {
    bool     in_use;
    uint64_t handle;
    uint64_t ring_handle;     /* the BO handle of the ring */
    uint64_t metadata_handle; /* the BO handle of the metadata BO */
    uint64_t read_index;      /* the last read packet index (service state) */
    uint64_t kick_count;      /* monotonic kick counter */
    bool     retained;        /* firmware owns this until unmap/reset */
};

/* ---- CS (command stream) table (the reference's CS table) ---- */
#define DEXT_COMPUTE_MAX_CS    64
#define DEXT_COMPUTE_MAX_CS_DW 4096
struct dext_compute_cs {
    bool     in_use;
    uint64_t handle;
    uint32_t ip_type;         /* dext_compute_cs_ip */
    uint32_t ip_instance;     /* SDMA instance (0/1) or GFX (0) */
    uint32_t written_dw;
    uint32_t dwords[DEXT_COMPUTE_MAX_CS_DW];
    uint64_t last_fence;
};

/* ---- fence table (the reference's submission fence state) ---- */
#define DEXT_COMPUTE_MAX_FENCE 1024
struct dext_compute_fence {
    bool     in_use;
    uint64_t handle;
    uint64_t value;      /* the fence value (the SDMA/CP fence counter) */
    bool     signaled;   /* set when the GPU wrote the WB slot (host: set on
                            submit for the host-verifiable scope) */
};

/* ---- the device properties snapshot (QueryInfo tag 6) is in dext_compute.h ---- */

/* ---- global compute state (process-global; the dext is single-tenant per
 * IOService instance — the multi-GPU instance-ization is a separate track) ---- */
static struct {
    struct dext_compute_bo        bo[DEXT_COMPUTE_MAX_BO];
    struct dext_compute_aql_queue aql[DEXT_COMPUTE_MAX_AQL];
    struct dext_compute_cs        cs[DEXT_COMPUTE_MAX_CS];
    struct dext_compute_fence     fence[DEXT_COMPUTE_MAX_FENCE];
    uint64_t bo_gen;
    uint64_t next_aql_handle;
    uint64_t next_cs_handle;
    uint64_t next_fence;
    uint64_t vram_base;
    uint64_t vram_visible;
    uint64_t vram_real;
    uint64_t gart_start;
    uint64_t gart_size;
    bool gart_reads_verified;
    enum dext_compute_stage       stage;
    struct dext_compute_props     props;
    uint64_t gfx_major, gfx_minor, gfx_revision;
    bool     pci_open;          /* set when dext_open succeeds */
    bool     claimed;           /* an owned session claimed it (tag 6 gate) */
} g_compute;

/* ---- optional dext I/O hook (dext_compute_dk.mm provides it under
 * LINUXU_DEXT; the host build leaves it NULL) ---- */
static dext_compute_gpu_op_hook g_compute_gpu_op_hook = NULL;

static bool compute_backend_ready(void)
{
#ifdef LINUXU_DEXT
    return g_compute.pci_open &&
           g_compute.claimed &&
           g_compute.stage == DEXT_COMPUTE_STAGE_SDMA_INIT &&
           g_compute_gpu_op_hook != NULL;
#else
    return true;
#endif
}

void dext_compute_set_hooks(dext_compute_open_hook open_hook,
                            dext_compute_gpu_op_hook gpu_op_hook)
{
    (void)open_hook;
    g_compute_gpu_op_hook = gpu_op_hook;
}

void dext_compute_set_pci_open(bool open)
{
    g_compute.pci_open = open;
}
void dext_compute_set_stage(enum dext_compute_stage stage)
{
    g_compute.stage = stage;
}
void dext_compute_set_vram(uint64_t base, uint64_t visible, uint64_t real)
{
    g_compute.vram_base    = base;
    g_compute.vram_visible = visible;
    g_compute.vram_real    = real;
}
void dext_compute_set_gfx_version(uint32_t major, uint32_t minor, uint32_t revision)
{
    g_compute.gfx_major = major;
    g_compute.gfx_minor = minor;
    g_compute.gfx_revision = revision;
}
void dext_compute_set_props(const struct dext_compute_props *props)
{
    if (props)
        g_compute.props = *props;
}
void dext_compute_set_claimed(bool claimed)
{
    g_compute.claimed = claimed;
}

void dext_compute_reset(void)
{
    memset(&g_compute, 0, sizeof(g_compute));
}

/* ---- internal helpers ---- */
static struct dext_compute_bo *bo_lookup(uint64_t handle)
{
    if (!handle || handle > DEXT_COMPUTE_MAX_BO)
        return NULL;
    struct dext_compute_bo *e = &g_compute.bo[handle - 1];
    return (e->in_use && e->handle == handle) ? e : NULL;
}
static int bo_find_free(uint64_t *handle)
{
    for (uint64_t i = 0; i < DEXT_COMPUTE_MAX_BO; i++) {
        if (!g_compute.bo[i].in_use) {
            g_compute.bo[i].handle = i + 1;
            *handle = i + 1;
            return 0;
        }
    }
    return -ENOMEM_L;
}
static struct dext_compute_cs *cs_lookup(uint64_t handle)
{
    if (!handle || handle > DEXT_COMPUTE_MAX_CS)
        return NULL;
    struct dext_compute_cs *e = &g_compute.cs[handle - 1];
    return (e->in_use && e->handle == handle) ? e : NULL;
}
static int cs_find_free(uint64_t *handle)
{
    for (uint64_t i = 0; i < DEXT_COMPUTE_MAX_CS; i++) {
        if (!g_compute.cs[i].in_use) {
            g_compute.cs[i].handle = i + 1;
            *handle = i + 1;
            return 0;
        }
    }
    return -ENOMEM_L;
}

/* =============== the public compute seam C API =============== */

/* RuntimeBuild (43): out[0]=magic, out[1]=ABI, out[2]=build. */
int dext_compute_runtime_build(uint64_t *out)
{
    if (!out)
        return -EINVAL_L;
    out[0] = MACLINUXGPU_RUNTIME_MAGIC;
    out[1] = MACLINUXGPU_RUNTIME_ABI;
    out[2] = MACLINUXGPU_RUNTIME_BUILD;
    return 0;
}

int dext_compute_runtime_build_cached(uint64_t *out)
{
    if (!out)
        return -EINVAL_L;
    out[0] = MACLINUXGPU_RUNTIME_MAGIC;
    out[1] = MACLINUXGPU_RUNTIME_ABI;
    out[2] = MACLINUXGPU_RUNTIME_BUILD;
    out[3] = MACLINUXGPU_RUNTIME_BUILD;
    return 0;
}

/* The host model owns no GPU context or in-flight hardware work. */
int dext_compute_quiescent(void)
{
    return 1;
}

void dext_compute_device_removed(void) {}

bool dext_compute_client_legacy(uint64_t client) { (void)client; return false; }
bool dext_compute_client_owns(uint64_t client) { (void)client; return false; }
int dext_compute_forget_client(uint64_t client) { (void)client; return 0; }
unsigned dext_compute_client_records(void) { return 0; }
int dext_compute_client_open_error(uint64_t client) { (void)client; return 0; }

/* The device spec needs the upstream device: none in the host build. */
int dext_compute_device_spec(void *out, size_t cap)
{
    (void)out;
    (void)cap;
    return -ENOTREADY_L;
}

/* QueryInfo (21): in tag, out values[].  Returns the number of out values
 * written on success (the caller copies them to scalarOutput + sets
 * scalarOutputCount), or a -E*_L error. */
int dext_compute_query_info(uint64_t tag, uint64_t *out, int out_cap)
{
    if (!out)
        return -EINVAL_L;
    switch (tag) {
    case 1: { /* GFX version: out[0..2]=major,minor,rev */
        if (out_cap < 3) return -EINVAL_L;
        out[0] = g_compute.gfx_major;
        out[1] = g_compute.gfx_minor;
        out[2] = g_compute.gfx_revision;
        return 3;
    }
    case 2: { /* VRAM sizes: out[0]=visible, out[1]=real */
        if (out_cap < 2) return -EINVAL_L;
        out[0] = g_compute.vram_visible;
        out[1] = g_compute.vram_real;
        return 2;
    }
    case 3: { /* IP versions: 4 packed (maj<<16|min<<8|rev) */
        if (out_cap < 4) return -EINVAL_L;
        uint64_t gfx = ((uint64_t)g_compute.gfx_major << 16) |
                       ((uint64_t)g_compute.gfx_minor << 8) |
                       (uint64_t)g_compute.gfx_revision;
        out[0] = gfx; out[1] = gfx; out[2] = gfx; out[3] = gfx;
        return 4;
    }
    case 5: /* VRAM accounting requires initialized hardware counters. */
        return -ENOTREADY_L;
    case 4: { /* bringup reached: out[0]=stage */
        if (out_cap < 1) return -EINVAL_L;
        out[0] = (uint64_t)g_compute.stage;
        return 1;
    }
    case 6: { /* device topology (10 values) */
        if (out_cap < 10) return -EINVAL_L;
        if (!g_compute.claimed || !g_compute.pci_open ||
            g_compute.stage != DEXT_COMPUTE_STAGE_SDMA_INIT ||
            !g_compute.props.valid)
            return -ENOTREADY_L;
        const struct dext_compute_props *p = &g_compute.props;
        out[0] = p->chip_id;
        out[1] = p->revision;
        out[2] = p->bdf;
        out[3] = p->domain;
        out[4] = p->compute_units;
        out[5] = p->shader_engines;
        out[6] = p->arrays_per_engine;
        out[7] = p->timestamp_frequency;
        out[8] = p->max_waves_per_cu;
        out[9] = p->wavefront_size;
        return 10;
    }
    case 8: { /* device spec: 32 dwords; header(0)==0 = "not resolved" */
        if (out_cap < 32) return -EINVAL_L;
        if (!g_compute.pci_open ||
            g_compute.stage < DEXT_COMPUTE_STAGE_GFX_INIT)
            return -ENOTREADY_L;
        memset(out, 0, 32 * sizeof(uint64_t));
        return 32;
    }
    default:
        return -EINVAL_L;
    }
}

/* BOAlloc (16) */
int dext_compute_bo_alloc(uint64_t size, uint32_t domain, uint64_t alignment,
                          uint64_t flags, uint64_t *out_handle,
                          uint64_t *out_gpu_va, uint64_t *out_cpu_addr)
{
    if (!out_handle)
        return -EINVAL_L;
    if (!size || flags != 0)
        return -EINVAL_L;
    if (domain != DEXT_COMPUTE_BO_DOMAIN_VRAM &&
        domain != DEXT_COMPUTE_BO_DOMAIN_GTT &&
        domain != DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY &&
        domain != DEXT_COMPUTE_BO_DOMAIN_DEVICE_VRAM)
        return -EINVAL_L;
    if (!alignment)
        alignment = 4096;
    if (!compute_backend_ready())
        return -ENOTREADY_L;
#ifdef LINUXU_DEXT
    /* DART pinning alone does not give the GPU a GART MC address. Require a
     * configured host window with a verified GPU readback before GTT BOs can
     * be handed to userspace. */
    if (domain == DEXT_COMPUTE_BO_DOMAIN_GTT &&
        (!g_compute.gart_start || !g_compute.gart_size ||
         !g_compute.gart_reads_verified))
        return -ENOTREADY_L;
#endif

    uint64_t handle;
    int r = bo_find_free(&handle);
    if (r)
        return r;
    struct dext_compute_bo *e = &g_compute.bo[handle - 1];
    memset(e, 0, sizeof(*e));
    e->in_use = true;
    e->handle = handle;
    e->size = size;
    e->alignment = alignment;
    e->domain = domain;
    e->generation = ++g_compute.bo_gen;

    if (g_compute_gpu_op_hook) {
        uint64_t args[3] = { size, domain, alignment };
        uint64_t result[2] = { 0, 0 };
        if (g_compute_gpu_op_hook(0, args, result, sizeof(result)) != 0 ||
            result[0] == 0) {
            memset(e, 0, sizeof(*e));
            return -ENOTREADY_L;
        }
        e->gpu_va   = result[0];
        e->cpu_addr = (void *)result[1];
    }
    *out_handle = handle;
    if (out_gpu_va)   *out_gpu_va   = e->gpu_va;
    if (out_cpu_addr) *out_cpu_addr = (uint64_t)(uintptr_t)e->cpu_addr;
    return 0;
}

/* BOFree (17) */
int dext_compute_bo_free(uint64_t handle)
{
    struct dext_compute_bo *e = bo_lookup(handle);
    if (!e)
        return -ENOENT_L;
    for (int i = 0; i < DEXT_COMPUTE_MAX_AQL; i++) {
        struct dext_compute_aql_queue *q = &g_compute.aql[i];
        if (q->in_use && (q->ring_handle == handle ||
                          q->metadata_handle == handle))
            return -EAGAIN_L;
    }
    if (g_compute_gpu_op_hook) {
        uint64_t args[1] = { handle };
        if (g_compute_gpu_op_hook(1, args, NULL, 0) != 0)
            return -ENOTREADY_L;
    }
    memset(e, 0, sizeof(*e));
    return 0;
}

/* BOGetInfo (18) */
int dext_compute_bo_get_info(uint64_t handle, uint64_t *out)
{
    struct dext_compute_bo *e = bo_lookup(handle);
    if (!e)
        return -ENOENT_L;
    out[0] = e->gpu_va;
    out[1] = e->byte_offset;
    out[2] = e->size;
    out[3] = e->alignment;
    out[4] = (uint64_t)e->domain | (((uint64_t)(e->cpu_addr != NULL)) << 8);
    return 0;
}

/* BOMap (36) */
int dext_compute_bo_map(uint64_t handle, uint64_t *out)
{
    struct dext_compute_bo *e = bo_lookup(handle);
    if (!e)
        return -ENOENT_L;
#ifdef LINUXU_DEXT
    return -ENOTREADY_L; /* no BO descriptor export is wired yet */
#endif
    if (e->domain == DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY) {
        out[0] = 6; /* kMacAMDGPUMemoryTypeDMABuffer */
        out[1] = e->size;
        return 0;
    }
    if (e->domain == DEXT_COMPUTE_BO_DOMAIN_GTT) {
        out[0] = 0x10000 + (handle - 1); /* kMacAMDGPUMemoryTypeBOBase + idx */
        out[1] = e->size;
        return 0;
    }
    return -ENOTREADY_L; /* VRAM: BAR0-aperture re-export, p3-hw-gate */
}

/* AQLQueueCreate (56) */
int dext_compute_aql_queue_create(uint64_t ring_handle,
                                  uint64_t metadata_handle,
                                  uint64_t packets,
                                  uint64_t *out_status,
                                  uint64_t *out_handle)
{
    if (ring_handle == metadata_handle || packets < 64 || packets > 4096 ||
        (packets & (packets - 1)))
        return -EINVAL_L;
    struct dext_compute_bo *ring = bo_lookup(ring_handle);
    struct dext_compute_bo *meta = bo_lookup(metadata_handle);
    if (!ring || !meta || ring->size < packets * 64 || meta->size < 512)
        return -EINVAL_L;
    if (g_compute.stage != DEXT_COMPUTE_STAGE_SDMA_INIT)
        return -ENOTREADY_L;
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    int slot = -1;
    for (int i = 0; i < DEXT_COMPUTE_MAX_AQL; i++) {
        struct dext_compute_aql_queue *q = &g_compute.aql[i];
        if (q->in_use && (q->ring_handle == ring_handle ||
                          q->metadata_handle == ring_handle ||
                          q->ring_handle == metadata_handle ||
                          q->metadata_handle == metadata_handle))
            return -EAGAIN_L;
        if (!q->in_use && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return -ENOMEM_L;
    struct dext_compute_aql_queue *q = &g_compute.aql[slot];
    memset(q, 0, sizeof(*q));
    q->in_use = true;
    q->ring_handle = ring_handle;
    q->metadata_handle = metadata_handle;
    q->handle = ++g_compute.next_aql_handle;
    uint64_t status = 0;
    if (g_compute_gpu_op_hook) {
        uint64_t args[3] = { ring_handle, metadata_handle, packets };
        if (g_compute_gpu_op_hook(2, args, &status, sizeof(status)) != 0 ||
            status != 0) {
            memset(q, 0, sizeof(*q));
            return -ENOTREADY_L;
        }
    }
    *out_status = status;
    *out_handle = q->handle;
    return 0;
}

/* AQLQueueKick (57) */
int dext_compute_aql_queue_kick(uint64_t handle, uint64_t wptr,
                                uint64_t *out_status)
{
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    for (int i = 0; i < DEXT_COMPUTE_MAX_AQL; i++) {
        struct dext_compute_aql_queue *q = &g_compute.aql[i];
        if (q->in_use && q->handle == handle) {
            q->kick_count++;
            uint64_t status = 0;
            if (g_compute_gpu_op_hook) {
                uint64_t args[2] = { handle, wptr };
                if (g_compute_gpu_op_hook(3, args, &status, sizeof(status)) != 0)
                    return -ENOTREADY_L;
            }
            *out_status = status;
            return 0;
        }
    }
    return -ENOENT_L;
}

/* AQLQueueDestroy (58) */
int dext_compute_aql_queue_destroy(uint64_t handle, uint64_t *out_status)
{
    for (int i = 0; i < DEXT_COMPUTE_MAX_AQL; i++) {
        struct dext_compute_aql_queue *q = &g_compute.aql[i];
        if (q->in_use && q->handle == handle) {
            uint64_t status = 0;
            if (g_compute_gpu_op_hook) {
                uint64_t args[1] = { handle };
                if (g_compute_gpu_op_hook(4, args, &status, sizeof(status)) != 0)
                    status = (uint64_t)(-1);
            }
            if (!q->retained)
                memset(q, 0, sizeof(*q));
            *out_status = status;
            return 0;
        }
    }
    return -ENOENT_L;
}

/* AQLQueueService (59) */
int dext_compute_aql_queue_service(uint64_t handle, uint64_t *out_status,
                                   uint64_t *out_inactive)
{
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    for (int i = 0; i < DEXT_COMPUTE_MAX_AQL; i++) {
        struct dext_compute_aql_queue *q = &g_compute.aql[i];
        if (q->in_use && q->handle == handle) {
            uint64_t status = 0, inactive = 0;
            if (g_compute_gpu_op_hook) {
                uint64_t args[1] = { handle };
                uint64_t result[2] = { 0, 0 };
                if (g_compute_gpu_op_hook(5, args, result, sizeof(result)) == 0) {
                    status = result[0];
                    inactive = result[1];
                } else return -ENOTREADY_L;
            }
            *out_status = status;
            *out_inactive = inactive;
            return 0;
        }
    }
    return -ENOENT_L;
}

/* The reference backend has no cache to invalidate. */
int dext_compute_code_sync(uint64_t timeout_us)
{
    if (!timeout_us || timeout_us > 1000000u) return -EINVAL_L;
    return compute_backend_ready() ? 0 : -ENOTREADY_L;
}

/* The reference backend's queues never fault. */
int dext_compute_aql_queue_fault(uint64_t handle, uint64_t *out_flags, uint64_t *out_va)
{
    if (!out_flags || !out_va) return -EINVAL_L;
    *out_flags = 0;
    *out_va = 0;
    for (int i = 0; i < DEXT_COMPUTE_MAX_AQL; i++)
        if (g_compute.aql[i].in_use && g_compute.aql[i].handle == handle) return 0;
    return -ENOENT_L;
}

/* AQLDispatch (55): in the AQLDispatchRequest struct, out 5 scalars. */
int dext_compute_rsrc1_clamp_ieee(void) { return 0; }

int dext_compute_aql_dispatch(const void *request, size_t request_size,
                              uint64_t *out)
{
    if (!request || request_size < 208 || !out)
        return -EINVAL_L;
    if (g_compute.stage != DEXT_COMPUTE_STAGE_SDMA_INIT)
        return -ENOTREADY_L;
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    const uint8_t *r = (const uint8_t *)request;
    uint32_t version;
    memcpy(&version, r, 4);
    uint64_t code_handle, kernarg_handle;
    memcpy(&code_handle, r + 8, 8);
    memcpy(&kernarg_handle, r + 24, 8);
    if (version != 1 || !code_handle || !kernarg_handle)
        return -EINVAL_L;
    if (!bo_lookup(code_handle) || !bo_lookup(kernarg_handle))
        return -ENOENT_L;
    uint64_t status = 0, completion = 0, stage = 1, inactive = 0,
             read_index = 0;
    if (g_compute_gpu_op_hook) {
        uint64_t result[5] = { 0, 0, 1, 0, 0 };
        if (g_compute_gpu_op_hook(6, request, result, sizeof(result)) != 0)
            return -ENOTREADY_L;
        status = result[0]; completion = result[1]; stage = result[2];
        inactive = result[3]; read_index = result[4];
    }
    out[0] = status; out[1] = completion; out[2] = stage;
    out[3] = inactive; out[4] = read_index;
    return 0;
}

/* HostWindow (54): in configure(0/1), out gart_start/gart_size/reads_supported. */
int dext_compute_host_window(uint64_t configure, uint64_t *out /* 3 */)
{
    if (!out)
        return -EINVAL_L;
    if (g_compute.stage != DEXT_COMPUTE_STAGE_SDMA_INIT)
        return -ENOTREADY_L;
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    /* The real GART host-window configure is the p3-hw-gate.  Host-verifiable:
     * report not-configured (0/0/0). */
    uint64_t gart_start = 0, gart_size = 0, reads_supported = 0;
    if (g_compute_gpu_op_hook) {
        uint64_t args[1] = { configure };
        if (g_compute_gpu_op_hook(7, args, out, 24) == 0) {
            gart_start = out[0]; gart_size = out[1]; reads_supported = out[2];
            g_compute.gart_start = gart_start;
            g_compute.gart_size = gart_size;
            g_compute.gart_reads_verified =
                gart_start && gart_size && reads_supported;
        }
    }
    out[0] = gart_start; out[1] = gart_size; out[2] = reads_supported;
    return 0;
}

/* ShutdownGPU (42): out status/phase (phase 6 = complete). */
int dext_compute_shutdown(uint64_t *out /* 2 */)
{
    if (!out)
        return -EINVAL_L;
#ifdef LINUXU_DEXT
    /* A reset of the in-memory selector state cannot quiesce the GPU. */
    if (!g_compute_gpu_op_hook)
        return -ENOTREADY_L;
#endif
    uint64_t status = 0, phase = 6;
    if (g_compute_gpu_op_hook) {
        uint64_t result[2] = { 0, 6 };
        g_compute_gpu_op_hook(8, NULL, result, sizeof(result));
        status = result[0]; phase = result[1];
    }
    out[0] = status; out[1] = phase;
    /* Reset the compute state (discard session). */
    dext_compute_reset();
    return 0;
}

/* HostMemoryTest (44): in size, out 6 scalars. */
int dext_compute_host_mem_test(uint64_t size, uint64_t *out /* 6 */)
{
    if (!out || !size)
        return -EINVAL_L;
    if (!g_compute.pci_open)
        return -ENOTREADY_L;
    if (!g_compute_gpu_op_hook)
        return -ENOTREADY_L;
    uint64_t args[1] = { size };
    if (g_compute_gpu_op_hook(10, args, out, sizeof(uint64_t) * 6) != 0) {
        out[0] = (uint64_t)(-1); /* status */
        for (int i = 1; i < 6; i++) out[i] = 0;
        return 0;
    }
    if (g_compute.gart_start && g_compute.gart_size &&
        out[0] == 0 && out[1] == 8 && out[2] == 0)
        g_compute.gart_reads_verified = true;
    return 0;
}

/* WaitFence (20): in fence_handle/timeout_ns, out status (0=signaled,1=timeout). */
int dext_compute_wait_fence(uint64_t fence_handle, uint64_t timeout_ns,
                            uint64_t *out_status)
{
    if (!out_status)
        return -EINVAL_L;
    if (!fence_handle)
        return -EINVAL_L;
    /* CS-handle path: the reference treats fence_handle == cs_handle. */
    struct dext_compute_cs *cs = cs_lookup(fence_handle);
    if (cs) {
        /* SubmitIB never accepts a stream here, so nothing is pending. */
        (void)timeout_ns;
        *out_status = 0;
        return 0;
    }
    /* Non-CS fence handle: the reference falls through to the fence table. */
    if (fence_handle > DEXT_COMPUTE_MAX_FENCE)
        return -ENOENT_L;
    struct dext_compute_fence *f = &g_compute.fence[fence_handle - 1];
    if (!f->in_use)
        return -ENOENT_L;
    (void)timeout_ns;
    *out_status = f->signaled ? 0 : 1;
    return 0;
}

/* SubmitIB (19): in cs_handle, out fence_handle (== cs_handle). */
int dext_compute_submit_ib(uint64_t cs_handle, uint64_t *out_fence)
{
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    if (!out_fence)
        return -EINVAL_L;
    struct dext_compute_cs *cs = cs_lookup(cs_handle);
    if (!cs)
        return -ENOENT_L;
    if (cs->written_dw == 0)
        return -EINVAL_L;
    /* This model has no ring to execute the stream on. Report the same
     * NotReady as the DriverKit backend instead of a fence that claims the
     * GPU completed work it never received. */
    (void)out_fence;
    return -ENOTREADY_L;
}

/* CSCreate (37): in ip_type/ip_instance, out handle. */
int dext_compute_cs_create(uint32_t ip_type, uint32_t ip_instance,
                           uint64_t *out_handle)
{
    if (!compute_backend_ready())
        return -ENOTREADY_L;
    if (!out_handle)
        return -EINVAL_L;
    if (ip_type > DEXT_COMPUTE_CS_IP_COMPUTE)
        return -EINVAL_L;
    uint64_t handle;
    int r = cs_find_free(&handle);
    if (r)
        return r;
    struct dext_compute_cs *e = &g_compute.cs[handle - 1];
    memset(e, 0, sizeof(*e));
    e->in_use = true;
    e->handle = handle;
    e->ip_type = ip_type;
    e->ip_instance = ip_instance;
    *out_handle = handle;
    return 0;
}

/* CSWriteDwords (38): in handle/count/dwords, out written. */
int dext_compute_cs_write_dwords(uint64_t handle, const uint32_t *dwords,
                                 uint32_t count, uint32_t *out_written)
{
    struct dext_compute_cs *e = cs_lookup(handle);
    if (!e || !dwords || !count)
        return -EINVAL_L;
    if (count > DEXT_COMPUTE_MAX_CS_DW)
        return -EINVAL_L;
    memcpy(e->dwords, dwords, count * sizeof(uint32_t));
    e->written_dw = count;
    *out_written = count;
    return 0;
}

/* CSDestroy (39): in handle. */
int dext_compute_cs_destroy(uint64_t handle)
{
    struct dext_compute_cs *e = cs_lookup(handle);
    if (!e)
        return -ENOENT_L;
    memset(e, 0, sizeof(*e));
    return 0;
}

/* The in-memory model has no KFD process: no signal events. */
int dext_compute_event(uint32_t op, uint32_t id, uint64_t out[3])
{
    (void)op; (void)id; (void)out;
    return -ENOTREADY_L;
}

int dext_compute_event_wait_begin(const uint32_t *ids, uint32_t count, int all,
                                  uint32_t timeout_ms, struct rt_kfd_wait **out)
{
    (void)ids; (void)count; (void)all; (void)timeout_ms;
    if (out) *out = NULL;
    return -ENOTREADY_L;
}
#endif /* LINUXU_DEXT */

/* AtomicRequester (60): shared by both builds; touches no device state. */
int dext_compute_atomic_requester(uint64_t enable, uint64_t *out)
{
    if (!out || enable > 1)
        return -EINVAL_L;
    out[0] = 1;                                   /* snapshot version */
    out[1] = out[2] = out[3] = UINT64_MAX;        /* no DevCtl2 access */
    out[4] = UINT64_MAX;                          /* no saved original */
    out[5] = 0;                                   /* not active */
    out[6] = 0;                                   /* no restore pending */
    out[7] = enable ? DEXT_COMPUTE_ATOMIC_UNSUPPORTED : 0;
    return 0;
}
