/* linuxu shim: ttm — the driver's own ttm/*.c set (ttm_bo.c,
 * ttm_bo_util.c, ttm_bo_vm.c, ttm_device.c, ttm_pool.c,
 * ttm_range_manager.c, ttm_tt.c — all in the 463-file driver build
 * set) provides every ttm_* symbol, so this shim keeps no ttm_*
 * definitions; the `linuxu_ttm_*` names below are shim-only
 * (nothing in driver/ or vendor/ uses them — verified by grep).
 * The linuxu_dma_ops table stays: kobject.c installs
 * it as struct device::dma_ops for the host dma_map_ops surface, and
 * the driver does not define it.
 *
 * W5 (GEM/TTM fault path): the in-process win.  In the real driver,
 * amdgpu_bo_mmap() maps the BO into the process and ttm_bo_vm.c's
 * fault handler resolves the first touches (pre-faulting up to
 * TTM_BO_VM_NUM_PREFAULT pages per fault).  In-process, the BO's
 * backing IS a host allocation, so "mmap" just returns a live pointer
 * and marks the BO mapped — a later fault is then a no-op
 * "already mapped" (linuxu_ttm_bo_shim_fault).  The shim primitive
 * (linuxu_ttm_bo_shim_mmap) exercises exactly that contract. */
#include <pthread.h>
#include <stdlib.h>

#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/scatterlist.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/mm.h>      /* full struct page (bo shim pages) */

struct page;

/* Coherent DMA always goes through the DART ownership table, including
 * allocations requested via device->dma_ops rather than the inline API. */
static void *linuxu_dma_alloc(struct device *dev, size_t size,
			      dma_addr_t *dma_handle, gfp_t flags,
			      unsigned long attrs)
{
	(void)attrs;
	return linuxu_dma_alloc_coherent(dev, size, dma_handle, flags);
}

static void linuxu_dma_free(struct device *dev, size_t size, void *vaddr,
			    dma_addr_t dma_handle, unsigned long attrs)
{
	(void)attrs;
	linuxu_dma_free_coherent(dev, size, vaddr, dma_handle);
}

static int linuxu_dma_get_sgtable(struct device *dev, struct sg_table *sgt,
				  void *vaddr, dma_addr_t dma_handle,
				  size_t size, unsigned long attrs)
{
	(void)attrs;
	(void)dev; (void)sgt; (void)vaddr; (void)dma_handle; (void)size;
	/* A CPU pointer is not a struct page and cannot be exported as an
	 * empty, supposedly valid scatterlist. No such export is implemented. */
	return -EOPNOTSUPP;
}

static dma_addr_t linuxu_dma_map_phys(struct device *dev, phys_addr_t phys,
				      size_t size, enum dma_data_direction dir,
				      unsigned long attrs)
{
	(void)attrs;
	(void)dev; (void)phys; (void)size; (void)dir;
	/* Host physical addresses require a platform DMA mapping. */
	return DMA_MAPPING_ERROR;
}

static void linuxu_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
				  size_t size, enum dma_data_direction dir,
				      unsigned long attrs)
{
	(void)attrs;
	(void)dev; (void)dma_handle; (void)size; (void)dir;
}

/* map_sg / unmap_sg: forwarded to the host DART layer
 * (linuxu/src/dart/dart.c defines linuxu_dma_map_sg/unmap_sg; the
 * dma_map_ops table passes (dev, sg, nents, dir) through verbatim;
 * the prototypes come from <linux/dma-mapping.h>)
 */

static int linuxu_dma_ops_map_sg(struct device *dev, struct scatterlist *sg,
				 int nents, enum dma_data_direction dir,
				 unsigned long attrs)
{
	(void)attrs;
	return linuxu_dma_map_sg(dev, sg, nents, dir);
}

static void linuxu_dma_ops_unmap_sg(struct device *dev, struct scatterlist *sg,
				    int nents, enum dma_data_direction dir,
				    unsigned long attrs)
{
	(void)attrs;
	linuxu_dma_unmap_sg(dev, sg, nents, dir);
}

const struct dma_map_ops linuxu_dma_ops = {
	.alloc = linuxu_dma_alloc,
	.free = linuxu_dma_free,
	.get_sgtable = linuxu_dma_get_sgtable,
	.map_phys = linuxu_dma_map_phys,
	.unmap_phys = linuxu_dma_unmap_phys,
	.map_sg = linuxu_dma_ops_map_sg,
	.unmap_sg = linuxu_dma_ops_unmap_sg,
};

