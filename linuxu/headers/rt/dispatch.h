#ifndef LINUXU_RT_DISPATCH_H
#define LINUXU_RT_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rt_compute_ctx;

/* Wire-compatible with ComputeDispatchRequest selector 51, version 2
 * (dext/amdgpu/amdgpu_dispatch_abi.h). */
struct rt_dispatch_request {
	uint32_t version;
	uint32_t flags;
	uint64_t code_handle;
	uint64_t code_offset;
	uint64_t code_bytes;
	uint32_t groups[3];
	uint32_t threads[3];
	uint32_t rsrc1;
	uint32_t rsrc2;
	uint32_t user_sgpr_count;
	uint32_t timeout_us;
	uint32_t user_sgpr[16];
	uint64_t buffers[16];
	uint32_t rsrc3;
	uint32_t reserved;
};

#ifdef __cplusplus
static_assert(offsetof(struct rt_dispatch_request, rsrc3) == 264,
	      "selector 51 legacy prefix size");
static_assert(sizeof(struct rt_dispatch_request) == 272,
	      "selector 51 version 2 size");
#else
_Static_assert(offsetof(struct rt_dispatch_request, rsrc3) == 264,
	       "selector 51 legacy prefix size");
_Static_assert(sizeof(struct rt_dispatch_request) == 272,
	       "selector 51 version 2 size");
#endif

/* Invalidates and writes back the GPU caches (instruction, scalar, vector,
 * GL1, GL2) with the compute ring's upstream ACQUIRE_MEM, then waits for its
 * fence. out_fence_sequence is set after submission, including when the
 * wait times out; a timeout may leave the submission in flight. */
int rt_compute_cache_sync(struct rt_compute_ctx *ctx, uint32_t timeout_us,
			  uint64_t *out_fence_sequence);

#ifdef __cplusplus
}
#endif
#endif
