/* Compute memory adapter over the probed upstream AMDGPU device. */
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <drm/amdgpu_drm.h>
#include <rt/compute.h>
#include <rt/rt.h>

#include "amdgpu.h"
#include "amdgpu_gart.h"
#include "amdgpu_object.h"
#include "amdgpu_ttm.h"
#include "amdgpu_vm.h"

struct rt_compute_bo {
	struct rt_compute_bo *next;
	struct amdgpu_bo *upstream;
	uint64_t size;
	uint64_t allocated_size;
	uint64_t gpu_address;
	dma_addr_t dma_address;
	void *cpu_address;
	enum rt_compute_domain domain;
	int coherent_bound;
};

struct rt_compute_ctx {
	struct pci_dev *pdev;
	struct amdgpu_device *adev;
	struct rt_compute_bo *bos;
	pthread_mutex_t lock;
	int host_memory_verified;
	int poisoned;
	struct dma_fence *uncertain_fence;
	uint64_t vram_pinned_at_open;
};

struct rt_compute_fence {
	struct dma_fence *upstream;
	int completion;
};

static int compute_ready(struct rt_compute_ctx *ctx)
{
	if (!ctx || !ctx->pdev || !ctx->adev ||
	    rt_pci_probe_result(ctx->pdev) != 0 ||
	    pci_get_drvdata(ctx->pdev) != &ctx->adev->ddev ||
	    ctx->adev->pdev != ctx->pdev ||
	    ctx->adev->shutdown || !ctx->adev->accel_working ||
	    !ctx->adev->mman.initialized || !ctx->adev->gart.ptr ||
	    !ctx->adev->gart.bo || !ctx->adev->gmc.gart_size ||
	    !ctx->adev->gmc.real_vram_size)
		return -ENODEV;
	return 0;
}

static int bo_belongs(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo)
{
	struct rt_compute_bo *cur;
	for (cur = ctx->bos; cur; cur = cur->next)
		if (cur == bo)
			return 1;
	return 0;
}

static int valid_range(const struct rt_compute_bo *bo,
		       uint64_t offset, uint64_t size)
{
	return offset <= bo->size && size <= bo->size - offset;
}

static int copy_ready(struct amdgpu_device *adev)
{
	return adev->mman.buffer_funcs_enabled &&
	       adev->mman.buffer_funcs && adev->mman.buffer_funcs_ring &&
	       adev->mman.buffer_funcs_ring->sched.ready;
}

int rt_compute_open(struct pci_dev *pdev, struct rt_compute_ctx **out)
{
	struct drm_device *ddev;
	struct rt_compute_ctx *ctx;
	int r;
	if (!out || !pdev)
		return -EINVAL;
	*out = NULL;
	if (rt_pci_probe_result(pdev) != 0)
		return -ENODEV;
	ddev = pci_get_drvdata(pdev);
	if (!ddev)
		return -ENODEV;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->pdev = pdev;
	ctx->adev = drm_to_adev(ddev);
	r = compute_ready(ctx);
	if (r) {
		free(ctx);
		return r;
	}
	if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
		free(ctx);
		return -ENOMEM;
	}
	ctx->vram_pinned_at_open = atomic64_read(&ctx->adev->vram_pin_size);
	*out = ctx;
	return 0;
}

int rt_compute_status(struct rt_compute_ctx *ctx)
{
	int r = compute_ready(ctx);
	if (r)
		return r;
	if (ctx->poisoned)
		return -EBUSY;
	return copy_ready(ctx->adev) &&
	       ctx->host_memory_verified ? 0 : -ENODEV;
}

struct amdgpu_device *rt_compute_device(struct rt_compute_ctx *ctx)
{
	return compute_ready(ctx) == 0 ? ctx->adev : NULL;
}

