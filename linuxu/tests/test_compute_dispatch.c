/* Selector 51's cache synchronization (rt_compute_cache_sync) over the
 * kernel compute ring, with ring submission and completion mocked. The IB
 * must carry only the ring's own NOP padding and ask amdgpu_ib_schedule()
 * for the ring's upstream ACQUIRE_MEM (AMDGPU_IB_FLAG_EMIT_MEM_SYNC); no
 * packet or register value comes from this code. No DriverKit service,
 * MMIO, or hardware is used by this executable. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <drm/amdgpu_drm.h>
#include <rt/compute.h>
#include <rt/dispatch.h>
#include "amdgpu.h"
#include "amdgpu_job.h"
#include "amdgpu_ring.h"

#define FAMILY_NOP 0xc0ff1000u /* whatever the ring's family defines */

static struct amdgpu_device device;
static long wait_result;
static int fence_status, submit_error, omit_fence, alloc_error;
static unsigned allocations, submissions, job_frees, fence_frees, pads, syncs;
static uint32_t ib_words;

int rt_compute_status(struct rt_compute_ctx *ctx) { return ctx ? 0 : -ENODEV; }
struct amdgpu_device *rt_compute_device(struct rt_compute_ctx *ctx)
{ (void)ctx; return &device; }

static void pad_ib(struct amdgpu_ring *ring, struct amdgpu_ib *ib)
{
	pads++;
	while (ib->length_dw & ring->funcs->align_mask)
		ib->ptr[ib->length_dw++] = ring->funcs->nop;
}
static void emit_mem_sync(struct amdgpu_ring *ring) { (void)ring; syncs++; }

int amdgpu_job_alloc_with_ib(struct amdgpu_device *adev,
        struct drm_sched_entity *entity, void *owner, size_t size,
        enum amdgpu_ib_pool_type pool, struct amdgpu_job **out, u64 id)
{
	(void)adev; (void)entity; (void)owner; (void)id;
	assert(pool == AMDGPU_IB_POOL_DIRECT);
	if (alloc_error) return alloc_error;
	assert(size == ib_words * 4);
	*out = calloc(1, sizeof(**out) + sizeof(struct amdgpu_ib));
	assert(*out);
	(*out)->ibs[0].ptr = malloc(size);
	assert((*out)->ibs[0].ptr);
	allocations++;
	return 0;
}
int amdgpu_ib_schedule(struct amdgpu_ring *ring, unsigned count,
        struct amdgpu_ib *ibs, struct amdgpu_job *job, struct dma_fence **out)
{
	(void)job;
	assert(ring == &device.gfx.compute_ring[0] && count == 1);
	/* Only the family's NOP padding; the ACQUIRE_MEM is the ring's. */
	assert(ibs[0].length_dw == ib_words);
	for (unsigned i = 0; i < ibs[0].length_dw; ++i)
		assert(ibs[0].ptr[i] == FAMILY_NOP);
	assert(ibs[0].flags == AMDGPU_IB_FLAG_EMIT_MEM_SYNC);
	if (ring->funcs->emit_mem_sync) ring->funcs->emit_mem_sync(ring);
	submissions++;
	if (submit_error) return submit_error;
	if (omit_fence) return 0;
	*out = calloc(1, sizeof(**out));
	assert(*out);
	kref_init(&(*out)->refcount);
	(*out)->seqno = 77;
	return 0;
}
void amdgpu_job_free(struct amdgpu_job *job)
{
	assert(job);
	free(job->ibs[0].ptr);
	free(job);
	job_frees++;
}
long dma_fence_wait_timeout(struct dma_fence *f, bool intr, long timeout)
{ assert(f && !intr && timeout > 0); return wait_result; }
int dma_fence_get_status(struct dma_fence *f) { assert(f); return fence_status; }
void dma_fence_release(struct kref *ref)
{ free(container_of(ref, struct dma_fence, refcount)); fence_frees++; }

int main(void)
{
	struct amdgpu_ring_funcs funcs = {
		.type = AMDGPU_RING_TYPE_COMPUTE, .align_mask = 0xff, .nop = FAMILY_NOP,
		.pad_ib = pad_ib, .emit_mem_sync = emit_mem_sync,
	};
	struct rt_compute_ctx *ctx = (void *)&device;
	uint64_t seq;
	device.gfx.num_compute_rings = 1;
	device.gfx.compute_ring[0].sched.ready = true;
	device.gfx.compute_ring[0].funcs = &funcs;
	ib_words = 256;

	wait_result = 1; fence_status = 1;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == 0 && seq == 77);
	assert(pads == 1 && syncs == 1);
	wait_result = 0;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -ETIMEDOUT && seq == 77);
	wait_result = -EINTR;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -EINTR && seq == 77);
	wait_result = 1; fence_status = -EIO;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -EIO && seq == 77);
	fence_status = 0;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -EIO);
	omit_fence = 1;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -EIO && seq == 0);
	omit_fence = 0; submit_error = -ENOMEM;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -ENOMEM && seq == 0);
	submit_error = 0; alloc_error = -ENOMEM;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -ENOMEM && seq == 0);
	alloc_error = 0;

	/* Another family's IB alignment. */
	funcs.align_mask = 0x7; ib_words = 8; fence_status = 1;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == 0 && seq == 77);

	/* Refused without submitting: bad timeouts, a ring without the
	 * upstream mem-sync or padding, or no ready compute ring. */
	unsigned before = allocations;
	assert(rt_compute_cache_sync(ctx, 0, &seq) == -EINVAL);
	assert(rt_compute_cache_sync(ctx, 1000001, &seq) == -EINVAL);
	assert(rt_compute_cache_sync(ctx, 1000, NULL) == -EINVAL);
	funcs.emit_mem_sync = NULL;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -ENODEV);
	funcs.emit_mem_sync = emit_mem_sync; funcs.pad_ib = NULL;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -ENODEV);
	funcs.pad_ib = pad_ib;
	device.gfx.compute_ring[0].sched.ready = false;
	assert(rt_compute_cache_sync(ctx, 1000, &seq) == -ENODEV);
	device.gfx.compute_ring[0].sched.ready = true;
	assert(rt_compute_cache_sync(NULL, 1000, &seq) == -ENODEV);
	assert(allocations == before && allocations == job_frees);
	assert(fence_frees == submissions - 2);
	puts("selector 51 cache sync: upstream ring padding and mem-sync, completion and failure passed");
	return 0;
}
