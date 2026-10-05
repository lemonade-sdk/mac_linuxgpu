/* Production selectors with failing runtime boundaries; no IOKit or GPU. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <rt/compute.h>
#include <rt/recovery.h>
#include <rt/dispatch.h>
#include <rt/gart.h>
#include "../../dext/sources/session_state.h"
#include "../../dext/sources/dext_compute.h"
#include "../../dext/sources/dext_aql.h"
#include "../../dext/sources/dext_kfd.h"

struct pci_dev { unsigned unused; };
struct rt_compute_ctx { bool live; };
struct rt_compute_bo { bool live; uint64_t size; unsigned domain; };
struct rt_compute_fence { unsigned unused; };
struct dext_aql_queue { bool live, retained, mapped; };
static struct pci_dev pdev;
/* Clients that come and go in the churn scenarios: four times the
 * backend's record table (DEXT_CLIENT_SLOTS, 64). */
#define CHURN_CLIENTS 256u
static struct rt_compute_ctx context;
static struct rt_compute_bo bos[32];
static struct dext_aql_queue queue;
static struct rt_compute_fence fence;
static unsigned char cpu_memory[4096];
static unsigned opens, closes, allocations, frees, destroys, kicks, services;
static int open_error, startup_failure, close_error, status_error;
static bool device_removed;
static int create_error, destroy_error, service_error, kick_error;
static int alloc_info_error, free_error, dispatch_error, bounded_error;
static bool create_retained, service_retained, kick_poison, bounded_uncertain;
static uint64_t dispatch_sequence;
static int startup_stage;
static int topology_error, limits_error;
static unsigned geometry_engines=4, geometry_arrays=2, geometry_cus=64;
static struct rt_compute_topology topology = {
    .gfx_target_version=120001, .simd_per_cu=2, .max_waves_per_simd=16,
    .lds_bytes=65536, .scratch_slots_per_cu=32, .max_engine_clock_mhz=2920,
    .xnack=RT_TARGET_FEATURE_OFF, .sramecc=RT_TARGET_FEATURE_ON, .xcc_count=1,
    .l1_bytes=32768, .l2_bytes=8u<<20, .l3_bytes=64u<<20,
    .product_name="Test Board",
};
static struct dext_aql_limits aql_limits = {
    .max_private_bytes=262128, .min_packets=64, .max_packets=4096, .slots=3,
};
static int fail_stage(void) { return ++startup_stage == startup_failure ? -EIO : 0; }

int rt_compute_open(struct pci_dev *p, struct rt_compute_ctx **out)
{
    assert(p == &pdev); ++opens;
    if (open_error) return open_error;
    assert(!context.live); context.live = true; *out = &context; return 0;
}
int rt_compute_close(struct rt_compute_ctx *ctx)
{
    assert(ctx == &context && ctx->live); ++closes;
    if (close_error) return close_error;
    for (unsigned i=0; i<32; ++i) assert(!bos[i].live);
    /* A queue of a removed device is left behind with nothing to run it. */
    assert(!queue.live || device_removed); ctx->live = false; return 0;
}
int rt_compute_status(struct rt_compute_ctx *ctx)
{ assert(ctx == &context && ctx->live); return status_error; }
int rt_compute_verify_host_memory(struct rt_compute_ctx *ctx)
{ assert(ctx == &context); return fail_stage(); }
int rt_compute_properties(struct rt_compute_ctx *ctx, struct rt_compute_properties *p)
{
    assert(ctx == &context);
    if (fail_stage()) return -EIO;
    *p = (struct rt_compute_properties) {
        .vram_bytes=1ULL<<30, .visible_vram_bytes=1ULL<<28,
        .gart_start=0x100000000ULL, .gart_bytes=1ULL<<29,
        .timestamp_frequency_hz=100000000, .gfx_ip_version=(12U<<24)|(1U<<8),
        .pci_chip_id=0x7551, .shader_engines=geometry_engines,
        .shader_arrays_per_engine=geometry_arrays, .simd_per_cu=2,
        .cu_count=geometry_cus, .max_waves_per_simd=16, .wavefront_size=32,
        .host_memory_verified=1
    };
    return 0;
}
int rt_compute_memory_usage(struct rt_compute_ctx *ctx, struct rt_compute_memory_usage *u)
{
    assert(ctx == &context);
    if (fail_stage()) return -EIO;
    *u = (struct rt_compute_memory_usage) {.total_bytes=1ULL<<30,
        .usable_bytes=1ULL<<29, .free_bytes=1ULL<<28}; return 0;
}
int rt_gart_status(void) { return fail_stage(); }
int rt_gart_get_window(uint64_t *base, uint64_t *size)
{ *base=0x100000000ULL; *size=1ULL<<29; return fail_stage(); }
int rt_gart_set_window(uint64_t size) { (void)size; return 0; }
int rt_compute_bo_alloc(struct rt_compute_ctx *ctx, uint64_t size,
                       uint64_t align, enum rt_compute_domain domain,
                       struct rt_compute_bo **out)
{
    assert(ctx == &context && ctx->live && size && align);
    assert(allocations<32);
    struct rt_compute_bo *bo = &bos[allocations++];
    assert(!bo->live); bo->live=true; bo->size=size; bo->domain=domain;
    *out=bo; return 0;
}
int rt_compute_bo_info(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
                      struct rt_compute_bo_info *out)
{
    assert(ctx == &context && bo && bo->live);
    if (alloc_info_error) return alloc_info_error;
    *out=(struct rt_compute_bo_info) {.size=bo->size,
        .gpu_address=0x200000000ULL+(uint64_t)(bo-bos)*0x10000,
        .cpu_address=bo->domain == RT_COMPUTE_GTT ? cpu_memory : NULL,
        .domain=bo->domain, .shared_descriptor_available=1}; return 0;
}
int rt_compute_bo_free(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo)
{
    assert(ctx == &context && ctx->live && bo && bo->live); ++frees;
    if (free_error) return free_error;
    bo->live=false; return 0;
}
int rt_compute_bo_read(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
                      uint64_t offset, void *dst, size_t size)
{ assert(ctx == &context && bo->live && offset <= bo->size && size <= bo->size-offset); memset(dst,0,size); return 0; }
int rt_compute_bo_write(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
                       uint64_t offset, const void *src, size_t size)
{ assert(ctx == &context && bo->live && src && offset <= bo->size && size <= bo->size-offset); return 0; }
int rt_compute_bo_copy(struct rt_compute_ctx *ctx, struct rt_compute_bo *src,
                      struct rt_compute_bo *dst, uint64_t src_offset,
                      uint64_t dst_offset, uint32_t size, struct rt_compute_fence **out)
{ assert(ctx == &context && src->live && dst->live && size); (void)src_offset; (void)dst_offset; *out=&fence; return 0; }
int rt_compute_fence_wait(struct rt_compute_fence *f) { assert(f==&fence); return 0; }
void rt_compute_fence_put(struct rt_compute_fence *f) { assert(f==&fence || !f); }
static int code_error, code_after_sync_error, clamp_ieee;
int dext_aql_rsrc1_clamp_ieee(struct rt_compute_ctx *ctx)
{ assert(ctx==&context); return clamp_ieee; }
static unsigned syncs;
int rt_compute_cache_sync(struct rt_compute_ctx *ctx, uint32_t timeout_us, uint64_t *sequence)
{ assert(ctx==&context && timeout_us); ++syncs; *sequence=dispatch_sequence; return dispatch_error; }
int dext_aql_dispatch_code(struct rt_compute_ctx *ctx, uint64_t code, const void *request,
                           size_t request_size, int (*before_map)(void *), void *arg,
                           uint64_t out[5], int *uncertain)
{
    assert(ctx==&context && code && request && request_size==272 && before_map);
    memset(out,0,5*sizeof(*out)); *uncertain=0;
    if (code_error) return code_error; /* refused before the HQD is mapped */
    int r=before_map(arg);
    if (r) return r;
    if (code_after_sync_error) { *uncertain=1; return code_after_sync_error; }
    out[2]=5; return 0;
}
int dext_aql_available(struct rt_compute_ctx *ctx)
{ assert(ctx==&context); return fail_stage(); }
static struct amdgpu_device *const fake_adev = (struct amdgpu_device *)&context;
struct amdgpu_device *rt_compute_device(struct rt_compute_ctx *ctx)
{ assert(ctx==&context); return fake_adev; }
int rt_device_topology(struct amdgpu_device *adev, struct rt_compute_topology *out)
{ assert(adev==fake_adev); if (topology_error) return topology_error; *out=topology; return 0; }
int dext_aql_limits(struct rt_compute_ctx *ctx, struct dext_aql_limits *out)
{ assert(ctx==&context); if (limits_error) return limits_error; *out=aql_limits; return 0; }
static int spec_error;
int rt_device_spec(struct amdgpu_device *adev, struct rt_device_spec *out)
{
    assert(adev==fake_adev);
    if (spec_error) return spec_error;
    memset(out,0,sizeof(*out));
    out->present=RT_DEVICE_SPEC_GEOMETRY|RT_DEVICE_SPEC_CUS|RT_DEVICE_SPEC_BACKENDS;
    out->shader_engines=4; out->shader_arrays_per_se=2; out->cus_per_array=8;
    out->active_cus=64; out->cu_bitmap[3][1]=0xff; out->active_rbs=16;
    return 0;
}
static unsigned legacy_creates;
int dext_aql_create(struct rt_compute_ctx *ctx, struct rt_compute_bo *ring,
                    struct rt_compute_bo *meta, uint32_t packets,
                    struct dext_aql_queue **out)
{
    assert(ctx==&context && ring->live && meta->live && packets==64);
    *out=NULL;
    ++legacy_creates;
    /* One legacy HQD: a second queue finds the partition exhausted and is
     * refused before any allocation or hardware access. */
    if (queue.live) return -ENOSPC;
    if (!create_error || create_retained) {
        queue.live=true; queue.retained=create_retained;
        queue.mapped=!create_error; *out=&queue;
    }
    return create_error;
}
int dext_aql_destroy(struct dext_aql_queue *q)
{
    assert(q==&queue && q->live); ++destroys;
    if (destroy_error) { q->retained=true; return destroy_error; }
    assert(!q->retained); q->live=false; return 0;
}
int dext_aql_service(struct dext_aql_queue *q, uint64_t *inactive)
{
    assert(q==&queue && q->live && !q->retained); ++services;
    if (service_retained) q->retained=true;
    *inactive=service_error ? 0x401 : 0;
    return service_error;
}
int dext_aql_kick(struct dext_aql_queue *q, uint64_t packet)
{
    assert(q==&queue && q->live); (void)packet;
    if (q->retained || !q->mapped) return -19;
    ++kicks;
    if (kick_poison) status_error=-EBUSY;
    return kick_error;
}
/* Device resets (rt/recovery.h): the hooks the backend installs, and the
 * queue's reset calls. */
