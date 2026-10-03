/* Finish only proven TTM owners left by a failed GMC software init.
 * The upstream IP unwinder skips GMC until its sw_init succeeds, although
 * amdgpu_ttm_init has already acquired TTM's global dummy page at that point.
 * Call before devres frees the containing DRM device. No hardware is touched. */
#include "amdgpu.h"
#include <drm/drm_drv.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_range_manager.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <rt/ttm_cleanup.h>

static bool pristine_list(const struct list_head *head)
{
	return head->next == head && head->prev == head;
}

static bool idle_manager(const struct ttm_resource_manager *manager,
			 const struct ttm_device *bdev, u64 usage)
{
	if (manager->bdev != bdev || manager->usage != usage)
		return false;
	for (unsigned i = 0; i < TTM_MAX_BO_PRIORITY; i++)
		if (!pristine_list(&manager->lru[i])) return false;
	for (unsigned i = 0; i < TTM_NUM_MOVE_FENCES; i++)
		if (manager->eviction_fences[i]) return false;
	return true;
}

/* Every node must be one of the independently established owners. Never
 * follow a foreign node while checking the list, including malformed cycles. */
static bool exact_list(const struct list_head *head,
		       const struct list_head *const *nodes, unsigned count)
{
	const struct list_head *prev = head, *next = head->next;
	unsigned seen = 0;
	for (unsigned n = 0; n < count; n++) {
		unsigned i;
		for (i = 0; i < count; i++) if (nodes[i] == next) break;
		if (i == count || (seen & (1u << i)) || next->prev != prev)
			return false;
		seen |= 1u << i;
		prev = next;
		next = next->next;
	}
	return next == head && head->prev == prev;
}

struct partial_bo {
	struct amdgpu_bo **owner;
	void **cpu;
	unsigned type;
};

static bool range_managers_proven(struct amdgpu_device *adev)
{
	const unsigned types[] = {AMDGPU_PL_GDS, AMDGPU_PL_GWS, AMDGPU_PL_OA,
		AMDGPU_PL_DOORBELL, AMDGPU_PL_MMIO_REMAP};
	const u64 sizes[] = {adev->gds.gds_size, adev->gds.gws_size, adev->gds.oa_size,
		adev->doorbell.size / PAGE_SIZE, 1};
	struct ttm_device *bdev = &adev->mman.bdev;
	const struct ttm_resource_manager_func *ops;
	bool present = false, valid = true;
	for (unsigned i = 0; i < ARRAY_SIZE(types); i++)
		present |= bdev->man_drv[types[i]] != NULL;
	if (!present) return true;
	/* The canonical operations table and private layout are file-local in
	 * upstream TTM. Learn its identity from an empty ordinary manager, rather
	 * than casting an unproven manager to a copied private struct layout. */
	struct ttm_device *probe = kzalloc(sizeof(*probe), GFP_KERNEL);
	if (!probe) return false;
	spin_lock_init(&probe->lru_lock);
	if (ttm_range_man_init(probe, AMDGPU_PL_DOORBELL, false, 1)) {
		kfree(probe);
		return false;
	}
	ops = probe->man_drv[AMDGPU_PL_DOORBELL]->func;
	if (ttm_range_man_fini(probe, AMDGPU_PL_DOORBELL))
		return false; /* Preserve the unexpected owner rather than freeing it. */
	kfree(probe);
	for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
		struct ttm_resource_manager *man = bdev->man_drv[types[i]];
		if (man && (man->func != ops || man->use_tt || man->size != sizes[i]))
			valid = false;
	}
	return valid;
}

