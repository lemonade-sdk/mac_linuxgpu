/* Another process's memory as a GPU buffer (rt/surface.h). */
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <rt/dart.h>
#include <rt/surface.h>
#include <rt/removal.h>

#include "amdgpu.h"
#include "amdgpu_dma_buf.h"
#include "sdma_v6_0_0_pkt_open.h"	/* the packet sdma_v7_0.c builds with too */

/* The exporter's state: the platform's mapping, released with the dma-buf. */
struct surface_buffer {
	struct rt_surface_segment *segments;
	uint32_t count;
	struct rt_surface_provider provider;
	bool armed;	/* the import succeeded: the provider is released with us */
};

struct rt_surface {
	int refs;		/* the import's, plus each hold (rt_surface_hold) */
	struct drm_gem_object *obj;
	uint64_t gpu_address;
	uint64_t size;
	uint32_t width, height, pitch;
	void *provider_context;
};

static struct sg_table *surface_map(struct dma_buf_attachment *attach,
				    enum dma_data_direction direction)
{
	struct surface_buffer *buffer = attach->dmabuf->priv;
	struct sg_table *table;
	(void)direction;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);
	if (sg_alloc_table(table, buffer->count, GFP_KERNEL)) {
		kfree(table);
		return ERR_PTR(-ENOMEM);
	}
	/* Device addresses only: no CPU page backs them in this process. */
	for (uint32_t i = 0; i < buffer->count; i++) {
		sg_dma_address(&table->sgl[i]) = buffer->segments[i].dma_address;
		sg_dma_len(&table->sgl[i]) = (unsigned int)buffer->segments[i].length;
		table->sgl[i].flags |= SG_DMA_ADDR;
	}
	return table;
}

static void surface_unmap(struct dma_buf_attachment *attach, struct sg_table *table,
			  enum dma_data_direction direction)
{
	(void)attach;
	(void)direction;
	sg_free_table(table);
	kfree(table);
}

/* The memory is pinned by the platform for the buffer's whole life. */
static int surface_pin(struct dma_buf_attachment *attach)
{
	(void)attach;
	return 0;
}

static void surface_unpin(struct dma_buf_attachment *attach)
{
	(void)attach;
}

static void surface_release_buffer(struct dma_buf *buf)
{
	struct surface_buffer *buffer = buf->priv;

	for (uint32_t i = 0; i < buffer->count; i++)
		linuxu_dart_import_release(buffer->segments[i].dma_address,
					   buffer->segments[i].length);
	if (buffer->armed && buffer->provider.release)
		buffer->provider.release(buffer->provider.context);
	kfree(buffer->segments);
	kfree(buffer);
}

static const struct dma_buf_ops surface_buffer_ops = {
	.pin = surface_pin,
	.unpin = surface_unpin,
	.map_dma_buf = surface_map,
	.unmap_dma_buf = surface_unmap,
	.release = surface_release_buffer,
};

static bool segments_valid(const struct rt_surface_segment *segments, uint32_t count,
			   uint64_t size)
{
	uint64_t total = 0;

	if (!segments || !count || count > RT_SURFACE_SEGMENTS_MAX)
		return false;
	for (uint32_t i = 0; i < count; i++) {
		const struct rt_surface_segment *s = &segments[i];

		if (!s->dma_address || !s->length || (s->dma_address | s->length) & (PAGE_SIZE - 1) ||
		    s->length > UINT32_MAX - PAGE_SIZE + 1 || s->length > UINT64_MAX - total ||
		    s->length - 1 > UINT64_MAX - s->dma_address)
			return false;
		total += s->length;
	}
	return total == size;
}

int rt_surface_import(struct pci_dev *pdev, const struct rt_surface_segment *segments,
		      uint32_t count, uint64_t size, uint32_t width, uint32_t height,
		      uint32_t pitch, const struct rt_surface_provider *provider,
		      struct rt_surface **out)
{
	struct drm_device *dev = pdev ? pci_get_drvdata(pdev) : NULL;
	struct dma_buf_export_info info = { .exp_name = "linuxu-surface" };
	struct surface_buffer *buffer;
	struct rt_surface *surface;
	struct amdgpu_bo *bo;
	struct dma_buf *buf;
	uint32_t charged = 0;
	int r;

	if (!out || !provider || !provider->release)
		return -EINVAL;
	*out = NULL;
	if (!dev || rt_removal_active(drm_to_adev(dev)))
		return -ENODEV;
	if (!width || !height || pitch < (uint64_t)width * 4 || pitch & 3 ||
	    (uint64_t)pitch * height > size || !segments_valid(segments, count, size))
		return -EINVAL;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	surface = kzalloc(sizeof(*surface), GFP_KERNEL);
	if (buffer)
		buffer->segments = kmalloc_array(count, sizeof(*segments), GFP_KERNEL);
	if (!buffer || !surface || !buffer->segments) {
		if (buffer)
			kfree(buffer->segments);
		kfree(buffer);
		kfree(surface);
		return -ENOMEM;
	}
	memcpy(buffer->segments, segments, count * sizeof(*segments));
	buffer->provider = *provider;
	for (; charged < count; charged++) {
		r = linuxu_dart_import(segments[charged].dma_address, segments[charged].length);
		if (r)
			goto uncharge;
	}
	buffer->count = count;