static struct rt_recovery_queue_hooks installed_hooks;
static unsigned hook_installs, hook_removals, prepares, restores;
static int restore_error;
void rt_recovery_set_queue_hooks(const struct rt_recovery_queue_hooks *hooks)
{
    if (hooks) { installed_hooks=*hooks; ++hook_installs; }
    else { memset(&installed_hooks,0,sizeof(installed_hooks)); ++hook_removals; }
}
/* dext_aql's own list: the one queue, while it lives. */
void dext_aql_reset_prepare(void)
{ ++prepares; if (queue.live) queue.mapped=false; }
int dext_aql_reset_restore(void)
{
    ++restores;
    if (!queue.live || queue.retained) return 0;
    if (restore_error) { queue.retained=true; return 1; }
    queue.mapped=true; return 0;
}
int dext_aql_uncertain(const struct dext_aql_queue *q)
{ assert(q==&queue && q->live); return q->retained || status_error==-EBUSY; }
int dext_aql_dispatch_bounded(struct rt_compute_ctx *ctx, uint64_t desc,
                              uint64_t kernarg, const void *request,
                              size_t request_size, uint64_t out[5], int *uncertain)
{
    assert(ctx==&context && desc && kernarg && request && request_size==208);
    *uncertain=bounded_uncertain; memset(out,0,5*sizeof(*out));
    return bounded_error;
}

/* ---- KFD-backed clients (dext_kfd.h) ---- */
static int kfd_supported_error = -ENODEV, kfd_open_error, kfd_create_error, kfd_destroy_error;
struct dext_kfd_client { bool live, uncertain; int pid; char comm[32];
    uint64_t window_base, window_size; unsigned queues; };
struct dext_kfd_queue { bool live; struct dext_kfd_client *c; uint64_t last; };
struct rt_kfd_bo { bool live; struct dext_kfd_client *c; uint64_t size, va; uint32_t domain; };
static struct dext_kfd_client kfd_clients[4];
static struct dext_kfd_queue kfd_queues[8];
static struct rt_kfd_bo kfd_bos[32];
static unsigned kfd_opens, kfd_closes, kfd_queue_creates, kfd_queue_destroys, kfd_kicks,
    kfd_bo_allocs, kfd_bo_frees, kfd_dispatches;
static uint64_t kfd_va_next = 0x7000000000ULL;
int dext_kfd_supported(struct rt_compute_ctx *ctx) { assert(ctx==&context); return kfd_supported_error; }
int dext_kfd_open(struct rt_compute_ctx *ctx, int pid, const char *comm, struct dext_kfd_client **out)
{
    assert(ctx==&context && !kfd_supported_error);
    *out=NULL; ++kfd_opens;
    if (kfd_open_error) return kfd_open_error;
    for (unsigned i=0;i<4;++i) if (!kfd_clients[i].live) {
        struct dext_kfd_client *c=&kfd_clients[i];
        memset(c,0,sizeof(*c)); c->live=true; c->pid=pid>0 ? pid : 9000+(int)i;
        if (comm) strncpy(c->comm,comm,sizeof(c->comm)-1);
        c->window_size=1ULL<<37; *out=c; return 0;
    }
    return -ENOMEM;
}
int dext_kfd_uncertain(const struct dext_kfd_client *c) { assert(c && c->live); return c->uncertain; }
/* As dext_kfd.mm: the close destroys the client's queue records (retrying
 * one whose removal failed), then the session; a removal that still fails
 * keeps the client. */
