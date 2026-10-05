#ifndef MACLINUXGPU_DEXT_AQL_H
#define MACLINUXGPU_DEXT_AQL_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct rt_compute_ctx;
struct rt_compute_bo;
struct dext_aql_queue;
int dext_aql_available(struct rt_compute_ctx *);
/* What a persistent AQL queue accepts: the largest per-work-item scratch,
 * the ring size range in packets, and the legacy HQDs the queue partition
 * reserves for DriverKit queues (slots). */
struct dext_aql_limits {
    uint32_t max_private_bytes;
    uint32_t min_packets, max_packets;
    uint32_t slots;
};
int dext_aql_limits(struct rt_compute_ctx *, struct dext_aql_limits *);
int dext_aql_create(struct rt_compute_ctx *, struct rt_compute_bo *ring,
                    struct rt_compute_bo *metadata, uint32_t packets,
                    struct dext_aql_queue **);
int dext_aql_kick(struct dext_aql_queue *, uint64_t packet);
/* Around a device reset (rt/recovery.h's queue hooks), from the reset
 * domain's thread: every mapped queue is unmapped with the GPU's state;
 * after the reset each is written again and mapped at its producer's
 * position (a destroy in between only frees it). A failed restore leaves
 * the queue retained (dext_aql_uncertain); returns how many failed. */
void dext_aql_reset_prepare(void);
int dext_aql_reset_restore(void);
int dext_aql_service(struct dext_aql_queue *, uint64_t *inactive);
/* Retention can begin during scratch service, not only create/destroy. Any
 * uncertain queue requires its caller to retain every client BO as well. */
int dext_aql_uncertain(const struct dext_aql_queue *);
/* A failed create/destroy may leave a retained queue: never release its BOs. */
int dext_aql_destroy(struct dext_aql_queue *);
/* One owned VMID0 dispatch. The caller validates descriptor/kernarg BO ranges
 * and retains all referenced user BOs for the call. Failure after MES map
 * may leave firmware owning the queue; uncertain then stays true and the
 * storage/slot remain allocated until verified device reset. */
int dext_aql_dispatch_bounded(struct rt_compute_ctx *ctx,
                              uint64_t descriptorVA, uint64_t kernargVA,
                              const void *request, size_t request_size,
                              uint64_t out[5], int *uncertain);
/* The same bounded dispatch for a raw shader launch (selector 51's
 * ComputeDispatchRequest, version 1 zero-extended to version 2): the code
 * address, PGM_RSRC1/2/3 and user SGPRs become a synthesized AMDHSA kernel
 * descriptor in the queue's storage block (aql_code_launch()). -EINVAL for a
 * launch AQL cannot express; -ENOSPC when every DriverKit HQD is held; both
 * before any hardware access. before_map(arg), when given, runs after the
 * HQD is reserved and the queue built, immediately before it is mapped; a
 * nonzero return abandons the launch with that error. */
/* Whether the device's COMPUTE_PGM_RSRC1 has DX10_CLAMP and IEEE_MODE, which
 * compute_dispatch_shape() then accepts. */
int dext_aql_rsrc1_clamp_ieee(struct rt_compute_ctx *ctx);
int dext_aql_dispatch_code(struct rt_compute_ctx *ctx, uint64_t codeVA,
                           const void *request, size_t request_size,
                           int (*before_map)(void *), void *arg,
                           uint64_t out[5], int *uncertain);
#ifdef __cplusplus
}
#endif
#endif