	info.ops = &surface_buffer_ops;
	info.size = size;
	info.flags = O_RDWR;
	info.priv = buffer;
	buf = dma_buf_export(&info);
	if (IS_ERR(buf)) {
		r = PTR_ERR(buf);
		goto uncharge;
	}
	/* From here the dma-buf owns @buffer and returns the DART charge. */
	surface->obj = amdgpu_gem_prime_import(dev, buf);
	dma_buf_put(buf);
	if (IS_ERR(surface->obj)) {
		r = PTR_ERR(surface->obj);
		kfree(surface);
		return r;
	}
	bo = gem_to_amdgpu_bo(surface->obj);
	r = amdgpu_bo_reserve(bo, false);
	if (r)
		goto put;
	r = amdgpu_bo_pin(bo, AMDGPU_GEM_DOMAIN_GTT);
	if (!r) {
		r = amdgpu_ttm_alloc_gart(&bo->tbo);
		if (r)
			amdgpu_bo_unpin(bo);
		else
			surface->gpu_address = amdgpu_bo_gpu_offset(bo);
	}
	amdgpu_bo_unreserve(bo);
	if (r)
		goto put;
	surface->size = size;
	surface->width = width;
	surface->height = height;
	surface->pitch = pitch;
	surface->provider_context = provider->context;
	surface->refs = 1;
	/* Success: the provider is released with the dma-buf. */
	buffer->armed = true;
	*out = surface;
	return 0;

put:
	/* The dma-buf goes with the BO; @buffer stays disarmed. */
	drm_gem_object_put(surface->obj);
	kfree(surface);
	return r;

uncharge:
	while (charged--)
		linuxu_dart_import_release(segments[charged].dma_address, segments[charged].length);
	kfree(buffer->segments);
	kfree(buffer);
	kfree(surface);
	return r;
}

void rt_surface_hold(struct rt_surface *surface)
{
	if (surface)
		__atomic_add_fetch(&surface->refs, 1, __ATOMIC_RELAXED);
}

void rt_surface_release(struct rt_surface *surface)
{
	struct amdgpu_bo *bo;

	if (!surface || __atomic_sub_fetch(&surface->refs, 1, __ATOMIC_ACQ_REL))
		return;
	bo = gem_to_amdgpu_bo(surface->obj);
	if (!amdgpu_bo_reserve(bo, true)) {
		amdgpu_bo_unpin(bo);
		amdgpu_bo_unreserve(bo);
	}
	drm_gem_object_put(surface->obj);
	kfree(surface);
}

void rt_surface_geometry(const struct rt_surface *surface, uint32_t *width, uint32_t *height,
			 uint32_t *pitch)
{
	*width = surface ? surface->width : 0;
	*height = surface ? surface->height : 0;
	*pitch = surface ? surface->pitch : 0;
}

uint64_t rt_surface_gpu_address(const struct rt_surface *surface)
{
	return surface ? surface->gpu_address : 0;
}

/* The copies of one rt_surface_copy() call, batched into SDMA jobs on the
 * TTM buffer-function entity: each job carries up to COPY_PACKETS_PER_JOB
 * copy packets (as amdgpu_copy_buffer() builds one job of several packets
 * for a range longer than copy_max_bytes), so a frame's damage is a few
 * jobs, not one per row. */
#define COPY_PACKETS_PER_JOB	1024u
/* SDMA's linear sub-window copy (COPY sub-opcode 4): a rectangle between
 * two pitched buffers in one 13-dword packet rather than a copy per row.
 * The fields as Mesa's ac_emit_sdma_copy_linear_sub_window writes them:
 * element size log2, x and y in elements, pitch - 1 in elements (shifted
 * by 16 from SDMA 7.0), slice pitch - 1, width - 1 and height - 1. */
#define SUBWIN_DW		13u
#define SUBWIN_DIM_MAX		16384u	/* x, y, width, height: 14 bits */
#define SUBWIN_PITCH_MAX	65536u	/* SDMA 7: 16 bits of pitch - 1, in elements */
#define COPY_JOB_DW		(COPY_PACKETS_PER_JOB * SUBWIN_DW)

struct copy_batch {
	struct amdgpu_device *adev;
	struct drm_sched_entity *entity;	/* NULL: the TTM buffer-function entity */
	struct amdgpu_ring *ring;		/* the entity's ring (for IB padding) */
	struct amdgpu_job *job;
	uint32_t packets;
	struct dma_fence *last;
	struct rt_surface_copy_stats *stats;
	/* What every job of the batch waits for (not referenced here). */
	struct dma_fence *deps[RT_SURFACE_ENGINES_MAX];
};