static bool exclusive_bo(struct amdgpu_device *adev, const struct partial_bo *entry)
{
	struct amdgpu_bo *bo = *entry->owner;
	struct ttm_buffer_object *tbo = &bo->tbo;
	struct drm_gem_object *gem = &tbo->base;
	struct dma_resv_iter cursor;
	bool empty;

	if (!amdgpu_bo_is_amdgpu_bo(tbo) || tbo->bdev != &adev->mman.bdev ||
	    gem->dev != &adev->ddev || kref_read(&gem->refcount) != 1 ||
	    kref_read(&tbo->kref) != 1 || tbo->type != ttm_bo_type_kernel ||
	    tbo->pin_count != 1 || tbo->deleted || tbo->bulk_move || tbo->ttm ||
	    tbo->sg || bo->parent || bo->vm_bo || bo->kfd_bo || gem->handle_count ||
	    gem->name || gem->dma_buf || gem->import_attach || gem->filp ||
	    gem->resv != &gem->_resv ||
	    (bo->flags & AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE) ||
	    !tbo->resource || tbo->resource->bo != tbo ||
	    tbo->resource->mem_type != entry->type ||
	    tbo->resource->size != gem->size || !gem->size ||
	    tbo->resource->lru.type != TTM_LRU_RESOURCE ||
	    (bo->kmap.bo && (bo->kmap.bo != tbo || !entry->cpu ||
			    *entry->cpu != bo->kmap.virtual)) ||
	    (entry->cpu && *entry->cpu && !bo->kmap.bo))
		return false;
#ifdef CONFIG_MMU_NOTIFIER
	if (bo->notifier.mm) return false;
#endif
	if (!dma_resv_trylock(gem->resv)) return false;
	dma_resv_iter_begin(&cursor, gem->resv, DMA_RESV_USAGE_BOOKKEEP);
	empty = dma_resv_iter_first(&cursor) == NULL &&
		dma_resv_test_signaled(gem->resv, DMA_RESV_USAGE_BOOKKEEP);
	dma_resv_iter_end(&cursor);
	dma_resv_unlock(gem->resv);
	return empty;
}

/* GMC has not published a successful software stage, so these are its only
 * admitted BO owners. In particular a later GART/SDMA or shared BO cannot be
 * inferred safe from a signaled fence or from the absence of user clients. */