int rt_compute_properties(struct rt_compute_ctx *ctx,
			  struct rt_compute_properties *out)
{
	struct amdgpu_device *adev;
	if (!out || compute_ready(ctx) != 0)
		return -ENODEV;
	pthread_mutex_lock(&ctx->lock);
	adev = ctx->adev;
	memset(out, 0, sizeof(*out));
	out->vram_bytes = adev->gmc.real_vram_size;
	out->visible_vram_bytes = adev->gmc.visible_vram_size;
	out->vram_start = adev->gmc.vram_start;
	out->gart_start = adev->gmc.gart_start;
	out->gart_bytes = adev->gmc.gart_size;
	out->gfx_ip_version = amdgpu_ip_version(adev, GC_HWIP, 0);
	out->ip_versions[0] = amdgpu_ip_version(adev, GC_HWIP, 0);
	out->ip_versions[1] = amdgpu_ip_version(adev, SDMA0_HWIP, 0);
	out->ip_versions[2] = amdgpu_ip_version(adev, MMHUB_HWIP, 0);
	out->ip_versions[3] = amdgpu_ip_version(adev, MP0_HWIP, 0);
	out->pci_domain = ctx->pdev->domain;
	out->pci_bus = ctx->pdev->bus ? ctx->pdev->bus->number : 0;
	out->pci_device = ctx->pdev->devfn >> 3;
	out->pci_function = ctx->pdev->devfn & 7;
	out->pci_chip_id = ctx->pdev->device;
	out->pci_revision = ctx->pdev->revision;
	out->chip_revision = adev->rev_id;
	if (adev->asic_funcs && adev->asic_funcs->get_xclk)
		out->timestamp_frequency_hz =
			(uint64_t)amdgpu_asic_get_xclk(adev) * 10000ULL;
	out->shader_engines = adev->gfx.config.max_shader_engines;
	out->shader_arrays_per_engine = adev->gfx.config.max_sh_per_se;
	out->cu_count = adev->gfx.cu_info.number;
	out->simd_per_cu = adev->gfx.cu_info.simd_per_cu;
	out->max_waves_per_simd = adev->gfx.cu_info.max_waves_per_simd;
	out->wavefront_size = adev->gfx.cu_info.wave_front_size;
	out->scratch_slots_per_cu = adev->gfx.cu_info.max_scratch_slots_per_cu;
	memcpy(out->cu_bitmap, adev->gfx.cu_info.bitmap[0],
	       sizeof(out->cu_bitmap));
	out->gpu_copy_ready = copy_ready(adev);
	out->host_memory_verified = ctx->host_memory_verified;
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

int rt_compute_memory_usage(struct rt_compute_ctx *ctx,
			    struct rt_compute_memory_usage *out)
{
	struct amdgpu_device *adev;
	uint64_t total, visible, pinned, used, vis_used, usable;
	if (!ctx || !out || compute_ready(ctx) != 0)
		return -ENODEV;
	pthread_mutex_lock(&ctx->lock);
	if (ctx->poisoned || !ctx->host_memory_verified ||
	    !ttm_resource_manager_used(&ctx->adev->mman.vram_mgr.manager)) {
		pthread_mutex_unlock(&ctx->lock);
		return -ENODEV;
	}
	adev = ctx->adev;
	total = adev->gmc.real_vram_size;
	visible = adev->gmc.visible_vram_size;
	pinned = ctx->vram_pinned_at_open;
	used = ttm_resource_manager_usage(&adev->mman.vram_mgr.manager);
	vis_used = amdgpu_vram_mgr_vis_usage(&adev->mman.vram_mgr);
	if (!total || !visible || visible > total ||
	    pinned > total || AMDGPU_VM_RESERVED_VRAM > total - pinned ||
	    used > total || vis_used > visible) {
		pthread_mutex_unlock(&ctx->lock);
		return -ERANGE;
	}
	usable = total - pinned - AMDGPU_VM_RESERVED_VRAM;
	*out = (struct rt_compute_memory_usage){
		.total_bytes = total,
		.usable_bytes = usable,
		.used_bytes = used,
		.free_bytes = min(usable,
			used >= total - AMDGPU_VM_RESERVED_VRAM ? 0 :
			total - AMDGPU_VM_RESERVED_VRAM - used),
		.visible_bytes = visible,
		.visible_used_bytes = vis_used,
	};
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

static int bo_release(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo)
{
	if (bo->coherent_bound) {
		struct amdgpu_device *adev = ctx->adev;
		uint64_t gart_base = amdgpu_gmc_sign_extend(adev->gmc.gart_start);
		uint64_t offset = bo->gpu_address - gart_base;
		int r = amdgpu_bo_reserve(bo->upstream, false);
		if (r)
			return r;
		amdgpu_gart_unbind(ctx->adev, offset,
				   (int)(bo->allocated_size / PAGE_SIZE));
		amdgpu_bo_unreserve(bo->upstream);
		bo->coherent_bound = 0;
	}
	if (bo->upstream)
		amdgpu_bo_free_kernel(&bo->upstream, &bo->gpu_address, NULL);
	if (bo->cpu_address)
		dma_free_coherent(&ctx->pdev->dev, (size_t)bo->allocated_size,
				  bo->cpu_address, bo->dma_address);
	free(bo);
	return 0;
}

/* Reserve GPU MC space with a pinned upstream BO. Its TTM streaming pages
 * only reserve that aperture; the coherent allocation below replaces the
 * aperture PTEs and is the sole exported CPU/DMA backing. */
static int bind_coherent_gtt(struct rt_compute_ctx *ctx,
			     struct rt_compute_bo *bo)
{
	struct amdgpu_device *adev = ctx->adev;
	uint64_t gart_base = amdgpu_gmc_sign_extend(adev->gmc.gart_start);
	uint64_t offset;
	dma_addr_t *pages;
	uint64_t flags;
	size_t count;
	size_t i;
	if (bo->gpu_address < gart_base)
		return -ERANGE;
	if (!bo->upstream->tbo.resource ||
	    bo->upstream->tbo.resource->mem_type != TTM_PL_TT ||
	    !bo->upstream->tbo.ttm)
		return -EINVAL;
	offset = bo->gpu_address - gart_base;
	if (offset >= adev->gmc.gart_size ||
	    bo->allocated_size > adev->gmc.gart_size - offset ||
	    (offset & (PAGE_SIZE - 1)))
		return -ERANGE;
	count = (size_t)(bo->allocated_size / PAGE_SIZE);
	if (!count || count > INT_MAX || count > SIZE_MAX / sizeof(*pages))
		return -E2BIG;
	bo->cpu_address = dma_alloc_coherent(&ctx->pdev->dev,
			(size_t)bo->allocated_size, &bo->dma_address, GFP_KERNEL);
	if (!bo->cpu_address || dma_mapping_error(&ctx->pdev->dev, bo->dma_address)) {
		if (bo->cpu_address)
			dma_free_coherent(&ctx->pdev->dev,
					  (size_t)bo->allocated_size,
					  bo->cpu_address, bo->dma_address);
		bo->cpu_address = NULL;
		return -ENOMEM;
	}
	if (bo->dma_address > UINT64_MAX - bo->allocated_size)
		return -ERANGE;
	pages = calloc(count, sizeof(*pages));
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < count; ++i)
		pages[i] = bo->dma_address + i * PAGE_SIZE;
	flags = amdgpu_ttm_tt_pte_flags(adev, bo->upstream->tbo.ttm,
					 bo->upstream->tbo.resource);
	flags |= AMDGPU_PTE_VALID | AMDGPU_PTE_READABLE | AMDGPU_PTE_WRITEABLE;
	flags &= ~AMDGPU_PTE_SNOOPED;
	int reserved = amdgpu_bo_reserve(bo->upstream, false);
	if (reserved) {
		free(pages);
		return reserved;
	}
	amdgpu_gart_bind(adev, offset, (int)count, pages, flags);
	amdgpu_gart_invalidate_tlb(adev);
	amdgpu_bo_unreserve(bo->upstream);
	bo->coherent_bound = 1;
	free(pages);
	return 0;
}

int rt_compute_bo_alloc(struct rt_compute_ctx *ctx, uint64_t size,
			uint64_t alignment, enum rt_compute_domain domain,
			struct rt_compute_bo **out)
{
	struct rt_compute_bo *bo;
	u32 upstream_domain;
	int r;
	if (!out || !size || !ctx)
		return -EINVAL;
	*out = NULL;
	if (domain != RT_COMPUTE_GTT && domain != RT_COMPUTE_VRAM)
		return -EINVAL;
	if (!alignment)
		alignment = PAGE_SIZE;
	if (alignment < PAGE_SIZE)
		alignment = PAGE_SIZE;
	if ((alignment & (alignment - 1)) || alignment > INT_MAX ||
	    size > SIZE_MAX - (PAGE_SIZE - 1) ||
	    size > ULONG_MAX - (PAGE_SIZE - 1))
		return -EINVAL;
	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return -ENOMEM;
	bo->size = size;
	bo->allocated_size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	bo->domain = domain;
	upstream_domain = domain == RT_COMPUTE_GTT ?
		AMDGPU_GEM_DOMAIN_GTT : AMDGPU_GEM_DOMAIN_VRAM;
	pthread_mutex_lock(&ctx->lock);
	r = compute_ready(ctx);
	if (r)
		goto fail;
	if (ctx->poisoned) {
		r = -EBUSY;
		goto fail;
	}
	r = amdgpu_bo_create_kernel(ctx->adev, (unsigned long)bo->allocated_size,
				    (int)alignment, upstream_domain,
				    &bo->upstream, &bo->gpu_address, NULL);
	if (r)
		goto fail;
	if (domain == RT_COMPUTE_GTT) {
		r = bind_coherent_gtt(ctx, bo);
		if (r)
			goto fail;
	}
	bo->next = ctx->bos;
	ctx->bos = bo;
	pthread_mutex_unlock(&ctx->lock);
	*out = bo;
	return 0;
fail:
	if (bo_release(ctx, bo) != 0) {
		bo->next = ctx->bos;
		ctx->bos = bo;
		ctx->poisoned = 1;
	}
	pthread_mutex_unlock(&ctx->lock);
	return r;
}

int rt_compute_bo_info(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
			struct rt_compute_bo_info *out)
{
	if (!ctx || !out)
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	if (!bo_belongs(ctx, bo)) {
		pthread_mutex_unlock(&ctx->lock);
		return -ENOENT;
	}
	*out = (struct rt_compute_bo_info){
		.size = bo->size,
		.gpu_address = bo->gpu_address,
		.dma_address = bo->dma_address,
		.cpu_address = bo->cpu_address,
		.domain = bo->domain,
		.shared_descriptor_available = bo->coherent_bound &&
			ctx->host_memory_verified && !ctx->poisoned &&
			copy_ready(ctx->adev),
	};
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

static int bo_transfer(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
		       uint64_t offset, void *buffer, size_t size, int write)
{
	int r = 0;
	int vram = 0;
	if (!ctx || (!buffer && size))
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	if (!bo_belongs(ctx, bo))
		r = -ENOENT;
	else if (!valid_range(bo, offset, size))
		r = -ERANGE;
	else if (ctx->poisoned)
		r = -EBUSY;
	else if (!bo->coherent_bound && bo->domain == RT_COMPUTE_VRAM)
		vram = 1;
	else if (!bo->coherent_bound)
		r = -EOPNOTSUPP;
	else if (compute_ready(ctx) != 0)
		r = -ENODEV;
	else if (write)
		memcpy((uint8_t *)bo->cpu_address + offset, buffer, size);
	else
		memcpy(buffer, (uint8_t *)bo->cpu_address + offset, size);
	if (vram && (ctx->poisoned || !ctx->host_memory_verified ||
		     !copy_ready(ctx->adev)))
		r = -ENODEV;
	pthread_mutex_unlock(&ctx->lock);
	if (!vram || r || !size)
		return r;
	/* VRAM has no trustworthy dext CPU pointer. Stage through a coherent
	 * GTT BO and require the real SDMA fence to complete for every chunk. */
	for (size_t done = 0; done < size; ) {
		struct rt_compute_bo *staging = NULL;
		struct rt_compute_fence *fence = NULL;
		uint32_t chunk = size - done > 65536 ? 65536 :
			(uint32_t)(size - done);
		r = rt_compute_bo_alloc(ctx, chunk, PAGE_SIZE, RT_COMPUTE_GTT,
					&staging);
		if (r)
			break;
		if (write) {
			r = rt_compute_bo_write(ctx, staging, 0,
						(uint8_t *)buffer + done, chunk);
			if (!r)
				r = rt_compute_bo_copy(ctx, staging, bo, 0,
						       offset + done, chunk, &fence);
		} else {
			r = rt_compute_bo_copy(ctx, bo, staging,
					       offset + done, 0, chunk, &fence);
			if (!r)
				r = rt_compute_bo_read(ctx, staging, 0,
						(uint8_t *)buffer + done, chunk);
		}
		rt_compute_fence_put(fence);
		int free_r = rt_compute_bo_free(ctx, staging);
		if (!r)
			r = free_r;
		if (r)
			break;
		done += chunk;
	}
	return r;
}

int rt_compute_bo_read(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
			uint64_t offset, void *dst, size_t size)
{
	return bo_transfer(ctx, bo, offset, dst, size, 0);
}

int rt_compute_bo_write(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
			 uint64_t offset, const void *src, size_t size)
{
	return bo_transfer(ctx, bo, offset, (void *)src, size, 1);
}

int rt_compute_bo_copy(struct rt_compute_ctx *ctx, struct rt_compute_bo *src,
			struct rt_compute_bo *dst, uint64_t src_offset,
			uint64_t dst_offset, uint32_t size,
			struct rt_compute_fence **out_fence)
{
	struct rt_compute_fence *result;
	struct dma_fence *fence = NULL;
	int r;
	if (!ctx || !out_fence || !size)
		return -EINVAL;
	*out_fence = NULL;
	result = calloc(1, sizeof(*result));
	if (!result)
		return -ENOMEM;
	pthread_mutex_lock(&ctx->lock);
	if (!bo_belongs(ctx, src) || !bo_belongs(ctx, dst))
		r = -ENOENT;
	else if (!valid_range(src, src_offset, size) ||
		 !valid_range(dst, dst_offset, size))
		r = -ERANGE;
	else if (src == dst && src_offset < dst_offset + size &&
		 dst_offset < src_offset + size)
		r = -EINVAL;
	else if (src->gpu_address > UINT64_MAX - src_offset ||
		 dst->gpu_address > UINT64_MAX - dst_offset)
		r = -ERANGE;
	else if (ctx->poisoned)
		r = -EBUSY;
	else if (compute_ready(ctx) != 0 || !copy_ready(ctx->adev))
		r = -ENODEV;
	else {
		mutex_lock(&ctx->adev->mman.default_entity.lock);
		r = amdgpu_copy_buffer(ctx->adev, &ctx->adev->mman.default_entity,
				       src->gpu_address + src_offset,
				       dst->gpu_address + dst_offset, size,
				       NULL, &fence, false, 0);
		if (!r && fence) {
			long waited = dma_fence_wait_timeout(fence, false,
					       msecs_to_jiffies(5000));
			r = waited > 0 ? dma_fence_get_status(fence) :
			    (waited == 0 ? -ETIMEDOUT : (int)waited);
			if (r > 0)
				r = 0;
			else if (r == 0)
				r = -EIO;
		}
		else if (!r)
			r = -EIO;
		if (r) {
			ctx->poisoned = 1;
			ctx->host_memory_verified = 0;
			ctx->uncertain_fence = fence;
			fence = NULL;
		}
		mutex_unlock(&ctx->adev->mman.default_entity.lock);
	}
	pthread_mutex_unlock(&ctx->lock);
	if (r) {
		dma_fence_put(fence);
		free(result);
		return r;
	}
	result->upstream = fence;
	result->completion = 0;
	*out_fence = result;
	return 0;
}

int rt_compute_verify_host_memory(struct rt_compute_ctx *ctx)
{
	struct rt_compute_bo *source = NULL;
	struct rt_compute_bo *vram = NULL;
	struct rt_compute_bo *destination = NULL;
	struct rt_compute_fence *fence = NULL;
	uint8_t *src_ptr, *dst_ptr;
	int r;
	if (!ctx)
		return -EINVAL;
	ctx->host_memory_verified = 0;
	if (compute_ready(ctx) != 0 || !copy_ready(ctx->adev))
		return -ENODEV;
	r = rt_compute_bo_alloc(ctx, PAGE_SIZE, PAGE_SIZE, RT_COMPUTE_GTT,
				&source);
	if (r)
		goto out;
	r = rt_compute_bo_alloc(ctx, PAGE_SIZE, PAGE_SIZE, RT_COMPUTE_VRAM,
				&vram);
	if (r)
		goto out;
	r = rt_compute_bo_alloc(ctx, PAGE_SIZE, PAGE_SIZE, RT_COMPUTE_GTT,
				&destination);
	if (r)
		goto out;
	src_ptr = source->cpu_address;
	dst_ptr = destination->cpu_address;
	if (!src_ptr || !dst_ptr) {
		r = -EFAULT;
		goto out;
	}
	for (size_t i = 0; i < PAGE_SIZE; ++i)
		src_ptr[i] = (uint8_t)((i * 73U + 0x5bU) ^ (i >> 7));
	memset(dst_ptr, 0, PAGE_SIZE);
	r = rt_compute_bo_copy(ctx, source, vram, 0, 0, PAGE_SIZE, &fence);
	if (r)
		goto out;
	rt_compute_fence_put(fence);
	fence = NULL;
	r = rt_compute_bo_copy(ctx, vram, destination, 0, 0, PAGE_SIZE,
			       &fence);
	if (r)
		goto out;
	if (memcmp(src_ptr, dst_ptr, PAGE_SIZE) != 0)
		r = -EIO;
out:
	rt_compute_fence_put(fence);
	struct rt_compute_bo *temporary[] = { destination, vram, source };
	for (size_t i = 0; i < sizeof(temporary) / sizeof(temporary[0]); ++i) {
		if (!temporary[i])
			continue;
		int cleanup = rt_compute_bo_free(ctx, temporary[i]);
		if (!r)
			r = cleanup;
	}
	/* Readback is not sufficient if its DMA resources could not be released. */
	ctx->host_memory_verified = r == 0;
	return r;
}

int rt_compute_fence_wait(struct rt_compute_fence *fence)
{
	return fence ? fence->completion : -EINVAL;
}

void rt_compute_fence_put(struct rt_compute_fence *fence)
{
	if (!fence)
		return;
	dma_fence_put(fence->upstream);
	free(fence);
}

int rt_compute_bo_free(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo)
{
	struct rt_compute_bo **link;
	if (!ctx || !bo)
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	if (ctx->poisoned) {
		pthread_mutex_unlock(&ctx->lock);
		return -EBUSY;
	}
	for (link = &ctx->bos; *link && *link != bo; link = &(*link)->next)
		;
	if (!*link) {
		pthread_mutex_unlock(&ctx->lock);
		return -ENOENT;
	}
	*link = bo->next;
	int r = bo_release(ctx, bo);
	if (r) {
		bo->next = *link;
		*link = bo;
		pthread_mutex_unlock(&ctx->lock);
		return r;
	}
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

int rt_compute_close(struct rt_compute_ctx *ctx)
{
	if (!ctx)
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	if (ctx->poisoned) {
		pthread_mutex_unlock(&ctx->lock);
		return -EBUSY;
	}
	while (ctx->bos) {
		struct rt_compute_bo *bo = ctx->bos;
		struct rt_compute_bo *next = bo->next;
		int r = bo_release(ctx, bo);
		if (r) {
			pthread_mutex_unlock(&ctx->lock);
			return r;
		}
		ctx->bos = next;
	}
	pthread_mutex_unlock(&ctx->lock);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
	return 0;
}