static int batch_submit(struct copy_batch *b)
{
	struct amdgpu_ring *ring = b->ring ? b->ring : b->adev->mman.buffer_funcs_ring;
	struct dma_fence *fence;

	if (!b->job)
		return 0;
	amdgpu_ring_pad_ib(ring, &b->job->ibs[0]);
	fence = amdgpu_job_submit(b->job);
	b->job = NULL;
	b->packets = 0;
	dma_fence_put(b->last);
	b->last = fence;
	b->stats->jobs++;
	return 0;
}

/* A job with room for @dwords more (and the padding): the current one, or
 * a new one once it is full. */
static int batch_room(struct copy_batch *b, uint32_t dwords)
{
	int r;

	if (b->job && (b->packets == COPY_PACKETS_PER_JOB ||
		       b->job->ibs[0].length_dw + dwords + 8 > COPY_JOB_DW))
		batch_submit(b);
	if (b->job)
		return 0;
	r = amdgpu_job_alloc_with_ib(b->adev, b->entity ? b->entity : &b->adev->mman.default_entity.base,
				     AMDGPU_FENCE_OWNER_UNDEFINED, COPY_JOB_DW * 4, AMDGPU_IB_POOL_DELAYED,
				     &b->job, AMDGPU_KERNEL_JOB_ID_TTM_COPY_BUFFER);
	if (r) {
		b->job = NULL;
		return r;
	}
	for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++) {
		if (!b->deps[i])
			continue;
		/* The reference is the job's, even on failure. */
		r = drm_sched_job_add_dependency(&b->job->base, dma_fence_get(b->deps[i]));
		if (r) {
			amdgpu_job_free(b->job);
			b->job = NULL;
			return r;
		}
	}
	return 0;
}

static int batch_copy(struct copy_batch *b, uint64_t src, uint64_t dst, uint64_t bytes)
{
	const struct amdgpu_buffer_funcs *funcs = b->adev->mman.buffer_funcs;
	int r;

	while (bytes) {
		uint32_t chunk = bytes > funcs->copy_max_bytes ? funcs->copy_max_bytes : (uint32_t)bytes;

		r = batch_room(b, funcs->copy_num_dw);
		if (r)
			return r;
		amdgpu_emit_copy_buffer(b->adev, &b->job->ibs[0], src, dst, chunk, 0);
		b->stats->bytes += chunk;
		src += chunk;
		dst += chunk;
		bytes -= chunk;
		b->packets++;
	}
	return 0;
}

/* Whether the engines take sub-window copies of @w x @h pixels between
 * pitches @src_pitch and @dst_pitch (bytes) at these addresses: SDMA 7.0
 * and later (the packet layout this emits; earlier engines get a copy per
 * row), within the packet's fields. */
static bool subwindow_fits(struct amdgpu_device *adev, uint64_t src, uint32_t src_pitch, uint64_t dst,
			   uint32_t dst_pitch, uint32_t w, uint32_t h)
{
	return amdgpu_ip_version(adev, SDMA0_HWIP, 0) >= IP_VERSION(7, 0, 0) && !(src & 3) && !(dst & 3) &&
	       !(src_pitch & 3) && !(dst_pitch & 3) && w && h && w <= SUBWIN_DIM_MAX && h <= SUBWIN_DIM_MAX &&
	       src_pitch / 4 <= SUBWIN_PITCH_MAX && dst_pitch / 4 <= SUBWIN_PITCH_MAX && w <= src_pitch / 4 &&
	       w <= dst_pitch / 4;
}

/* @w x @h pixels (4 bytes) from @src to @dst, rows @src_pitch and @dst_pitch
 * bytes apart, in one sub-window packet (subwindow_fits). */
static int batch_rect(struct copy_batch *b, uint64_t src, uint32_t src_pitch, uint64_t dst,
		      uint32_t dst_pitch, uint32_t w, uint32_t h)
{
	struct amdgpu_ib *ib;
	int r = batch_room(b, SUBWIN_DW);

	if (r)
		return r;
	ib = &b->job->ibs[0];
	ib->ptr[ib->length_dw++] = SDMA_PKT_COPY_LINEAR_SUBWIN_HEADER_OP(SDMA_OP_COPY) |
				   SDMA_PKT_COPY_LINEAR_SUBWIN_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR_SUB_WIND) |
				   SDMA_PKT_COPY_LINEAR_SUBWIN_HEADER_ELEMENTSIZE(2);
	ib->ptr[ib->length_dw++] = lower_32_bits(src);
	ib->ptr[ib->length_dw++] = upper_32_bits(src);
	ib->ptr[ib->length_dw++] = 0;					/* x, y */
	ib->ptr[ib->length_dw++] = (src_pitch / 4 - 1) << 16;		/* z, pitch - 1 */
	ib->ptr[ib->length_dw++] = src_pitch / 4 * h - 1;		/* slice pitch - 1 */
	ib->ptr[ib->length_dw++] = lower_32_bits(dst);
	ib->ptr[ib->length_dw++] = upper_32_bits(dst);
	ib->ptr[ib->length_dw++] = 0;
	ib->ptr[ib->length_dw++] = (dst_pitch / 4 - 1) << 16;
	ib->ptr[ib->length_dw++] = dst_pitch / 4 * h - 1;
	ib->ptr[ib->length_dw++] = (w - 1) | (h - 1) << 16;		/* width - 1, height - 1 */
	ib->ptr[ib->length_dw++] = 0;					/* depth - 1, cache policies */
	b->stats->bytes += (uint64_t)w * 4 * h;
	b->packets++;
	return 0;
}