int dext_kfd_close(struct dext_kfd_client *c)
{
    assert(c && c->live);
    for (unsigned i=0;i<8;++i) if (kfd_queues[i].live && kfd_queues[i].c==c) {
        ++kfd_queue_destroys;
        if (kfd_destroy_error) { c->uncertain=true; return -EBUSY; }
        kfd_queues[i].live=false; --c->queues;
    }
    c->uncertain=false;
    ++kfd_closes;
    for (unsigned i=0;i<32;++i) if (kfd_bos[i].live && kfd_bos[i].c==c) kfd_bos[i].live=false;
    c->live=false; return 0;
}
int dext_kfd_settle(struct dext_kfd_client *c, unsigned int wait_ms)
{
    (void)wait_ms;
    assert(c && c->live);
    for (unsigned i=0;i<8;++i) if (kfd_queues[i].live && kfd_queues[i].c==c && kfd_destroy_error)
        return -EBUSY;
    c->uncertain=false;
    return 0;
}
int dext_kfd_info(struct dext_kfd_client *c, struct dext_kfd_info *out)
{
    assert(c && c->live);
    *out=(struct dext_kfd_info){.pid=c->pid,.slots=127,.window_base=c->window_base,
        .window_size=c->window_size,.gpuvm_base=1ULL<<16,.gpuvm_limit=(1ULL<<47)-1};
    return 0;
}
int dext_kfd_set_window(struct dext_kfd_client *c, uint64_t base, uint64_t size)
{
    assert(c && c->live);
    if (!size) size=c->window_size;
    if (c->window_base) return c->window_base==base && c->window_size==size ? 0 : -EBUSY;
    if (!base || (base&(size-1)) || size>c->window_size) return -EINVAL;
    c->window_base=base; c->window_size=size; return 0;
}
/* KFD signal events (selectors 86/87): ids from 1, a mailbox per id. */
static uint32_t kfd_events_next = 1, kfd_events_live;
int dext_kfd_event_create(struct dext_kfd_client *c, uint32_t *id, uint32_t *trigger,
                          uint64_t *mailbox_va)
{
    assert(c && c->live);
    *id = *trigger = kfd_events_next++;
    *mailbox_va = 0x7f0000000000ULL + (uint64_t)*id * 8;
    kfd_events_live++;
    return 0;
}
int dext_kfd_event_destroy(struct dext_kfd_client *c, uint32_t id)
{
    assert(c && c->live);
    if (!id || id >= kfd_events_next || !kfd_events_live) return -ENOENT;
    kfd_events_live--;
    return 0;
}
int dext_kfd_event_set(struct dext_kfd_client *c, uint32_t id)
{
    assert(c && c->live);
    return id && id < kfd_events_next ? 0 : -ENOENT;
}
int dext_kfd_wait_begin(struct dext_kfd_client *c, const uint32_t *ids, uint32_t count,
                        int all, uint32_t timeout_ms, struct rt_kfd_wait **out)
{
    assert(c && c->live && out);
    (void)all; (void)timeout_ms;
    *out = NULL;
    for (uint32_t i = 0; i < count; i++)
        if (!ids[i] || ids[i] >= kfd_events_next) return -ENOENT;
    return -EBUSY;	/* the test never runs a wait */
}
int dext_kfd_queue_abi(struct dext_aql_limits *out)
{ *out=(struct dext_aql_limits){.max_private_bytes=262128,.min_packets=64,.max_packets=4096}; return 0; }
int dext_kfd_bo_alloc(struct dext_kfd_client *c, uint64_t size, uint64_t alignment,
                      uint32_t domain, struct rt_kfd_bo **out, uint64_t *va)
{
    assert(c && c->live && size && alignment && (domain==2 || domain==3));
    /* Shared buffers live in the client's window. */
    if (domain==2 && !c->window_base) return -EINVAL;
    for (unsigned i=0;i<32;++i) if (!kfd_bos[i].live) {
        struct rt_kfd_bo *bo=&kfd_bos[i];
        bo->live=true; bo->c=c; bo->size=size; bo->domain=domain;
        bo->va=domain==2 ? c->window_base+(uint64_t)i*0x10000 : (kfd_va_next+=0x100000);
        *out=bo; *va=bo->va; ++kfd_bo_allocs; return 0;
    }
    return -ENOMEM;
}
int dext_kfd_bo_free(struct dext_kfd_client *c, struct rt_kfd_bo *bo)
{ assert(c && bo && bo->live && bo->c==c); bo->live=false; ++kfd_bo_frees; return 0; }
int dext_kfd_bo_read(struct dext_kfd_client *c, struct rt_kfd_bo *bo, uint64_t offset,
                     void *dst, size_t bytes)
{ assert(c && bo->live && offset<=bo->size && bytes<=bo->size-offset); memset(dst,0,bytes); return 0; }
int dext_kfd_bo_write(struct dext_kfd_client *c, struct rt_kfd_bo *bo, uint64_t offset,
                      const void *src, size_t bytes)
{ assert(c && bo->live && src && offset<=bo->size && bytes<=bo->size-offset); return 0; }
int dext_kfd_bo_copy(struct dext_kfd_client *c, struct rt_kfd_bo *src, uint64_t so,
                     struct rt_kfd_bo *dst, uint64_t dof, uint64_t bytes)
{ assert(c && src->live && dst->live && bytes); (void)so; (void)dof; return 0; }
static unsigned char kfd_pages[2][16384];
int dext_kfd_bo_ranges(struct dext_kfd_client *c, struct rt_kfd_bo *bo,
                       int (*fn)(void *, void *, uint64_t), void *arg)
{
    assert(c && bo->live && bo->domain==2);
    int r=fn(arg,kfd_pages[0],8192);
    return r ? r : fn(arg,kfd_pages[1],8192);
}
int dext_kfd_queue_create(struct dext_kfd_client *c, struct rt_kfd_bo *ring,
                          struct rt_kfd_bo *metadata, uint32_t packets, struct dext_kfd_queue **out)
{
    assert(c && c->live && ring->live && metadata->live && ring->c==c && metadata->c==c);
    assert(ring->domain==2 && metadata->domain==2 && packets>=64);
    *out=NULL; ++kfd_queue_creates;
    if (kfd_create_error) return kfd_create_error;
    for (unsigned i=0;i<8;++i) if (!kfd_queues[i].live) {
        kfd_queues[i]=(struct dext_kfd_queue){.live=true,.c=c}; ++c->queues;
        *out=&kfd_queues[i]; return 0;
    }
    return -ENOSPC;
}
/* A client whose GPU work faulted: KFD evicted its queues (dext_kfd.mm
 * answers -EFAULT for them from then on). */
static struct dext_kfd_client *kfd_faulted;
static int kfd_code_after_sync_error;
static uint64_t kfd_stopped_code;	/* the CP stopped queues with this error code */
int dext_kfd_queue_kick(struct dext_kfd_queue *q, uint64_t packet)
{ assert(q && q->live); if (q->c==kfd_faulted) return -EFAULT; q->last=packet; ++kfd_kicks; return 0; }
/* The delivery thread's form: never a queue that went (the kick table
 * retired it first). */
static unsigned kfd_direct_kicks;
int dext_kfd_queue_kick_direct(struct dext_kfd_queue *q, uint64_t packet)
{ assert(q && q->live); if (q->c==kfd_faulted) return -EFAULT; q->last=packet; ++kfd_direct_kicks; return 0; }
int dext_kfd_queue_service(struct dext_kfd_queue *q, uint64_t *inactive)
{
    assert(q && q->live); *inactive=0;
    if (q->c==kfd_faulted) return -EFAULT;
    if (kfd_stopped_code) { *inactive=kfd_stopped_code; return -ENOEXEC; }
    return 0;
}
int dext_kfd_fault(struct dext_kfd_client *c, uint32_t *flags, uint64_t *va)
{
    assert(c && c->live && flags && va);
    *flags=0; *va=0;
    if (c!=kfd_faulted) return 0;
    *flags=DEXT_KFD_FAULT_VALID|DEXT_KFD_FAULT_NOT_PRESENT; *va=0x1235d4000ULL; return 1;
}
int dext_kfd_queue_destroy(struct dext_kfd_queue *q)
{
    assert(q && q->live); ++kfd_queue_destroys;
    if (kfd_destroy_error) { q->c->uncertain=true; return kfd_destroy_error; }
    q->live=false; --q->c->queues; return 0;
}
unsigned int dext_kfd_queue_count(struct dext_kfd_client *c) { return c->queues; }
int dext_kfd_dispatch_bounded(struct dext_kfd_client *c, uint64_t desc, uint64_t kernarg,
                              const void *request, size_t request_size, uint64_t out[5], int *uncertain)
{
    assert(c && c->live && desc && kernarg && request && request_size==208);
    *uncertain=0; memset(out,0,5*sizeof(*out)); out[2]=5; out[4]=1; ++kfd_dispatches; return 0;
}
int dext_kfd_dispatch_code(struct dext_kfd_client *c, uint64_t code, const void *request,
                           size_t request_size, int (*before_map)(void *), void *arg,
                           uint64_t out[5], int *uncertain)
{
    assert(c && c->live && code && request && request_size==272 && before_map);
    memset(out,0,5*sizeof(*out)); *uncertain=0;
    int r=before_map(arg);
    if (r) return r;
    /* The launch timed out after its cache sync was submitted; its queue
     * was destroyed through KFD, so nothing is uncertain. */
    if (kfd_code_after_sync_error) return kfd_code_after_sync_error;
    out[2]=5; ++kfd_dispatches; return 0;
}
static int count_range(void *arg, void *cpu, uint64_t bytes)
{ assert(cpu && bytes); *(uint64_t *)arg+=bytes; return 0; }

static uint64_t alloc_bo(unsigned domain)
{
    uint64_t handle=0, gpu=0, cpu=UINT64_MAX;
    assert(dext_compute_bo_alloc(4096,domain,4096,0,&handle,&gpu,&cpu)==0);
    assert(handle && gpu && !cpu); return handle;
}
static uint64_t create_queue(uint64_t *ring, uint64_t *meta)
{
    uint64_t status=UINT64_MAX, handle=0;
    *ring=alloc_bo(DEXT_COMPUTE_BO_DOMAIN_GTT);
    *meta=alloc_bo(DEXT_COMPUTE_BO_DOMAIN_GTT);
    assert(dext_compute_aql_queue_create(*ring,*meta,64,&status,&handle)==0);
    assert(!status && handle); return handle;
}
/* A client asks for its compute session (tag 12) as HSA does, gets a KFD
 * process and sets its own host window. */