static int cleanup_managers(struct amdgpu_device *adev)
{
	struct ttm_device *bdev = &adev->mman.bdev;
	struct amdgpu_vram_mgr *vram = &adev->mman.vram_mgr;
	struct partial_bo bos[AMDGPU_RESV_MAX + 1];
	const struct list_head *pinned[ARRAY_SIZE(bos)], *vres[ARRAY_SIZE(bos)];
	u64 usage[TTM_NUM_MEM_TYPES] = {0};
	unsigned count = 0, vcount = 0;
	bool have_vram = bdev->man_drv[TTM_PL_VRAM] != NULL;
	bool have_gtt = bdev->man_drv[TTM_PL_TT] != NULL;

	if (adev->mman.buffer_funcs_enabled || adev->mman.clear_entities ||
	    adev->mman.num_clear_entities || adev->mman.num_move_entities ||
	    adev->mman.sdma_access_bo || adev->mman.sdma_access_ptr ||
	    adev->rmmio_remap.bo || adev->in_suspend)
		return -EBUSY;
	if (have_vram && bdev->man_drv[TTM_PL_VRAM] != &vram->manager)
		return -EBUSY;
	if (have_gtt && (bdev->man_drv[TTM_PL_TT] != &adev->mman.gtt_mgr.manager ||
			!drm_mm_initialized(&adev->mman.gtt_mgr.mm) ||
			!drm_mm_clean(&adev->mman.gtt_mgr.mm)))
		return -EBUSY;
	if (bdev->man_drv[AMDGPU_PL_PREEMPT] &&
	    bdev->man_drv[AMDGPU_PL_PREEMPT] != &adev->mman.preempt_mgr)
		return -EBUSY;
	for (unsigned i = 0; i < TTM_NUM_MEM_TYPES; i++) {
		switch (i) {
		case TTM_PL_SYSTEM: case TTM_PL_VRAM: case TTM_PL_TT:
		case AMDGPU_PL_PREEMPT: case AMDGPU_PL_GDS: case AMDGPU_PL_GWS:
		case AMDGPU_PL_OA: case AMDGPU_PL_DOORBELL: case AMDGPU_PL_MMIO_REMAP:
			break;
		default:
			if (bdev->man_drv[i]) return -EBUSY;
		}
	}
	if (!range_managers_proven(adev)) return -EBUSY;
	if (!pristine_list(&vram->reservations_pending) ||
	    !pristine_list(&vram->reserved_pages)) return -EBUSY;
	for (unsigned i = 0; i < AMDGPU_RESV_MAX; i++) {
		struct amdgpu_vram_resv *r = &adev->mman.resv_region[i];
		if (!r->bo) {
			if (r->cpu_ptr) return -EBUSY;
			continue;
		}
		if (!have_vram || !r->size || r->size > U64_MAX - (PAGE_SIZE - 1) ||
		    r->bo->tbo.base.size != PAGE_ALIGN(r->size) ||
		    (!r->needs_cpu_map && (r->cpu_ptr || r->bo->kmap.bo))) return -EBUSY;
		bos[count++] = (struct partial_bo){&r->bo,
			r->needs_cpu_map ? &r->cpu_ptr : NULL, TTM_PL_VRAM};
	}
	if (adev->doorbell.kernel_doorbells)
		bos[count++] = (struct partial_bo){&adev->doorbell.kernel_doorbells,
			(void **)&adev->doorbell.cpu_addr, AMDGPU_PL_DOORBELL};
	else if (adev->doorbell.cpu_addr) return -EBUSY;
	if (atomic_read(&ttm_glob.bo_count) != count) return -EBUSY;
	for (unsigned i = 0; i < count; i++) {
		struct amdgpu_bo *bo = *bos[i].owner;
		for (unsigned j = 0; j < i; j++)
			if (*bos[j].owner == bo) return -EBUSY;
		if (!exclusive_bo(adev, &bos[i]) || !bdev->man_drv[bos[i].type] ||
		    usage[bos[i].type] > U64_MAX - bo->tbo.base.size) return -EBUSY;
		usage[bos[i].type] += bo->tbo.base.size;
		pinned[i] = &bo->tbo.resource->lru.link;
		if (bos[i].type == TTM_PL_VRAM)
			vres[vcount++] = &to_amdgpu_vram_mgr_resource(bo->tbo.resource)->vres_node;
	}
	if (!exact_list(&bdev->unevictable, pinned, count) ||
	    !exact_list(&vram->allocated_vres_list, vres, vcount)) return -EBUSY;
	for (unsigned i = 0; i < TTM_NUM_MEM_TYPES; i++) {
		struct ttm_resource_manager *man = bdev->man_drv[i];
		if (man && (!idle_manager(man, bdev, usage[i]) ||
			    (i != TTM_PL_SYSTEM && !man->use_type))) return -EBUSY;
	}
	if (have_vram) {
		if (vram->mm.size != vram->manager.size ||
		    usage[TTM_PL_VRAM] > vram->mm.size ||
		    vram->mm.avail != vram->mm.size - usage[TTM_PL_VRAM]) return -EBUSY;
	} else if (count || vram->manager.use_type ||
		   !idle_manager(&vram->manager, bdev, 0)) return -EBUSY;

	/* All checks precede the first ownership mutation. The failed probe is
	 * exclusive; no client or successful GMC stage has published these BOs. */
	for (unsigned i = 0; i < count; i++)
		amdgpu_bo_free_kernel(bos[i].owner, NULL, bos[i].cpu);
	if (atomic_read(&ttm_glob.bo_count) || !pristine_list(&bdev->unevictable) ||
	    !pristine_list(&vram->allocated_vres_list)) return -EBUSY;
	for (unsigned i = 0; i < TTM_NUM_MEM_TYPES; i++)
		if (bdev->man_drv[i] && !idle_manager(bdev->man_drv[i], bdev, 0))
			return -EBUSY;
	if (have_vram && vram->mm.avail != vram->mm.size) return -EBUSY;
	memset(adev->mman.resv_region, 0, sizeof(adev->mman.resv_region));
	adev->psp.mem_train_ctx.init = PSP_MEM_TRAIN_NOT_SUPPORT;
	if (adev->mman.aper_base_kaddr) {
		iounmap(adev->mman.aper_base_kaddr);
		adev->mman.aper_base_kaddr = NULL;
	}
	/* Only registered managers have completed init. PREEMPT's failed sysfs
	 * creation leaves an initialized empty struct but installs no manager. */
	if (have_vram) amdgpu_vram_mgr_fini(adev);
	if (have_gtt) amdgpu_gtt_mgr_fini(adev);
	if (bdev->man_drv[AMDGPU_PL_PREEMPT]) amdgpu_preempt_mgr_fini(adev);
	const unsigned ranges[] = {AMDGPU_PL_GDS, AMDGPU_PL_GWS, AMDGPU_PL_OA,
		AMDGPU_PL_DOORBELL, AMDGPU_PL_MMIO_REMAP};
	for (unsigned i = 0; i < ARRAY_SIZE(ranges); i++)
		if (bdev->man_drv[ranges[i]] && ttm_range_man_fini(bdev, ranges[i]))
			return -EBUSY;
	for (unsigned i = 0; i < TTM_NUM_MEM_TYPES; i++)
		if (i != TTM_PL_SYSTEM && bdev->man_drv[i]) return -EBUSY;
	return 0;
}

