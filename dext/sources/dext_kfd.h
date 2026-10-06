/* KFD-backed compute clients: the runtime's AQL queue ABI over a KFD
 * compute session (rt/kfd_session.h).
 *
 * A client opened here is one Linux KFD process. Its BOs live in that
 * process's GPUVM: shared GTT buffers at CPU VA == GPU VA inside the
 * client's host window, VRAM and queue resources in the session's private
 * range. Persistent queues are KFD user queues (CREATE_QUEUE, scheduled by
 * MES over its HQD pool), so their number is KFD's per-process limit, not
 * the legacy HQDs the queue partition leaves free. Bounded launches
 * (selectors 51 and 55) run on a short-lived KFD queue of the same process.
 *
 * Self-contained C API (no DriverKit headers) shared by the selector
 * backend and its host tests; the DriverKit implementation is
 * dext_kfd.mm. */
#ifndef MACLINUXGPU_DEXT_KFD_H
#define MACLINUXGPU_DEXT_KFD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct rt_compute_ctx;
struct rt_kfd_bo;
struct dext_kfd_client;
struct dext_kfd_queue;

/* 0 when the device can back KFD clients (rt_kfd_session_supported). */
int dext_kfd_supported(struct rt_compute_ctx *ctx);
int dext_kfd_open(struct rt_compute_ctx *ctx, int pid, const char *comm,
                  struct dext_kfd_client **out);
/* Destroy every queue and BO and exit the KFD process, as Linux tears down
 * a process that dies with live queues: a queue MES does not confirm
 * removing is recovered through MES's hung-queue reset, and a copy that
 * outlived its timeout is waited for once more (rt_kfd_session_close).
 * -EBUSY when GPU completion is still uncertain: the client is kept and
 * only dext_kfd_settle or another close may touch it. */
int dext_kfd_close(struct dext_kfd_client *c);
int dext_kfd_uncertain(const struct dext_kfd_client *c);
/* Retry what left the client uncertain (rt_kfd_session_settle) and drop the
 * queue records whose queue the session recovered. 0 when the client is
 * certain again and may be used or closed normally. */
int dext_kfd_settle(struct dext_kfd_client *c, unsigned int wait_ms);

/* What the runtime sees of the client (QueryInfo tags 10 and 12). */
struct dext_kfd_info {
    int pid;
    uint32_t slots;              /* KFD's per-process queue limit */
    uint64_t window_base;        /* 0 until set */
    uint64_t window_size;
    uint64_t gpuvm_base, gpuvm_limit;
};
int dext_kfd_info(struct dext_kfd_client *c, struct dext_kfd_info *out);
/* size 0: the offered window size (rt_kfd_session_set_window). */
int dext_kfd_set_window(struct dext_kfd_client *c, uint64_t base, uint64_t size);
struct dext_aql_limits;
/* The queue ABI limits (scratch per work-item, ring sizes) of KFD-backed
 * queues; slots is left to dext_kfd_info. */
int dext_kfd_queue_abi(struct dext_aql_limits *out);

/* domain: 2 GTT (shared, in the window), 3 VRAM (private). */
int dext_kfd_bo_alloc(struct dext_kfd_client *c, uint64_t size, uint64_t alignment,
                      uint32_t domain, struct rt_kfd_bo **out, uint64_t *va);
int dext_kfd_bo_free(struct dext_kfd_client *c, struct rt_kfd_bo *bo);
int dext_kfd_bo_read(struct dext_kfd_client *c, struct rt_kfd_bo *bo,
                     uint64_t offset, void *dst, size_t bytes);
int dext_kfd_bo_write(struct dext_kfd_client *c, struct rt_kfd_bo *bo,
                      uint64_t offset, const void *src, size_t bytes);
int dext_kfd_bo_copy(struct dext_kfd_client *c, struct rt_kfd_bo *src, uint64_t src_offset,
                     struct rt_kfd_bo *dst, uint64_t dst_offset, uint64_t bytes);
/* The host pages of a GTT BO as CPU-contiguous runs (for client mappings). */
int dext_kfd_bo_ranges(struct dext_kfd_client *c, struct rt_kfd_bo *bo,
                       int (*fn)(void *arg, void *cpu, uint64_t bytes), void *arg);

/* The persistent queue ABI of selectors 56-59 (dext_aql.h's semantics):
 * the runtime's ring and amd_queue_t metadata BOs, the dext fills the
 * device fields of amd_queue_t (apertures, CU/wave limits, scratch,
 * inactive signal). */
int dext_kfd_queue_create(struct dext_kfd_client *c, struct rt_kfd_bo *ring,
                          struct rt_kfd_bo *metadata, uint32_t packets,
                          struct dext_kfd_queue **out);
int dext_kfd_queue_kick(struct dext_kfd_queue *q, uint64_t packet);
/* -EFAULT once the client's process took a GPU memory fault: KFD evicted
 * its queues, which never run again (dext_kfd_fault says where).
 * -ENOEXEC when the CP stopped the queue with an error code (left in
 * *inactive) that is not a scratch request; a fault it hit is reported as
 * -EFAULT once KFD's interrupt work signaled the process's memory event. */
int dext_kfd_queue_service(struct dext_kfd_queue *q, uint64_t *inactive);
int dext_kfd_queue_destroy(struct dext_kfd_queue *q);
unsigned int dext_kfd_queue_count(struct dext_kfd_client *c);
/* The client's GPU memory fault (rt_kfd_session_fault): 1 with *flags
 * (DEXT_KFD_FAULT_*) and *va once its process faulted, 0 while not. */
#define DEXT_KFD_FAULT_VALID		(1u << 31)
#define DEXT_KFD_FAULT_NOT_PRESENT	(1u << 0)
#define DEXT_KFD_FAULT_READ_ONLY	(1u << 1)
#define DEXT_KFD_FAULT_NO_EXECUTE	(1u << 2)
#define DEXT_KFD_FAULT_IMPRECISE	(1u << 3)
int dext_kfd_fault(struct dext_kfd_client *c, uint32_t *flags, uint64_t *va);

/* Signal events of the client's KFD process and waits on them
 * (rt_kfd_event_create, rt_kfd_wait_begin/run). */
struct rt_kfd_wait;
int dext_kfd_event_create(struct dext_kfd_client *c, uint32_t *id, uint32_t *trigger,
                          uint64_t *mailbox_va);
int dext_kfd_event_destroy(struct dext_kfd_client *c, uint32_t id);
int dext_kfd_event_set(struct dext_kfd_client *c, uint32_t id);
int dext_kfd_wait_begin(struct dext_kfd_client *c, const uint32_t *ids, uint32_t count,
                        int all, uint32_t timeout_ms, struct rt_kfd_wait **out);

/* Bounded launches, out[] as dext_aql_dispatch_bounded/dispatch_code. The
 * queue is destroyed through KFD before returning, so only a failed
 * destroy leaves the launch uncertain. */
int dext_kfd_dispatch_bounded(struct dext_kfd_client *c, uint64_t descriptorVA,
                              uint64_t kernargVA, const void *request, size_t request_size,
                              uint64_t out[5], int *uncertain);
int dext_kfd_dispatch_code(struct dext_kfd_client *c, uint64_t codeVA,
                           const void *request, size_t request_size,
                           int (*before_map)(void *), void *arg,
                           uint64_t out[5], int *uncertain);
#ifdef __cplusplus
}
#endif
#endif