/* A rectangle of @w x @h pixels: one sub-window packet when the engine
 * takes it, else a range per row (whole rows of equal pitch as one range). */
static int batch_pixels(struct copy_batch *b, uint64_t src, uint32_t src_pitch, uint64_t dst,
			uint32_t dst_pitch, uint32_t w, uint32_t h, uint32_t frame_width)
{
	int r = 0;

	if (w == frame_width && src_pitch == dst_pitch)
		return batch_copy(b, src, dst, (uint64_t)(h - 1) * src_pitch + (uint64_t)w * 4);
	if (subwindow_fits(b->adev, src, src_pitch, dst, dst_pitch, w, h))
		return batch_rect(b, src, src_pitch, dst, dst_pitch, w, h);
	for (uint32_t row = 0; !r && row < h; row++)
		r = batch_copy(b, src + (uint64_t)row * src_pitch, dst + (uint64_t)row * dst_pitch, (uint64_t)w * 4);
	return r;
}

int rt_surface_copy(struct rt_surface *src, struct drm_gem_object *dst, uint32_t dst_pitch,
		    const struct rt_surface_rect *rects, uint32_t count, unsigned int timeout_ms,
		    struct rt_surface_copy_stats *stats)
{
	struct rt_surface_copy_stats local = { 0 };
	struct copy_batch batch = { 0 };
	struct dma_fence *last = NULL;
	struct amdgpu_device *adev;
	struct amdgpu_bo *bo;
	uint64_t dst_address;
	u64 start;
	long waited;
	int r;

	if (!stats)
		stats = &local;
	memset(stats, 0, sizeof(*stats));
	if (!src || !dst || !rects || !count || dst_pitch < (uint64_t)src->width * 4 ||
	    dst->size < (uint64_t)dst_pitch * src->height)
		return -EINVAL;
	bo = gem_to_amdgpu_bo(dst);
	adev = amdgpu_ttm_adev(bo->tbo.bdev);
	if (rt_removal_active(adev) || !adev->mman.buffer_funcs_enabled || !adev->mman.buffer_funcs_ring ||
	    !adev->mman.buffer_funcs_ring->sched.ready)
		return -ENODEV;

	r = amdgpu_bo_reserve(bo, false);
	if (r)
		return r;
	r = amdgpu_bo_pin(bo, AMDGPU_GEM_DOMAIN_VRAM);
	if (!r)
		dst_address = amdgpu_bo_gpu_offset(bo);
	amdgpu_bo_unreserve(bo);
	if (r)
		return r;

	start = ktime_get_ns();
	batch.adev = adev;
	batch.stats = stats;
	mutex_lock(&adev->mman.default_entity.lock);
	for (uint32_t i = 0; !r && i < count; i++) {
		const struct rt_surface_rect *rect = &rects[i];
		uint32_t x = rect->x, y = rect->y, w, h;

		if (x >= src->width || y >= src->height || !rect->width || !rect->height)
			continue;
		w = min_t(u32, rect->width, src->width - x);
		h = min_t(u32, rect->height, src->height - y);
		stats->rows += h;
		if (!x && w == src->width && src->pitch == dst_pitch) {
			/* Whole rows with equal pitches: one range. */
			r = batch_copy(&batch, src->gpu_address + (u64)y * src->pitch,
				       dst_address + (u64)y * dst_pitch,
				       (u64)(h - 1) * src->pitch + (u64)w * 4);
			continue;
		}
		for (uint32_t row = y; !r && row < y + h; row++)
			r = batch_copy(&batch, src->gpu_address + (u64)row * src->pitch + (u64)x * 4,
				       dst_address + (u64)row * dst_pitch + (u64)x * 4, (u64)w * 4);
	}
	if (r && batch.job) {
		/* Nothing of a failed batch reaches the GPU. */
		amdgpu_job_free(batch.job);
		batch.job = NULL;
	}
	if (!r)
		batch_submit(&batch);
	mutex_unlock(&adev->mman.default_entity.lock);
	last = batch.last;