int rt_amdgpu_cleanup_failed_probe(struct pci_dev *pdev)
{
	struct drm_device *ddev;
	struct amdgpu_device *adev;
	struct ttm_device *bdev;
	struct amdgpu_ip_block *gmc = NULL;

	if (!pdev)
		return 0;
	/* Identify the AMDGPU binding by its driver, not by a device ID: the
	 * same partial-TTM state can be left by any ASIC's failed GMC init. */
	ddev = pci_get_drvdata(pdev);
	/* The upstream descriptor is file-local. Its exported release callback
	 * identifies the AMDGPU container before drm_to_adev can inspect it. */
	if (!ddev || ddev->dev != &pdev->dev || !ddev->driver ||
	    ddev->driver->release != amdgpu_driver_release_kms ||
	    !ddev->driver->name || strcmp(ddev->driver->name, "amdgpu"))
		return 0;
	adev = drm_to_adev(ddev);
	if (!adev->mman.initialized) return 0;
	if (adev->pdev != pdev || adev->dev != &pdev->dev ||
	    (adev->flags & AMD_IS_APU) || adev->gmc.is_app_apu ||
	    adev->mman.ttm_pools ||
	    adev->num_ip_blocks <= 0 ||
	    (unsigned)adev->num_ip_blocks > ARRAY_SIZE(adev->ip_blocks))
		return -EBUSY;
	for (int i = 0; i < adev->num_ip_blocks; i++) {
		struct amdgpu_ip_block *ip = &adev->ip_blocks[i];
		if (ip->status.hw || ip->status.late_initialized) return -EBUSY;
		if (!ip->version || ip->version->type != AMD_IP_BLOCK_TYPE_GMC)
			continue;
		if (gmc) return -EBUSY;
		gmc = ip;
	}
	if (!gmc || gmc->status.sw || gmc->status.hw || gmc->status.late_initialized)
		return -EBUSY;
	bdev = &adev->mman.bdev;
	if (!bdev->wq || bdev->pool.dev != &pdev->dev ||
	    bdev->man_drv[TTM_PL_SYSTEM] != &bdev->sysman ||
	    !idle_manager(&bdev->sysman, bdev, 0) ||
	    !ttm_glob.dummy_read_page || page_ref_count(ttm_glob.dummy_read_page) != 1)
		return -EBUSY;
	/* A second TTM device could own global pools independently of this probe. */
	if (ttm_glob.device_list.next != &bdev->device_list ||
	    ttm_glob.device_list.prev != &bdev->device_list ||
	    bdev->device_list.next != &ttm_glob.device_list ||
	    bdev->device_list.prev != &ttm_glob.device_list)
		return -EBUSY;
	for (unsigned cache = 0; cache < TTM_NUM_CACHING_TYPES; cache++) {
		for (unsigned order = 0; order < NR_PAGE_ORDERS; order++) {
			struct ttm_pool_type *pool = &bdev->pool.caching[cache].orders[order];
			struct list_lru *pages = &pool->pages;
			if ((pool->pool && pool->pool != &bdev->pool) || pages->nr_items ||
			    (pool->pool && !pages->node) ||
			    (pages->node && (pages->node->nr_items ||
					    !pristine_list(&pages->node->list))))
				return -EBUSY;
		}
	}
	if (cleanup_managers(adev)) return -EBUSY;
	/* No manager/BO can reference the dummy page. Use TTM's normal release,
	 * which drains its workqueue and relinquishes the page through put_page.
	 * The DriverKit DMA hold still owns its backing until endpoint isolation. */
	ttm_device_fini(bdev);
	adev->mman.initialized = false;
	dev_info(adev->dev, "released proven partial TTM owners after failed GMC init\n");
	return 1;
}
