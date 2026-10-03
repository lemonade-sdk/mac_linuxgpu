/* GEM mappings of Linux-file clients (lx_internal.h): what backs an amdgpu
 * buffer a client mmaps through its render node.
 *
 * Linux maps a GEM buffer lazily: each CPU fault reserves the buffer, moves
 * a VRAM buffer into the CPU-visible part of VRAM if needed
 * (amdgpu_bo_fault_reserve_notify) and inserts the page or BAR PFN, and a
 * later move zaps the mapping so the next access faults again. A macOS
 * client mapping can be neither faulted nor revoked, so the buffer is
 * placed as the fault path would place it and pinned there until the
 * client unmaps it. */
#include <stdint.h>

#include <linux/errno.h>
#include <linux/mm.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_tt.h>
#include <rt/lx_files.h>

#include "amdgpu.h"
#include "amdgpu_object.h"
#include "amdgpu_res_cursor.h"
#include "lx_internal.h"

int rt_lx_gem_map(struct drm_device *ddev, struct pci_dev *pdev,
		  struct vm_area_struct *vma, uint64_t length, void **pinned,
		  uint32_t *backing, uint32_t *cache,
		  int (*add)(void *arg, uint32_t bar, uint64_t addr, uint64_t bytes),
		  void *arg)
{
	/* drm_gem_mmap -> ttm_bo_mmap_obj left the buffer here, referenced. */
	struct ttm_buffer_object *tbo = vma->vm_private_data;
	struct amdgpu_device *adev = drm_to_adev(ddev);
	struct amdgpu_bo *abo;
	struct ttm_resource *res;
	uint64_t first;
	int r;

	if (!tbo || tbo->bdev != &adev->mman.bdev)
		return -EINVAL;
	abo = ttm_to_amdgpu_bo(tbo);
	first = (uint64_t)(vma->vm_pgoff - drm_vma_node_start(&tbo->base.vma_node)) << PAGE_SHIFT;
	if (first > amdgpu_bo_size(abo) || length > amdgpu_bo_size(abo) - first)
		return -EINVAL;
	r = amdgpu_bo_reserve(abo, false);
	if (r)
		return r;
	res = abo->tbo.resource;
	if (res && res->mem_type == TTM_PL_VRAM && amdgpu_bo_fault_reserve_notify(tbo)) {
		amdgpu_bo_unreserve(abo);
		return -EFAULT;
	}
	res = abo->tbo.resource;
	if (!res) {
		amdgpu_bo_unreserve(abo);
		return -EFAULT;
	}
	r = amdgpu_bo_pin(abo, res->mem_type == TTM_PL_VRAM ? AMDGPU_GEM_DOMAIN_VRAM :
			  AMDGPU_GEM_DOMAIN_GTT);
	if (r) {
		amdgpu_bo_unreserve(abo);
		return r;
	}
	res = abo->tbo.resource;
	if (res->mem_type == TTM_PL_VRAM) {
		struct amdgpu_res_cursor cursor;

		*backing = RT_LX_RANGE_BAR;
		*cache = RT_LX_CACHE_WRITE_COMBINE;
		amdgpu_res_first(res, first, length, &cursor);
		while (!r && cursor.remaining) {
			uint32_t bar;
			uint64_t offset;

			r = rt_lx_bar_of(pdev, adev->gmc.aper_base + cursor.start, cursor.size,
					 &bar, &offset);
			if (!r)
				r = add(arg, bar, offset, cursor.size);
			amdgpu_res_next(&cursor, cursor.size);
		}
	} else {
		struct ttm_tt *ttm = abo->tbo.ttm;

		*backing = RT_LX_RANGE_CPU;
		*cache = (abo->flags & AMDGPU_GEM_CREATE_CPU_GTT_USWC) ?
			RT_LX_CACHE_WRITE_COMBINE : RT_LX_CACHE_DEFAULT;
		if (!ttm || !ttm->pages || !ttm_tt_is_populated(ttm))
			r = -EFAULT;
		for (uint64_t off = first; !r && off < first + length; off += PAGE_SIZE) {
			struct page *page = ttm->pages[off >> PAGE_SHIFT];
			void *cpu = page ? page_address(page) : NULL;

			r = cpu ? add(arg, 0, (uint64_t)(uintptr_t)cpu, PAGE_SIZE) : -EFAULT;
		}
	}
	if (r) {
		amdgpu_bo_unpin(abo);
	} else {
		/* The pin's own reference keeps the buffer past the VMA's. */
		drm_gem_object_get(&abo->tbo.base);
		*pinned = abo;
	}
	amdgpu_bo_unreserve(abo);
	return r;
}

void rt_lx_gem_unpin(void *pinned)
{
	struct amdgpu_bo *abo = pinned;

	if (!abo)
		return;
	if (!amdgpu_bo_reserve(abo, true)) {
		amdgpu_bo_unpin(abo);
		amdgpu_bo_unreserve(abo);
	}
	drm_gem_object_put(&abo->tbo.base);
}