	/* Jobs on one entity run in order: the last fence covers them all.
	 * Both buffers carry it, so neither can move or be destroyed (and the
	 * platform mapping released) before the copies finished. */
	if (last) {
		struct amdgpu_bo *src_bo = gem_to_amdgpu_bo(src->obj);

		if (!amdgpu_bo_reserve(src_bo, true)) {
			amdgpu_bo_fence(src_bo, last, true);
			amdgpu_bo_unreserve(src_bo);
		} else {
			dma_fence_wait(last, false);
		}
		if (!amdgpu_bo_reserve(bo, true)) {
			amdgpu_bo_fence(bo, last, false);
			amdgpu_bo_unreserve(bo);
		} else {
			dma_fence_wait(last, false);
		}
	}
	if (last) {
		waited = dma_fence_wait_timeout(last, false, msecs_to_jiffies(timeout_ms));
		if (!r)
			r = waited < 0 ? (int)waited : waited == 0 ? -ETIME : 0;
		if (!r && last->error)
			r = last->error;
		dma_fence_put(last);
	}
	stats->ns = ktime_get_ns() - start;

	if (!amdgpu_bo_reserve(bo, true)) {
		amdgpu_bo_unpin(bo);
		amdgpu_bo_unreserve(bo);
	}
	return r;
}

/* ---- pinning check ---- */