static void kfd_session(uint64_t id)
{
    uint64_t words[8], window[3];
    dext_compute_select_client(id);
    assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
    assert(!dext_compute_host_window(1ULL<<37,window) && window[0]==1ULL<<37);
}
static void expect_frozen(uint64_t payload)
{
    unsigned old_frees=frees, old_destroys=destroys, old_closes=closes;
    uint64_t out[10];
    assert(dext_compute_bo_free(payload)==-EBUSY_L);
    assert(dext_compute_bo_alloc(4096,1,4096,0,out,NULL,NULL)==-ENOTREADY_L);
    assert(dext_compute_query_info(1,out,10)==-ENOTREADY_L);
    assert(dext_compute_stop()==-EBUSY_L);
    assert(dext_compute_start(&pdev)==-EBUSY_L);
    assert(frees==old_frees && destroys==old_destroys && closes==old_closes);
}
static void startup_case(int stage, bool cannot_close)
{
    startup_failure=stage; close_error=cannot_close ? -EBUSY : 0;
    assert(dext_compute_start(&pdev)==(cannot_close ? -EBUSY_L : -ENOTREADY_L));
    assert(opens==1 && closes==1 && context.live==cannot_close);
    if (cannot_close) {
        assert(dext_compute_stop()==-EBUSY_L);
        assert(dext_compute_start(&pdev)==-EBUSY_L);
        assert(closes==1 && opens==1);
    } else {
        startup_failure=0;
        assert(dext_compute_start(&pdev)==0);
        assert(dext_compute_stop()==0 && closes==2);
    }
}
int main(int argc, char **argv)
{
    assert(argc>=2);
    if (!strcmp(argv[1],"startup")) {
        assert(argc==4); startup_case(atoi(argv[2]),atoi(argv[3])); return 0;
    }
    if (!strcmp(argv[1],"kfd-without-legacy-hqd")) {
        /* No legacy HQD left (stage 7 is the queue partition) but KFD
         * sessions available: the device starts, KFD clients get queues and
         * legacy clients get none. */
        uint64_t words[8], window[3], ring, meta, q, status;
        startup_failure=7; kfd_supported_error=0;
        assert(dext_compute_start(&pdev)==0);
        /* The serving build is the compiled one (RuntimeBuild's out[3]). */
        uint64_t compiled[4] = {0};
        assert(dext_compute_runtime_build_cached(compiled)==0);
        assert(dext_compute_runtime_build(words)==0 && words[2]==compiled[3] && compiled[3]>=258);
        dext_compute_set_kfd_policy(false);
        dext_compute_select_client(3);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==1 && !words[2]);
        {
            /* A legacy client has no KFD process: no signal events. */
            uint64_t event[3];
            assert(dext_compute_event(DEXT_COMPUTE_EVENT_CREATE,0,event)==-ENOTREADY_L);
        }
        {
            uint64_t topology[16];
            assert(dext_compute_query_info(10,topology,16)==16 && !topology[7]);
        }
        ring=alloc_bo(2); meta=alloc_bo(2);
        assert(dext_compute_aql_queue_create(ring,meta,64,&status,&q)==-ENOMEM_L);
        dext_compute_set_kfd_policy(true);
        dext_compute_select_client(4);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
        {
            /* A KFD client's signal events. */
            uint64_t event[3];
            uint32_t id;
            struct rt_kfd_wait *wait = (struct rt_kfd_wait *)1;
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_CREATE,0,event) && event[0] &&
                   event[1]==event[0] && event[2]);
            id = (uint32_t)event[0];
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_SET,id,event));
            assert(dext_compute_event_wait_begin(&id,1,0,10,&wait)==-EBUSY_L && !wait);
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,id,event));
            assert(dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,4000,event)==-ENOENT_L);
            assert(dext_compute_event(7,id,event)==-EINVAL_L);
        }
        assert(!dext_compute_host_window(1ULL<<37,window));
        ring=alloc_bo(2); meta=alloc_bo(2);
        assert(!dext_compute_aql_queue_create(ring,meta,64,&status,&q));
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0 && kfd_closes==1);
        /* The policy off and no legacy HQD: the device does not start. */
        dext_compute_set_kfd_policy(false);
        startup_stage=0;
        assert(dext_compute_start(&pdev)==-ENOTREADY_L);
        puts(argv[1]); return 0;
    }
    if (!strcmp(argv[1],"open-failure")) {
        open_error=-ENOMEM;
        assert(dext_compute_start(&pdev)==-ENOTREADY_L);
        assert(!context.live && !closes);
        assert(dext_compute_stop()==0); return 0;
    }
    assert(dext_compute_start(&pdev)==0);
    uint64_t payload=alloc_bo(DEXT_COMPUTE_BO_DOMAIN_VRAM), out[10];
    struct rt_dispatch_request raw={.version=2,.code_handle=payload,.code_bytes=64,
        .timeout_us=100};
    if (!strcmp(argv[1],"dispatch-timeout")) {
        /* The cache sync was submitted and did not complete. */
        dispatch_error=-ETIMEDOUT; dispatch_sequence=7;
        assert(dext_compute_dispatch(&raw,sizeof(raw),out)==-EBUSY_L);
        expect_frozen(payload);
    } else if (!strcmp(argv[1],"dispatch")) {
        /* Cache sync on the kernel ring, then the AQL launch; the sync's
         * fence is the launch fence. Version 1 requests are zero-extended. */
        dispatch_sequence=41;
        assert(dext_compute_rsrc1_clamp_ieee()==0);
        clamp_ieee=1;
        assert(dext_compute_rsrc1_clamp_ieee()==1);
        assert(dext_compute_dispatch(&raw,sizeof(raw),out)==0);
        assert(out[0]==0 && out[1]==41 && out[2]==3 && syncs==1);
        raw.version=1;
        assert(dext_compute_dispatch(&raw,264,out)==0 && syncs==2);
        raw.version=2;
        /* Every HQD held, or a launch AQL cannot express: refused before
         * any hardware access, no sync, the session stays healthy. */
        code_error=-ENOSPC;
        assert(dext_compute_dispatch(&raw,sizeof(raw),out)==-ENOMEM_L && syncs==2);
        code_error=-EINVAL;
        assert(dext_compute_dispatch(&raw,sizeof(raw),out)==-EINVAL_L && syncs==2);
        code_error=0; dispatch_error=-ENODEV; dispatch_sequence=0;
        assert(dext_compute_dispatch(&raw,sizeof(raw),out)==-ENOTREADY_L && syncs==3);
        dispatch_error=0; dispatch_sequence=42;
        assert(dext_compute_query_info(1,out,10)==3);
        /* A failure once the queue is mapped freezes the session. */
        code_after_sync_error=-ETIMEDOUT;
        assert(dext_compute_dispatch(&raw,sizeof(raw),out)==-EBUSY_L);
        expect_frozen(payload);
    } else if (!strcmp(argv[1],"code-sync")) {
        /* CodeSync: the cache sync alone; no queue, no launch. A ring that
         * does not finish is an error for the caller, never a freeze. */
        dispatch_sequence=5;
        assert(dext_compute_code_sync(100000)==0 && syncs==1 && kfd_dispatches==0);
        assert(dext_compute_code_sync(0)==-EINVAL_L && dext_compute_code_sync(1000001)==-EINVAL_L);
        assert(syncs==1);
        dispatch_error=-ETIMEDOUT;
        assert(dext_compute_code_sync(100)==-EBUSY_L && syncs==2);
        dispatch_error=0;
        assert(dext_compute_query_info(1,out,10)==3);	/* not frozen */
        assert(!dext_compute_bo_free(payload));
    } else if (!strcmp(argv[1],"query-topology")) {
        uint64_t words[16];
        /* Tag 6 word 8: waves per CU from the reported SIMD count. */
        assert(dext_compute_query_info(6,out,10)==10 && out[8]==2*16);
        assert(dext_compute_query_info(10,words,15)==-EINVAL_L);
        assert(dext_compute_query_info(10,words,16)==16);
        assert(words[0]==1 && words[1]==120001 && words[2]==2 && words[3]==16 &&
               words[4]==65536 && words[5]==262128 && words[6]==32 && words[7]==3 &&
               words[8]==(64|(4096ull<<32)) && words[9]==32768 &&
               words[10]==(8u<<20) && words[11]==(64u<<20) && words[12]==2920 &&
               words[13]==(2|(3<<2)) && words[14]==1 && words[15]==0);
        assert(dext_compute_query_info(11,words,7)==-EINVAL_L);
        memset(words,0xff,sizeof(words));
        assert(dext_compute_query_info(11,words,8)==8);
        assert(!memcmp(words,"Test Board",11));
        for (unsigned i=11;i<64;++i) assert(!((const char *)words)[i]);
        topology_error=-ENODEV;
        assert(dext_compute_query_info(10,words,16)==-ENOTREADY_L);
        assert(dext_compute_query_info(11,words,8)==-ENOTREADY_L);
        topology_error=0; limits_error=-ENODEV;
        assert(dext_compute_query_info(10,words,16)==-ENOTREADY_L);
        limits_error=0;
        /* Ring sizes come from the AQL limits. */
        uint64_t ring=alloc_bo(2), meta=alloc_bo(2);
        assert(dext_compute_aql_queue_create(ring,meta,32,out,out+1)==-EINVAL_L);
        assert(dext_compute_aql_queue_create(ring,meta,8192,out,out+1)==-EINVAL_L);
        assert(dext_compute_aql_queue_create(ring,meta,96,out,out+1)==-EINVAL_L);
        assert(dext_compute_stop()==0);
    } else if (!strcmp(argv[1],"geometry")) {
        /* Structural limits: upstream's cu_info.bitmap holds 16 engine/array
         * words of 32 CU bits. 6 engines x 2 arrays and 200 CUs fit (more
         * than the former 128-CU literal); 9 x 2 words do not. */
        assert(dext_compute_stop()==0);
        geometry_engines=6; geometry_cus=200;
        assert(dext_compute_start(&pdev)==0 && dext_compute_stop()==0);
        geometry_engines=9;
        assert(dext_compute_start(&pdev)==-ENOTREADY_L);
        geometry_engines=4; geometry_cus=4*4*32+1;
        assert(dext_compute_start(&pdev)==-ENOTREADY_L);
    } else if (!strcmp(argv[1],"bounded-nospc")) {
        struct { uint32_t version,flags; uint64_t code_handle,descriptor_offset;
            uint64_t kernarg_handle,kernarg_offset,kernarg_bytes;
            uint32_t groups[3],threads[3],timeout_us,reserved; uint64_t buffers[16];
        } request={.version=1,.code_handle=payload,.kernarg_handle=payload,
            .kernarg_bytes=16,.groups={1,1,1},.threads={1,1,1},.timeout_us=100};
        /* Every HQD held: kIOReturnNoResources, session stays healthy. */
        bounded_error=-ENOSPC;
        assert(dext_compute_aql_dispatch(&request,sizeof(request),out)==-ENOMEM_L);
        assert(dext_compute_query_info(1,out,10)==3);
        bounded_error=0;
        assert(dext_compute_aql_dispatch(&request,sizeof(request),out)==0);
        assert(dext_compute_stop()==0 && frees==1 && closes==1);
    } else if (!strcmp(argv[1],"client-churn-close")) {
        /* The device stays up across clients, and the last client's leaving
         * still closes it at the real close points (a legacy client, a raw
         * BAR mapping, ...): the session's close stops compute, and the next
         * client starts it again. Far more clients than the backend has
         * records come and go; each still gets its KFD process. */
        kfd_supported_error=0;
        for (uint64_t id=100; id<100+CHURN_CLIENTS; ++id) {
            uint64_t ring, meta;
            assert(!dext_compute_client_identity(id,4000+(int)id,"lse"));
            kfd_session(id);
            (void)create_queue(&ring,&meta);
            dext_compute_select_client(0);
            assert(dext_compute_stop()==0 && !kfd_clients[0].live);
            assert(dext_compute_start(&pdev)==0);
        }
        assert(kfd_opens==CHURN_CLIENTS && kfd_closes==kfd_opens);
        assert(dext_compute_stop()==0);
    } else if (!strcmp(argv[1],"client-churn")) {
        /* Every kind of client, far more than the backend has records,
         * joins and leaves the running device as the dext ends each: a
         * session (HSA) client that frees its queue and one killed with it
         * live (both released), a Linux-file client whose QueryInfo opened
         * a KFD process (released: it owns one), a session client that
         * never joined, and observers (display agent, mtopg), which never
         * get a record. Every stopped client's record is forgotten. */
        kfd_supported_error=0;
        const unsigned rounds=CHURN_CLIENTS;
        uint64_t id=1000, words[16];
        for (unsigned i=0; i<rounds; ++i) {
            uint64_t ring, meta, q, status;
            /* HSA, freeing its queue first. */
            assert(!dext_compute_client_identity(++id,7000,"llama-bench"));
            kfd_session(id);
            q=create_queue(&ring,&meta);
            assert(!dext_compute_aql_queue_destroy(q,&status));
            dext_compute_select_client(0);
            assert(dext_compute_client_owns(id) && !dext_compute_release_client(id));
            assert(!dext_compute_forget_client(id));
            /* HSA, killed with its queue live. */
            assert(!dext_compute_client_identity(++id,7001,"lse"));
            kfd_session(id);
            (void)create_queue(&ring,&meta);
            dext_compute_select_client(0);
            assert(!dext_compute_release_client(id) && !dext_compute_client_owns(id));
            assert(!dext_compute_forget_client(id));
            /* Linux-file: its topology query opened a KFD process. */
            dext_compute_select_client(++id);
            assert(dext_compute_query_info(10,words,16)==16 && words[7]==127);
            dext_compute_select_client(0);
            assert(dext_compute_client_owns(id) && !dext_compute_release_client(id));
            assert(!dext_compute_forget_client(id));
            /* A session client that never joined: an identity only. */
            assert(!dext_compute_client_identity(++id,7002,"MacLinuxGPUHost"));
            assert(!dext_compute_client_owns(id) && !dext_compute_forget_client(id));
            /* Observers: no record to forget. */
            assert(!dext_compute_forget_client(++id));
            assert(!dext_compute_client_records());
        }
        assert(kfd_opens==3*rounds && kfd_closes==kfd_opens && !kfd_clients[0].live);
        /* A record whose KFD process cannot be closed stays, counted, until
         * a close succeeds (the session's stop retries it). */
        assert(!dext_compute_client_identity(++id,7003,"lse"));
        kfd_session(id);
        {
            uint64_t ring, meta;
            (void)create_queue(&ring,&meta);
        }
        dext_compute_select_client(0);
        kfd_destroy_error=-ETIMEDOUT;
        assert(dext_compute_release_client(id)==-EBUSY_L);
        assert(dext_compute_forget_client(id)==-EBUSY_L && dext_compute_client_records()==1);
        kfd_destroy_error=0;
        assert(dext_compute_stop()==0 && !dext_compute_client_records());
        /* And the device still serves the next client. */
        assert(dext_compute_start(&pdev)==0);
        assert(!dext_compute_client_identity(++id,7004,"lse"));
        kfd_session(id);
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0 && !dext_compute_client_records());
    } else if (!strcmp(argv[1],"host-window-no-session")) {
        /* A client that only brings the GPU up asks for the host window: it
         * gets the GART window and stays undecided; no KFD process opens
         * until it asks for its compute session. */
        uint64_t window[3], words[8];
        kfd_supported_error=0;
        assert(!dext_compute_client_identity(21,7100,"MacLinuxGPUHost"));
        dext_compute_select_client(21);
        assert(!dext_compute_host_window(0,window) && window[0]==0x100000000ULL &&
               window[1]==1ULL<<29 && !kfd_opens);
        assert(!dext_compute_client_owns(21) && !dext_compute_client_legacy(21));
        /* A client that does ask (HSA: tag 12, then the window). */
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2 && kfd_opens==1);
        assert(!dext_compute_host_window(0,window) && window[1]==1ULL<<37);
        dext_compute_select_client(0);
        assert(!dext_compute_release_client(21) && !dext_compute_forget_client(21));
        assert(dext_compute_stop()==0);
    } else if (!strcmp(argv[1],"client-records-full")) {
        /* No record for a client (every slot held) is said: its identity
         * and its compute session fail with "no memory". */
        uint64_t words[8];
        kfd_supported_error=0;
        uint64_t id=1;
        while (id<CHURN_CLIENTS && !dext_compute_client_identity(id,1,"held")) ++id;
        assert(id<CHURN_CLIENTS && dext_compute_client_records()==id-1);
        dext_compute_select_client(id);
        assert(dext_compute_query_info(12,words,8)==-ENOMEM_L && !kfd_opens);
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0 && !dext_compute_client_records());
    } else if (!strcmp(argv[1],"device-spec")) {
        /* QueryInfo tag 8: the structure, cut to the caller's room above
         * its header, which states the bytes filled. */
        struct mlg_device_spec spec;
        memset(&spec,0xa5,sizeof(spec));
        assert(dext_compute_device_spec(&spec,sizeof(spec))==(int)sizeof(spec));
        assert(spec.version==MLG_DEVICE_SPEC_VERSION && spec.size==sizeof(spec) && !spec.reserved);
        assert(spec.present==(MLG_DEVICE_SPEC_GEOMETRY|MLG_DEVICE_SPEC_CUS|MLG_DEVICE_SPEC_BACKENDS));
        assert(spec.shader_engines==4 && spec.cus_per_array==8 && spec.active_cus==64 &&
               spec.cu_bitmap[3][1]==0xff && spec.active_rbs==16 && !spec.cc_sa_disable);
        assert(dext_compute_device_spec(&spec,32)==32 && spec.size==32);
        assert(dext_compute_device_spec(&spec,8)==-EINVAL_L);
        spec_error=-ENODEV;
        assert(dext_compute_device_spec(&spec,sizeof(spec))==-ENOTREADY_L);
    } else if (!strcmp(argv[1],"reset-hooks") || !strcmp(argv[1],"reset-hooks-fail")) {
        /* The session's queues around a device reset: the hooks exist
         * while the session does; each queue is unmapped before and
         * mapped again after; one that cannot be freezes the session. */
        uint64_t ring, meta, status=UINT64_MAX;
        assert(hook_installs==1 && installed_hooks.before_reset && installed_hooks.after_reset);
        const uint64_t handle=create_queue(&ring,&meta);
        assert(queue.mapped);
        restore_error=!strcmp(argv[1],"reset-hooks-fail") ? -ETIMEDOUT : 0;
        installed_hooks.before_reset(installed_hooks.arg);
        assert(prepares==1 && !queue.mapped);
        /* Between the two, the queue is not mapped: a kick is refused
         * and the session is not frozen for it. */
        assert(dext_compute_aql_queue_kick(handle,1,&status)==-ENOTREADY_L);
        assert(dext_compute_query_info(1,(uint64_t[10]){0},10)==3);
        installed_hooks.after_reset(installed_hooks.arg,true);
        assert(restores==1);
        if (restore_error) {
            /* The hook leaves the queue retained; its owner's next call
             * freezes the session, on the owner's queue. */
            assert(dext_compute_aql_queue_kick(handle,1,&status)==-EBUSY_L);
            expect_frozen(payload);
        } else {
            assert(queue.mapped);
            assert(dext_compute_aql_queue_kick(handle,1,&status)==0 && !status);
            assert(dext_compute_stop()==0 && hook_removals==1 && !installed_hooks.before_reset);
        }
    } else if (!strcmp(argv[1],"create-nospc")) {
        uint64_t ring=alloc_bo(2), meta=alloc_bo(2);
        create_error=-ENOSPC;
        assert(dext_compute_aql_queue_create(ring,meta,64,out,out+1)==-ENOMEM_L);
        assert(dext_compute_query_info(1,out,10)==3);
        create_error=0;
        assert(dext_compute_aql_queue_create(ring,meta,64,out,out+1)==0);
        assert(dext_compute_stop()==0 && destroys==1 && frees==3 && closes==1);
    } else if (!strcmp(argv[1],"bounded-timeout") || !strcmp(argv[1],"bounded-oom")) {
        struct { uint32_t version,flags; uint64_t code_handle,descriptor_offset;
            uint64_t kernarg_handle,kernarg_offset,kernarg_bytes;
            uint32_t groups[3],threads[3],timeout_us,reserved; uint64_t buffers[16];
        } request={.version=1,.code_handle=payload,.kernarg_handle=payload,
            .kernarg_bytes=16,.groups={1,1,1},.threads={1,1,1},.timeout_us=100};
        bounded_uncertain=!strcmp(argv[1],"bounded-timeout");
        bounded_error=bounded_uncertain ? -ETIMEDOUT : -ENOMEM;
        /* Only slot exhaustion is reported as "no resources". */
        assert(dext_compute_aql_dispatch(&request,sizeof(request),out)==
               (bounded_uncertain ? -EBUSY_L : -ENOTREADY_L));
        if (bounded_uncertain) expect_frozen(payload);
        else assert(dext_compute_stop()==0 && frees==1 && closes==1);
    } else if (!strcmp(argv[1],"legacy-one-hqd")) {
        /* Without KFD sessions a client's queues are legacy HQDs: with one
         * left free by the partition, a second queue (HRX's default two
         * per GPU) is refused with "no resources" (build 226). */
        uint64_t r0, m0, r1, m1, words[16];
        aql_limits.slots=1;
        dext_compute_select_client(7);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==1 && words[2]==1);
        assert(dext_compute_query_info(10,words,16)==16 && words[7]==1);
        create_queue(&r0,&m0);
        r1=alloc_bo(2); m1=alloc_bo(2);
        assert(dext_compute_aql_queue_create(r1,m1,64,out,out+1)==-ENOMEM_L);
        assert(legacy_creates==2 && !kfd_opens);
        assert(!dext_compute_release_client(7));
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0 && closes==1);
    } else if (!strcmp(argv[1],"kfd-two-queues")) {
        /* The same device, one legacy HQD, but KFD has a device node scheduled
         * by MES: the client becomes a KFD process and selector 56 succeeds
         * for both of HRX's queues, without a legacy HQD. */
        uint64_t words[16], window[3], r0, m0, r1, m1, q0, q1, status=1;
        aql_limits.slots=1;
        kfd_supported_error=0;
        assert(!dext_compute_client_identity(7,4242,"hrx-client"));
        dext_compute_select_client(7);
        assert(dext_compute_query_info(12,words,7)==-EINVAL_L);
        assert(dext_compute_query_info(12,words,8)==8);
        assert(words[0]==1 && words[1]==2 && words[2]==127 && words[3]==4242);
        assert(!words[4] && words[5]==1ULL<<37 && kfd_opens==1);
        assert(!strcmp(kfd_clients[0].comm,"hrx-client"));
        /* Tag 10 word 7 is KFD's per-process queue limit. */
        assert(dext_compute_query_info(10,words,16)==16 && words[7]==127);
        assert(words[8]==(64|(4096ull<<32)));
        /* The client's own window: query, then set it once. */
        assert(!dext_compute_host_window(0,window) && !window[0] &&
               window[1]==1ULL<<37 && window[2]==1);
        assert(!dext_compute_host_window(3ULL<<37,window) && window[0]==3ULL<<37);
        assert(dext_compute_host_window(4ULL<<37,window)==-EBUSY_L);
        r0=alloc_bo(2); m0=alloc_bo(2); r1=alloc_bo(2); m1=alloc_bo(2);
        uint64_t code=alloc_bo(3), info[5], map[2];
        assert(!dext_compute_bo_get_info(r0,info) && info[0]>=3ULL<<37 &&
               info[0]<4ULL<<37 && (info[4]>>8)==1);
        assert(!dext_compute_bo_get_info(code,info) && info[0]>=0x7000000000ULL);
        /* Shared buffers map their page runs, not one allocation. */
        assert(!dext_compute_bo_map(r0,map) && map[1]==4096);
        void *cpu=(void *)1; uint64_t bytes=0, mapped=0;
        assert(dext_compute_bo_memory((uint32_t)map[0],&cpu,&bytes)==-EAGAIN_L &&
               !cpu && bytes==4096);
        assert(!dext_compute_bo_memory_ranges((uint32_t)map[0],count_range,&mapped) &&
               mapped==16384);
        assert(dext_compute_bo_map(code,map)==-ENOTREADY_L);
        assert(!dext_compute_aql_queue_create(r0,m0,64,&status,&q0) && !status && q0);
        assert(!dext_compute_aql_queue_create(r1,m1,64,&status,&q1) && !status && q1);
        assert(q0!=q1 && kfd_queue_creates==2 && !legacy_creates);
        assert(dext_compute_aql_queue_create(r0,m1,64,&status,out)==-EBUSY_L);
        assert(!dext_compute_aql_queue_kick(q0,0,&status) && !dext_compute_aql_queue_kick(q1,0,&status));
        assert(kfd_kicks==2);
        /* The delivery thread's form (kick_table.h) rings the client's own
         * published queues, and only those. */
        assert(!dext_compute_aql_queue_kick_direct(7,q0,3) && kfd_direct_kicks==1);
        assert(dext_compute_aql_queue_kick_direct(8,q0,3)==-EAGAIN_L && kfd_direct_kicks==1);
        assert(dext_compute_aql_queue_kick_direct(7,0xdead,3)==-EAGAIN_L && kfd_direct_kicks==1);
        /* None while the device is not taking work (a power transition). */
        dext_compute_kick_gate(false);
        assert(dext_compute_aql_queue_kick_direct(7,q0,4)==-EAGAIN_L && kfd_direct_kicks==1);
        dext_compute_kick_gate(true);
        assert(!dext_compute_aql_queue_service(q1,&status,out) && !out[0]);
        assert(dext_compute_bo_free(r0)==-EBUSY_L);
        /* Copies and bounded launches stay inside the process. */
        assert(!dext_compute_bo_copy(r0,code,0,0,64));
        /* Another client's BO is not this client's handle. */
        assert(dext_compute_bo_copy(r0,payload,0,0,64)==-EINVAL_L);
        dispatch_sequence=41;
        assert(!dext_compute_dispatch(&(struct rt_dispatch_request){.version=2,
            .code_handle=code,.code_bytes=64,.timeout_us=100},272,out));
        assert(kfd_dispatches==1 && syncs==1);
        /* No cross-process sharing of a KFD BO yet. */
        assert(dext_compute_bo_export(code,1,2,info)==-ENOTREADY_L);
        assert(!dext_compute_aql_queue_destroy(q0,&status) && kfd_queue_destroys==1);
        /* Retired before its destroy: never rung again (the stub asserts). */
        assert(dext_compute_aql_queue_kick_direct(7,q0,5)==-EAGAIN_L);
        assert(!dext_compute_aql_queue_kick_direct(7,q1,5) && kfd_direct_kicks==2);
        assert(!dext_compute_bo_free(r0));
        /* Client close: queues, then the KFD process; nothing quarantines. */
        assert(!dext_compute_release_client(7));
        assert(dext_compute_aql_queue_kick_direct(7,q1,6)==-EAGAIN_L && kfd_direct_kicks==2);
        assert(kfd_queue_destroys==2 && kfd_closes==1 && !kfd_clients[0].live);
        dext_compute_select_client(7);
        assert(dext_compute_bo_get_info(m0,info)==-ENOENT_L);
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0 && closes==1 && dext_compute_quiescent());
    } else if (!strcmp(argv[1],"kfd-open-failure")) {
        /* A KFD process that cannot be opened on a device that supports
         * KFD is that client's error, said by every compute call (never the
         * legacy path, which may have no HQD); the open is not retried. */
        uint64_t words[16], window[3], handle=0, gpu=0, cpu=0;
        kfd_supported_error=0; kfd_open_error=-EIO;
        assert(!dext_compute_client_identity(9,4242,"lse"));
        dext_compute_select_client(9);
        assert(dext_compute_query_info(12,words,8)==-ENOTREADY_L && kfd_opens==1);
        assert(dext_compute_query_info(10,words,16)==-ENOTREADY_L);
        assert(dext_compute_bo_alloc(4096,2,4096,0,&handle,&gpu,&cpu)==-ENOTREADY_L);
        assert(dext_compute_host_window(0,window)==-ENOTREADY_L);
        assert(kfd_opens==1 && !legacy_creates && allocations==1 && !kfd_bo_allocs);
        assert(dext_compute_client_open_error(9)==-EIO && !dext_compute_client_owns(9));
        assert(dext_compute_client_records()==1 && !dext_compute_forget_client(9));
        assert(!dext_compute_client_records());
        /* Out of memory says so. */
        kfd_open_error=-ENOMEM;
        dext_compute_select_client(10);
        assert(dext_compute_query_info(12,words,8)==-ENOMEM_L);
        assert(!dext_compute_forget_client(10));
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0);
    } else if (!strcmp(argv[1],"kfd-stop")) {
        /* Last client gone: stop releases its KFD process before the
         * compute context. */
        uint64_t words[8], window[3], r0, m0, q0, status;
        kfd_supported_error=0;
        dext_compute_select_client(11);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
        {
            /* A KFD client's signal events. */
            uint64_t event[3];
            uint32_t id;
            struct rt_kfd_wait *wait = (struct rt_kfd_wait *)1;
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_CREATE,0,event) && event[0] &&
                   event[1]==event[0] && event[2]);
            id = (uint32_t)event[0];
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_SET,id,event));
            assert(dext_compute_event_wait_begin(&id,1,0,10,&wait)==-EBUSY_L && !wait);
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,id,event));
            assert(dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,4000,event)==-ENOENT_L);
            assert(dext_compute_event(7,id,event)==-EINVAL_L);
        }
        assert(!dext_compute_host_window(1ULL<<37,window));
        r0=alloc_bo(2); m0=alloc_bo(2);
        assert(!dext_compute_aql_queue_create(r0,m0,64,&status,&q0));
        dext_compute_select_client(0);
        assert(dext_compute_stop()==0 && kfd_closes==1 && kfd_queue_destroys==1 && closes==1);
    } else if (!strcmp(argv[1],"kfd-destroy-retained")) {
        /* MES did not confirm the removal: the process keeps its memory and
         * the device freezes as for a legacy queue. */
        uint64_t words[8], window[3], r0, m0, q0, status;
        kfd_supported_error=0;
        dext_compute_select_client(12);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
        {
            /* A KFD client's signal events. */
            uint64_t event[3];
            uint32_t id;
            struct rt_kfd_wait *wait = (struct rt_kfd_wait *)1;
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_CREATE,0,event) && event[0] &&
                   event[1]==event[0] && event[2]);
            id = (uint32_t)event[0];
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_SET,id,event));
            assert(dext_compute_event_wait_begin(&id,1,0,10,&wait)==-EBUSY_L && !wait);
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,id,event));
            assert(dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,4000,event)==-ENOENT_L);
            assert(dext_compute_event(7,id,event)==-EINVAL_L);
        }
        assert(!dext_compute_host_window(1ULL<<37,window));
        r0=alloc_bo(2); m0=alloc_bo(2);
        assert(!dext_compute_aql_queue_create(r0,m0,64,&status,&q0));
        kfd_destroy_error=-ETIMEDOUT;
        assert(dext_compute_aql_queue_destroy(q0,&status)==-EBUSY_L);
        dext_compute_select_client(0);
        expect_frozen(payload);
        assert(!kfd_closes);
    } else if (!strcmp(argv[1],"kfd-death-recovered") || !strcmp(argv[1],"kfd-death-kept")) {
        /* The client dies after MES failed to confirm a removal. Its close
         * (which retries the recovery) runs anyway: when the GPU let go the
         * device thaws, otherwise the client is kept and a later stop
         * retries. */
        uint64_t words[8], window[3], r0, m0, q0, status;
        const bool recovered=!strcmp(argv[1],"kfd-death-recovered");
        kfd_supported_error=0;
        dext_compute_select_client(13);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
        {
            /* A KFD client's signal events. */
            uint64_t event[3];
            uint32_t id;
            struct rt_kfd_wait *wait = (struct rt_kfd_wait *)1;
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_CREATE,0,event) && event[0] &&
                   event[1]==event[0] && event[2]);
            id = (uint32_t)event[0];
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_SET,id,event));
            assert(dext_compute_event_wait_begin(&id,1,0,10,&wait)==-EBUSY_L && !wait);
            assert(!dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,id,event));
            assert(dext_compute_event(DEXT_COMPUTE_EVENT_DESTROY,4000,event)==-ENOENT_L);
            assert(dext_compute_event(7,id,event)==-EINVAL_L);
        }
        assert(!dext_compute_host_window(1ULL<<37,window));
        r0=alloc_bo(2); m0=alloc_bo(2);
        assert(!dext_compute_aql_queue_create(r0,m0,64,&status,&q0));
        kfd_destroy_error=-ETIMEDOUT;
        assert(dext_compute_aql_queue_destroy(q0,&status)==-EBUSY_L);
        dext_compute_select_client(0);
        assert(dext_compute_bo_free(payload)==-EBUSY_L);
        if (recovered) kfd_destroy_error=0;
        assert(dext_compute_release_client(13)==(recovered ? 0 : -EBUSY_L));
        assert(kfd_closes==(recovered ? 1u : 0u));
        if (recovered) {
            /* Thawed: the device works again. */
            assert(!dext_compute_bo_free(payload));
        } else {
            assert(dext_compute_stop()==-EBUSY_L && !dext_compute_quiescent());
            kfd_destroy_error=0;
        }
        assert(dext_compute_stop()==0 && kfd_closes==1 && dext_compute_quiescent());
    } else if (!strcmp(argv[1],"kfd-fault-isolated")) {
        /* Client 21's GPU work faults while client 22 runs: 21's failed
         * launch and its evicted queues are 21's alone. Nothing freezes the
         * device, 22 runs throughout, and both leave with ordinary
         * releases. */
        uint64_t words[8], window[3], ra, ma, qa, rb, mb, qb, status, code, flags=1, va=1;
        kfd_supported_error=0;
        dext_compute_select_client(21);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
        assert(!dext_compute_host_window(1ULL<<37,window));
        ra=alloc_bo(2); ma=alloc_bo(2); code=alloc_bo(3);
        assert(!dext_compute_aql_queue_create(ra,ma,64,&status,&qa));
        struct dext_kfd_client *faulting=&kfd_clients[0];
        dext_compute_select_client(22);
        assert(dext_compute_query_info(12,words,8)==8 && words[1]==2);
        assert(!dext_compute_host_window(2ULL<<37,window));
        rb=alloc_bo(2); mb=alloc_bo(2);
        assert(!dext_compute_aql_queue_create(rb,mb,64,&status,&qb));
        /* 21's code-object sync launch times out after the cache sync went
         * to the kernel ring: an error for 21, no device freeze. */
        dext_compute_select_client(21);
        struct rt_dispatch_request sync_launch={.version=2,.code_handle=code,.code_bytes=64,
            .timeout_us=100000};
        dispatch_sequence=9; kfd_code_after_sync_error=-ETIMEDOUT;
        assert(dext_compute_dispatch(&sync_launch,sizeof(sync_launch),out)==-ENOTREADY_L);
        kfd_code_after_sync_error=0;
        assert(dext_compute_query_info(1,out,10)==3);	/* not frozen */
        /* The CP stops 21's queue first, with an error code: reported
         * with the code, not as a failed call. */
        kfd_stopped_code=0x80;
        assert(dext_compute_aql_queue_service(qa,out,out+1)==-EQUEUE_L && out[1]==0x80);
        kfd_stopped_code=0;
        assert(dext_compute_query_info(1,out,10)==3);	/* not frozen */
        /* The fault: KFD evicted 21's queues for good. */
        kfd_faulted=faulting;
        assert(dext_compute_aql_queue_service(qa,out,out+1)==-EFAULT_L);
        assert(!dext_compute_aql_queue_fault(qa,&flags,&va) &&
               flags==(DEXT_KFD_FAULT_VALID|DEXT_KFD_FAULT_NOT_PRESENT) && va==0x1235d4000ULL);
        assert(dext_compute_aql_queue_kick(qa,0,out)==-EFAULT_L);
        assert(dext_compute_aql_queue_kick_direct(21,qa,0)==-EFAULT_L);
        assert(!dext_compute_aql_queue_kick_direct(22,qb,0));
        /* 22 neither sees it nor stops. */
        dext_compute_select_client(22);
        assert(!dext_compute_aql_queue_kick(qb,0,out) && !dext_compute_aql_queue_service(qb,out,out+1));
        assert(!dext_compute_aql_queue_fault(qb,&flags,&va) && !flags && !va);
        /* 21 leaves; the device and 22 go on; then 22 leaves. */
        dext_compute_select_client(0);
        assert(dext_compute_release_client(21)==0 && kfd_closes==1);
        assert(dext_compute_aql_queue_kick_direct(21,qa,1)==-EAGAIN_L);
        kfd_faulted=NULL;
        assert(!dext_compute_bo_free(payload));
        dext_compute_select_client(22);
        assert(!dext_compute_aql_queue_kick(qb,1,out));
        dext_compute_select_client(0);
        assert(dext_compute_release_client(22)==0 && kfd_closes==2);
        assert(dext_compute_stop()==0 && dext_compute_quiescent());
    } else if (!strcmp(argv[1],"allocation-cleanup")) {
        alloc_info_error=-EIO; free_error=-EBUSY;
        assert(dext_compute_bo_alloc(4096,1,4096,0,out,NULL,NULL)==-ENOTREADY_L);
        assert(frees==1); expect_frozen(payload);
    } else if (!strcmp(argv[1],"create-retained") || !strcmp(argv[1],"create-oom")) {
        uint64_t ring=alloc_bo(2), meta=alloc_bo(2);
        create_retained=!strcmp(argv[1],"create-retained");
        create_error=create_retained ? -ETIMEDOUT : -ENOMEM;
        assert(dext_compute_aql_queue_create(ring,meta,64,out,out+1)==
               (create_retained ? -EBUSY_L : -ENOTREADY_L));
        if (create_retained) expect_frozen(payload);
        else assert(dext_compute_stop()==0 && frees==3 && closes==1);
    } else {
        uint64_t ring, meta, q=create_queue(&ring,&meta);
        assert(dext_compute_bo_free(ring)==-EBUSY_L);
        assert(dext_compute_bo_free(meta)==-EBUSY_L);
        assert(!frees);
        if (!strcmp(argv[1],"service-retained")) {
            service_error=-EIO; service_retained=true;
            assert(dext_compute_aql_queue_service(q,out,out+1)==-EBUSY_L);
            assert(services==1 && !frees); expect_frozen(payload);
        } else if (!strcmp(argv[1],"kick-poison")) {
            kick_error=-EIO; kick_poison=true;
            assert(dext_compute_aql_queue_kick(q,0,out)==-EBUSY_L);
            assert(kicks==1); expect_frozen(payload);
        } else if (!strcmp(argv[1],"destroy-retained")) {
            destroy_error=-ETIMEDOUT;
            assert(dext_compute_aql_queue_destroy(q,out)==-EBUSY_L);
            assert(destroys==1); expect_frozen(payload);
        } else if (!strcmp(argv[1],"stop-retained")) {
            destroy_error=-ETIMEDOUT;
            assert(dext_compute_stop()==-EBUSY_L);
            assert(destroys==1 && !frees && !closes); expect_frozen(payload);
        } else if (!strcmp(argv[1],"stop-removed")) {
            /* The queue could not be removed, then the device left the
             * bus: nothing can run the queue, so the stop completes and
             * every buffer goes. */
            destroy_error=-ETIMEDOUT;
            assert(dext_compute_stop()==-EBUSY_L && !closes);
            dext_compute_device_removed(); device_removed=true;
            assert(dext_compute_stop()==0 && closes==1 && frees==3);
            assert(dext_compute_quiescent());
        } else if (!strcmp(argv[1],"ordinary-errors")) {
            kick_error=-EINVAL;
            assert(dext_compute_aql_queue_kick(q,UINT64_MAX,out)==-ENOTREADY_L);
            service_error=-ENOMEM;
            assert(dext_compute_aql_queue_service(q,out,out+1)==-ENOTREADY_L);
            service_error=-EINVAL;
            assert(dext_compute_aql_queue_service(q,out,out+1)==-ENOTREADY_L);
            kick_error=service_error=0;
            assert(dext_compute_aql_queue_kick(q,0,out)==0);
            assert(dext_compute_aql_queue_service(q,out,out+1)==0);
            assert(dext_compute_aql_queue_destroy(q,out)==0);
            assert(dext_compute_stop()==0 && frees==3 && closes==1);
        } else if (!strcmp(argv[1],"close-retained")) {
            close_error=-EBUSY;
            assert(dext_compute_stop()==-EBUSY_L);
            assert(destroys==1 && frees==3 && closes==1);
            assert(dext_compute_stop()==-EBUSY_L && closes==1);
        } else { assert(!"unknown scenario"); }
    }
    puts(argv[1]); return 0;
}