/* ---- W5: BO mmap pre-fault (shim-only host primitives) ----
 *
 * struct ttm_buffer_object is carried in <drm/ttm/ttm_bo.h> by the
 * driver's ttm set; the shim BO below is the host-side stand-in the
 * test (and later the dext mmap path) uses: it owns the anon backing
 * pages plus the mmap/fault state.  All members are appended after
 * nothing — this is a standalone struct, not an extension of the
 * driver's ttm_buffer_object.
 */
struct linuxu_ttm_bo_shim {
	struct page *pages;    /* backing pages (kmalloc-backed host pages) */
	int npages;
	size_t size;           /* total backing bytes */
	void *vaddr;           /* live in-process pointer (NULL = unmapped) */
	void *backing;         /* owned independently of the mapping */
	unsigned long faults;  /* faults seen after mapping (no-ops) */
	pthread_mutex_t lock;  /* guards vaddr + faults */
};

/* create a BO with `nr_pages` anon backing pages (host heap) */
struct linuxu_ttm_bo_shim *linuxu_ttm_bo_shim_create(int nr_pages)
{
	struct linuxu_ttm_bo_shim *bo;
	size_t total;
	int i;

	if (nr_pages <= 0)
		return NULL;
	bo = kzalloc(sizeof(*bo), 0);
	if (!bo)
		return NULL;
	total = (size_t)nr_pages * PAGE_SIZE;
	bo->pages = kcalloc(nr_pages, sizeof(struct page), 0);
	if (!bo->pages) {
		kfree(bo);
		return NULL;
	}
	for (i = 0; i < nr_pages; i++)
		bo->pages[i].flags = 1; /* PAGE_SHIM_ANON: live host page */
	/* one contiguous anon region; the "pages" are its slices.
	 * NOTE: this struct page is shim bookkeeping only (the P0
	 * pattern: the arena slot is not the backing; page.c owns the
	 * real 16 KB slots).  It is intentionally NOT pool-allocated:
	 * the mmap target (vaddr) is the anon region below. */
	bo->vaddr = NULL; /* backing exists but is not yet mapped */
	bo->size = total;
	bo->npages = nr_pages;
	if (pthread_mutex_init(&bo->lock, NULL)) {
		kfree(bo->pages);
		kfree(bo);
		return NULL;
	}
	return bo;
}

/* pre-fault mmap: give the caller a live in-process pointer to the BO
 * backing and mark it mapped.  Returns 0 on success (the pointer comes
 * out in *out_vaddr); -17 (EEXIST) if already mapped (idempotent —
 * the same pointer is returned, no double-map). */
int linuxu_ttm_bo_shim_mmap(struct linuxu_ttm_bo_shim *bo, void **out_vaddr)
{
	void *v;

	if (!bo || !out_vaddr)
		return -22;
	pthread_mutex_lock(&bo->lock);
	if (bo->vaddr) {
		*out_vaddr = bo->vaddr; /* already mapped: same pointer */
		pthread_mutex_unlock(&bo->lock);
		return -17;
	}
	v = bo->backing;
	if (!v) {
		v = calloc(1, bo->size);
		if (!v) {
			pthread_mutex_unlock(&bo->lock);
			return -ENOMEM;
		}
		bo->backing = v;
	}
	bo->vaddr = v;
	*out_vaddr = v;
	pthread_mutex_unlock(&bo->lock);
	return 0;
}

/* the later fault: everything the BO mapped is resident in-process,
 * so a fault is a no-op "already mapped".  Returns 0 if mapped, -2 if
 * the BO was never mmap'd. */
int linuxu_ttm_bo_shim_fault(struct linuxu_ttm_bo_shim *bo)
{
	int ret;

	if (!bo)
		return -22;
	pthread_mutex_lock(&bo->lock);
	ret = bo->vaddr ? 0 : -2;
	if (bo->vaddr)
		bo->faults++;
	pthread_mutex_unlock(&bo->lock);
	return ret;
}

/* unmap: drop the in-process pointer (backing is released by
 * linuxu_ttm_bo_shim_destroy) */
void linuxu_ttm_bo_shim_unmap(struct linuxu_ttm_bo_shim *bo)
{
	if (!bo)
		return;
	pthread_mutex_lock(&bo->lock);
	bo->vaddr = NULL;
	pthread_mutex_unlock(&bo->lock);
}

/* teardown: release the backing pages (kmemcheck-clean) */
void linuxu_ttm_bo_shim_destroy(struct linuxu_ttm_bo_shim *bo)
{
	if (!bo)
		return;
	linuxu_ttm_bo_shim_unmap(bo);
	pthread_mutex_destroy(&bo->lock);
	free(bo->backing);
	kfree(bo->pages);
	kfree(bo);
}