int rt_surface_verify(struct rt_surface *surface, uint32_t seed, const void *cpu_view,
		      unsigned int timeout_ms, struct rt_surface_verify_result *result)
{
	struct rt_surface_copy_stats stats = { 0 };
	struct copy_batch batch = { 0 };
	struct amdgpu_device *adev;
	struct amdgpu_bo *bo = NULL;
	uint64_t gpu = 0, offsets[RT_SURFACE_SAMPLES];
	uint32_t *read = NULL;
	u64 start;
	long waited;
	int r = 0;

	if (!surface || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	result->version = 1;
	result->samples = RT_SURFACE_SAMPLES;
	result->sample_bytes = RT_SURFACE_SAMPLE_BYTES;
	result->first_gpu_mismatch = result->first_cpu_mismatch = UINT64_MAX;
	result->gpu_address = surface->gpu_address;
	if (surface->size < RT_SURFACE_SAMPLE_BYTES)
		return -EINVAL;
	/* First and last bytes, and evenly between, 256-byte aligned. */
	for (uint32_t i = 0; i < RT_SURFACE_SAMPLES; i++)
		offsets[i] = ((surface->size - RT_SURFACE_SAMPLE_BYTES) * i / (RT_SURFACE_SAMPLES - 1)) &
			     ~(uint64_t)(RT_SURFACE_SAMPLE_BYTES - 1);

	adev = amdgpu_ttm_adev(gem_to_amdgpu_bo(surface->obj)->tbo.bdev);
	if (rt_removal_active(adev) || !adev->mman.buffer_funcs_enabled || !adev->mman.buffer_funcs_ring ||
	    !adev->mman.buffer_funcs_ring->sched.ready)
		return -ENODEV;
	r = amdgpu_bo_create_kernel(adev, RT_SURFACE_SAMPLES * RT_SURFACE_SAMPLE_BYTES, PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_GTT, &bo, &gpu, (void **)&read);
	if (r)
		return r;
	memset(read, 0, RT_SURFACE_SAMPLES * RT_SURFACE_SAMPLE_BYTES);

	start = ktime_get_ns();
	batch.adev = adev;
	batch.stats = &stats;
	mutex_lock(&adev->mman.default_entity.lock);
	for (uint32_t i = 0; !r && i < RT_SURFACE_SAMPLES; i++)
		r = batch_copy(&batch, surface->gpu_address + offsets[i],
			       gpu + (uint64_t)i * RT_SURFACE_SAMPLE_BYTES, RT_SURFACE_SAMPLE_BYTES);
	if (r && batch.job) {
		amdgpu_job_free(batch.job);
		batch.job = NULL;
	}
	if (!r)
		batch_submit(&batch);
	mutex_unlock(&adev->mman.default_entity.lock);
	if (batch.last) {
		struct amdgpu_bo *src_bo = gem_to_amdgpu_bo(surface->obj);

		if (!amdgpu_bo_reserve(src_bo, true)) {
			amdgpu_bo_fence(src_bo, batch.last, true);
			amdgpu_bo_unreserve(src_bo);
		}
		waited = dma_fence_wait_timeout(batch.last, false, msecs_to_jiffies(timeout_ms));
		if (!r)
			r = waited < 0 ? (int)waited : waited == 0 ? -ETIME : 0;
		if (!r && batch.last->error)
			r = batch.last->error;
		if (r == -ETIME) {
			/* The copy may still write the buffer: keep it until it does. */
			amdgpu_bo_reserve(bo, true);
			amdgpu_bo_fence(bo, batch.last, false);
			amdgpu_bo_unreserve(bo);
		}
		dma_fence_put(batch.last);
	}
	result->gpu_ns = ktime_get_ns() - start;

	if (!r) {
		for (uint32_t i = 0; i < RT_SURFACE_SAMPLES; i++)
			for (uint32_t d = 0; d < RT_SURFACE_SAMPLE_BYTES / 4; d++) {
				uint64_t at = offsets[i] + (uint64_t)d * 4;
				uint32_t want = rt_surface_pattern(seed, at / 4);
				uint32_t got = read[i * (RT_SURFACE_SAMPLE_BYTES / 4) + d];

				if (!i && !d) {
					result->gpu_value = got;
					result->expected_value = want;
				}
				if (got != want) {
					if (!result->gpu_mismatches++) {
						result->first_gpu_mismatch = at;
						result->gpu_value = got;
						result->expected_value = want;
					}
				}
				if (cpu_view) {
					uint32_t cpu = ((const uint32_t *)cpu_view)[at / 4];

					if (cpu != want && !result->cpu_mismatches++)
						result->first_cpu_mismatch = at;
				}
			}
		result->cpu_checked = cpu_view != NULL;
	}
	amdgpu_bo_free_kernel(&bo, &gpu, (void **)&read);
	return r;
}

/* ---- imports by owner ---- */

static DEFINE_MUTEX(surface_table_lock);
static struct {
	uint64_t owner;
	uint32_t handle;
	struct rt_surface *surface;
} surface_table[RT_SURFACE_IMPORTS_MAX];
static uint32_t surface_next_handle = 1;

uint32_t rt_surface_add(uint64_t owner, struct rt_surface *surface)
{
	uint32_t handle = 0;

	if (!surface)
		return 0;
	mutex_lock(&surface_table_lock);
	for (uint32_t i = 0; i < RT_SURFACE_IMPORTS_MAX; i++) {
		if (surface_table[i].surface)
			continue;
		handle = surface_next_handle++;
		if (!surface_next_handle)
			surface_next_handle = 1;
		surface_table[i].owner = owner;
		surface_table[i].handle = handle;
		surface_table[i].surface = surface;
		break;
	}
	mutex_unlock(&surface_table_lock);
	return handle;
}

struct rt_surface *rt_surface_get(uint64_t owner, uint32_t handle)
{
	struct rt_surface *found = NULL;

	mutex_lock(&surface_table_lock);
	for (uint32_t i = 0; handle && i < RT_SURFACE_IMPORTS_MAX; i++)
		if (surface_table[i].surface && surface_table[i].owner == owner &&
		    surface_table[i].handle == handle)
			found = surface_table[i].surface;
	mutex_unlock(&surface_table_lock);
	return found;
}

struct rt_surface *rt_surface_get_hold(uint64_t owner, uint32_t handle)
{
	struct rt_surface *found = NULL;

	mutex_lock(&surface_table_lock);
	for (uint32_t i = 0; handle && i < RT_SURFACE_IMPORTS_MAX; i++)
		if (surface_table[i].surface && surface_table[i].owner == owner &&
		    surface_table[i].handle == handle)
			found = surface_table[i].surface;
	rt_surface_hold(found);
	mutex_unlock(&surface_table_lock);
	return found;
}

void *rt_surface_provider_context(const struct rt_surface *surface)
{
	return surface ? surface->provider_context : NULL;
}

/* Take matching entries out under the lock, release them outside it (the
 * release takes upstream locks). */
static unsigned int surface_remove_matching(bool all, uint64_t owner, uint32_t handle)
{
	struct rt_surface *taken[RT_SURFACE_IMPORTS_MAX];
	unsigned int n = 0;

	mutex_lock(&surface_table_lock);
	for (uint32_t i = 0; i < RT_SURFACE_IMPORTS_MAX; i++) {
		if (!surface_table[i].surface)
			continue;
		if (!all && (surface_table[i].owner != owner ||
			     (handle && surface_table[i].handle != handle)))
			continue;
		taken[n++] = surface_table[i].surface;
		surface_table[i].surface = NULL;
	}
	mutex_unlock(&surface_table_lock);
	for (unsigned int i = 0; i < n; i++)
		rt_surface_release(taken[i]);
	return n;
}

int rt_surface_remove(uint64_t owner, uint32_t handle)
{
	if (!handle)
		return -ENOENT;
	return surface_remove_matching(false, owner, handle) ? 0 : -ENOENT;
}

unsigned int rt_surface_remove_owner(uint64_t owner)
{
	return surface_remove_matching(false, owner, 0);
}

unsigned int rt_surface_remove_all(void)
{
	return surface_remove_matching(true, 0, 0);
}

unsigned int rt_surface_count(void)
{
	unsigned int n = 0;

	mutex_lock(&surface_table_lock);
	for (uint32_t i = 0; i < RT_SURFACE_IMPORTS_MAX; i++)
		n += surface_table[i].surface != NULL;
	mutex_unlock(&surface_table_lock);
	return n;
}

/* ---- asynchronous copies on engines the caller owns ---- */

int rt_surface_engines_init(struct amdgpu_device *adev, struct rt_surface_engines *e)
{
	memset(e, 0, sizeof(*e));
	for (int i = 0; i < adev->sdma.num_instances && e->count < RT_SURFACE_ENGINES_MAX; i++) {
		struct amdgpu_ring *ring = &adev->sdma.instance[i].ring;
		struct drm_gpu_scheduler *sched = &ring->sched;
		struct drm_sched_entity *entity;

		if (!ring->sched.ready || !adev->mman.buffer_funcs)
			continue;
		entity = kzalloc(sizeof(*entity), GFP_KERNEL);
		if (!entity)
			break;
		if (drm_sched_entity_init(entity, DRM_SCHED_PRIORITY_NORMAL, &sched, 1, NULL)) {
			kfree(entity);
			continue;
		}
		e->entity[e->count] = entity;
		e->ring[e->count] = ring;
		e->count++;
	}
	return e->count ? 0 : -ENODEV;
}

void rt_surface_engines_fini(struct rt_surface_engines *e)
{
	for (unsigned int i = 0; i < e->count; i++) {
		drm_sched_entity_destroy(e->entity[i]);
		kfree(e->entity[i]);
	}
	memset(e, 0, sizeof(*e));
}

/* A rectangle's bytes, clipped to the surface. */
static uint64_t rect_bytes(const struct rt_surface *src, uint32_t dst_pitch,
			   const struct rt_surface_rect *r, uint32_t *x, uint32_t *y,
			   uint32_t *w, uint32_t *h)
{
	if (r->x >= src->width || r->y >= src->height || !r->width || !r->height)
		return 0;
	*x = r->x;
	*y = r->y;
	*w = min_t(u32, r->width, src->width - r->x);
	*h = min_t(u32, r->height, src->height - r->y);
	(void)dst_pitch;
	return (uint64_t)*w * 4 * *h;
}

/* @r inside a frame of @width x @height, and its source rows too. */
static bool vram_copy_valid(const struct rt_surface_vram_copy *c, uint32_t width, uint32_t height)
{
	const struct rt_surface_rect *r = &c->dst;

	return r->width && r->height && r->x < width && r->width <= width - r->x &&
	       r->y < height && r->height <= height - r->y && c->src_y < height &&
	       r->height <= height - c->src_y;
}

int rt_surface_frame_submit(struct rt_surface *src, struct drm_gem_object *dst, uint64_t dst_address,
			    uint32_t dst_pitch, const struct rt_surface_frame_plan *plan,
			    struct rt_surface_engines *engines,
			    struct dma_fence *fences[RT_SURFACE_ENGINES_MAX],
			    struct rt_surface_copy_stats *stats)
{
	struct copy_batch batch[RT_SURFACE_ENGINES_MAX] = { 0 };
	struct rt_surface_copy_stats local[RT_SURFACE_ENGINES_MAX] = { 0 };
	struct amdgpu_device *adev;
	struct amdgpu_bo *dst_bo, *src_bo, *prev_bo = NULL;
	struct dma_fence *vram_done = NULL;
	uint64_t total = 0, half;
	unsigned int use, e = 0;
	u64 start = ktime_get_ns();
	int r = 0;

	for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++)
		fences[i] = NULL;
	if (stats)
		memset(stats, 0, sizeof(*stats));
	if (!src || !dst || !plan || (plan->count && !plan->rects) || !engines || !engines->count ||
	    dst_pitch < (uint64_t)src->width * 4 || dst->size < (uint64_t)dst_pitch * src->height)
		return -EINVAL;
	if (plan->vram_count) {
		if (!plan->vram || !plan->prev || plan->prev == dst || !plan->prev_address ||
		    plan->prev->size < (uint64_t)dst_pitch * src->height)
			return -EINVAL;
		for (uint32_t i = 0; i < plan->vram_count; i++)
			if (!vram_copy_valid(&plan->vram[i], src->width, src->height))
				return -EINVAL;
		prev_bo = gem_to_amdgpu_bo(plan->prev);
	}
	dst_bo = gem_to_amdgpu_bo(dst);
	src_bo = gem_to_amdgpu_bo(src->obj);
	adev = amdgpu_ttm_adev(dst_bo->tbo.bdev);
	if (rt_removal_active(adev))
		return -ENODEV;
	for (uint32_t i = 0; i < plan->count; i++) {
		uint32_t x, y, w, h;

		total += rect_bytes(src, dst_pitch, &plan->rects[i], &x, &y, &w, &h);
	}
	if (!total && !plan->vram_count)
		return 0;
	/* Large damage goes to both engines, about half each. */
	use = total >= (1u << 20) ? engines->count : 1;
	half = (total + use - 1) / use;
	for (unsigned int i = 0; i < use; i++) {
		batch[i].adev = adev;
		batch[i].entity = engines->entity[i];
		batch[i].ring = engines->ring[i];
		batch[i].stats = &local[i];
	}
	/* From the previous framebuffer, on the first engine, once the
	 * copies that drew it are done. Full-width rows are one range. */
	for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++)
		batch[0].deps[i] = plan->vram_count ? plan->prev_fences[i] : NULL;
	for (uint32_t i = 0; !r && i < plan->vram_count; i++) {
		const struct rt_surface_rect *d = &plan->vram[i].dst;
		const uint64_t before = local[0].bytes;

		if (stats)
			stats->rows += d->height;
		r = batch_pixels(&batch[0], plan->prev_address + (u64)plan->vram[i].src_y * dst_pitch + (u64)d->x * 4,
				 dst_pitch, dst_address + (u64)d->y * dst_pitch + (u64)d->x * 4, dst_pitch,
				 d->width, d->height, src->width);
		local[0].vram_bytes += local[0].bytes - before;
		local[0].bytes = before;
	}
	if (!r && plan->vram_count) {
		/* Its own job: the surface's copies on another engine wait for
		 * it, as they may write over what it wrote. */
		batch_submit(&batch[0]);
		vram_done = batch[0].last ? dma_fence_get(batch[0].last) : NULL;
		for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++)
			batch[0].deps[i] = NULL;
		for (unsigned int i = 1; i < use; i++)
			batch[i].deps[0] = vram_done;
	}
	for (uint32_t i = 0; !r && i < plan->count; i++) {
		uint32_t x, y, w, h;

		if (!rect_bytes(src, dst_pitch, &plan->rects[i], &x, &y, &w, &h))
			continue;
		if (stats)
			stats->rows += h;
		for (uint32_t row = y; !r && row < y + h;) {
			uint32_t rows = y + h - row;

			if (e + 1 < use && local[e].bytes >= half)
				e++;
			if (e + 1 < use) {
				/* This engine takes rows up to about its half. */
				const uint64_t room = half - local[e].bytes;

				rows = (uint32_t)min_t(u64, rows, room / ((u64)w * 4) + 1);
			}
			r = batch_pixels(&batch[e], src->gpu_address + (u64)row * src->pitch + (u64)x * 4, src->pitch,
					 dst_address + (u64)row * dst_pitch + (u64)x * 4, dst_pitch, w, rows, src->width);
			row += rows;
		}
	}
	for (unsigned int i = 0; i < use; i++) {
		if (r && batch[i].job) {
			amdgpu_job_free(batch[i].job);
			batch[i].job = NULL;
		} else if (!r) {
			batch_submit(&batch[i]);
		}
		if (batch[i].last) {
			/* Neither buffer moves or goes before the copy finished, and
			 * a commit of @dst waits for it (an implicit in-fence). */
			if (!amdgpu_bo_reserve(src_bo, true)) {
				amdgpu_bo_fence(src_bo, batch[i].last, true);
				amdgpu_bo_unreserve(src_bo);
			} else {
				dma_fence_wait(batch[i].last, false);
			}
			if (!amdgpu_bo_reserve(dst_bo, true)) {
				amdgpu_bo_fence(dst_bo, batch[i].last, false);
				amdgpu_bo_unreserve(dst_bo);
			} else {
				dma_fence_wait(batch[i].last, false);
			}
			fences[i] = batch[i].last;
		}
		if (stats) {
			stats->jobs += local[i].jobs;
			stats->bytes += local[i].bytes;
			stats->vram_bytes += local[i].vram_bytes;
		}
	}
	if (vram_done) {
		if (prev_bo && !amdgpu_bo_reserve(prev_bo, true)) {
			amdgpu_bo_fence(prev_bo, vram_done, true);
			amdgpu_bo_unreserve(prev_bo);
		} else {
			dma_fence_wait(vram_done, false);
		}
		dma_fence_put(vram_done);
	}
	if (stats)
		stats->ns = ktime_get_ns() - start;
	return r;
}

int rt_surface_copy_submit(struct rt_surface *src, struct drm_gem_object *dst, uint64_t dst_address,
			   uint32_t dst_pitch, const struct rt_surface_rect *rects, uint32_t count,
			   struct rt_surface_engines *engines,
			   struct dma_fence *fences[RT_SURFACE_ENGINES_MAX],
			   struct rt_surface_copy_stats *stats)
{
	const struct rt_surface_frame_plan plan = { .rects = rects, .count = count };

	if (!rects || !count) {
		for (unsigned int i = 0; i < RT_SURFACE_ENGINES_MAX; i++)
			fences[i] = NULL;
		return -EINVAL;
	}
	return rt_surface_frame_submit(src, dst, dst_address, dst_pitch, &plan, engines, fences, stats);
}
