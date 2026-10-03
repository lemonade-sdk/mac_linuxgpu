/* GPU cache synchronization over an initialized upstream compute ring.
 *
 * Selector 51 launches run on the AQL legacy queue (dext_aql_dispatch_code);
 * this file supplies the cache invalidation and writeback that bracket them
 * for code and data written outside the GPU.  Every packet comes from the
 * ring's upstream per-family functions: the IB holds only the ring's NOP
 * padding (ring->funcs->nop / pad_ib) and is scheduled with
 * AMDGPU_IB_FLAG_EMIT_MEM_SYNC, so amdgpu_ib_schedule() emits the family's
 * own ACQUIRE_MEM (ring->funcs->emit_mem_sync, e.g. gfx_v12_0_emit_mem_sync:
 * GL2/GLM invalidate and writeback, GL1/GLV/GLK/GLI invalidate) and fence. */
#include <stdint.h>
#include <string.h>

#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <drm/amdgpu_drm.h>
#include <rt/compute.h>
#include <rt/dispatch.h>

#include "amdgpu.h"
#include "amdgpu_job.h"
#include "amdgpu_ring.h"

/* Bound on the IB size one padding unit may need. */
#define RT_SYNC_MAX_DWORDS 4096u

int rt_compute_cache_sync(struct rt_compute_ctx *ctx, uint32_t timeout_us,
			  uint64_t *out_fence_sequence)
{
	struct amdgpu_device *adev;
	struct amdgpu_ring *ring;
	struct amdgpu_job *job = NULL;
	struct amdgpu_ib *ib;
	struct dma_fence *fence = NULL;
	uint32_t unit;
	long waited;
	int r;

	if (!out_fence_sequence)
		return -EINVAL;
	*out_fence_sequence = 0;
	if (!timeout_us || timeout_us > 1000000)
		return -EINVAL;
	if (rt_compute_status(ctx) != 0)
		return -ENODEV;
	adev = rt_compute_device(ctx);
	if (!adev || !adev->gfx.num_compute_rings)
		return -ENODEV;
	ring = &adev->gfx.compute_ring[0];
	if (!ring->sched.ready || !ring->funcs ||
	    ring->funcs->type != AMDGPU_RING_TYPE_COMPUTE ||
	    !ring->funcs->emit_mem_sync || !ring->funcs->pad_ib)
		return -ENODEV;
	/* One NOP padded to the ring's IB alignment. */
	unit = ring->funcs->align_mask + 1;
	if (!unit || unit > RT_SYNC_MAX_DWORDS)
		return -ENODEV;
	r = amdgpu_job_alloc_with_ib(adev, NULL, NULL, unit * 4,
				     AMDGPU_IB_POOL_DIRECT, &job, 0);
	if (r)
		return r;
	ib = &job->ibs[0];
	ib->ptr[0] = ring->funcs->nop;
	ib->length_dw = 1;
	amdgpu_ring_pad_ib(ring, ib);
	if (!ib->length_dw || ib->length_dw > unit) {
		r = -EINVAL;
		goto out;
	}
	ib->flags = AMDGPU_IB_FLAG_EMIT_MEM_SYNC;
	r = amdgpu_ib_schedule(ring, 1, ib, job, &fence);
	if (r)
		goto out;
	if (!fence) {
		r = -EIO;
		goto out;
	}
	*out_fence_sequence = fence->seqno;
	waited = dma_fence_wait_timeout(fence, false,
		msecs_to_jiffies((timeout_us + 999u) / 1000u));
	r = waited > 0 ? dma_fence_get_status(fence) :
	    (waited == 0 ? -ETIMEDOUT : (int)waited);
	if (r > 0)
		r = 0;
	else if (r == 0)
		r = -EIO;
out:
	/* Upstream amdgpu_job_free_resources passes the initialized hardware
	 * fence to its IB suballocator, retaining IB storage until it signals. */
	dma_fence_put(fence);
	amdgpu_job_free(job);
	return r;
}
