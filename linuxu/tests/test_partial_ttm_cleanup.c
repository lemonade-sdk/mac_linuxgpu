/* Unchanged upstream initialization/finalization bodies with memory-only
 * boundaries for unused BO callbacks, workqueues, debugfs and page pools. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <drm/ttm/ttm_range_manager.h>
#include <string.h>
#include "amdgpu.h"
#include <drm/drm_drv.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_tt.h>
#include <linux/debugfs.h>
#include <rt/dart.h>
#include <rt/ttm_cleanup.h>
#include "ttm_bo_internal.h"

extern void partial_ttm_bridge_start(void);
extern void partial_ttm_bridge_check(unsigned live, unsigned retired);
extern void partial_ttm_bridge_finish(void);

static DEFINE_MUTEX(ttm_global_mutex);
static unsigned ttm_glob_use_count;
struct ttm_global ttm_glob;
struct dentry *ttm_debugfs_root;
static struct workqueue_struct *test_workqueue;
static unsigned pools, pool_managers;
static const struct ttm_resource_manager_func ttm_sys_manager_func;
static const struct ttm_resource_manager_func amdgpu_gtt_mgr_func;
static const struct ttm_resource_manager_func amdgpu_preempt_mgr_func;
static const struct device_attribute dev_attr_mem_info_preempt_used;
static const struct drm_driver test_kms_driver = {
	.name = "amdgpu", .release = amdgpu_driver_release_kms,
};
extern int linuxu_module_init_gpu_buddy_module_init(void);
extern void linuxu_module_exit_gpu_buddy_module_exit(void);
static bool fail_vram = true;
static int fail_alloc = -1;
static unsigned bo_freed;

void *partial_malloc(size_t size)
{
	if (fail_alloc == 0) { fail_alloc = -1; return NULL; }
	if (fail_alloc > 0) fail_alloc--;
	return malloc(size);
}
static void forbidden(void) { assert(!"unexpected hardware, asynchronous BO or post-PREEMPT path"); }
void amdgpu_driver_release_kms(struct drm_device *dev) { (void)dev; forbidden(); }
static void foreign_release(struct drm_device *dev) { (void)dev; forbidden(); }
#define debugfs_create_dir(...) NULL
#define debugfs_remove(...) ((void)0)
#define debugfs_create_atomic_t(...) ((void)0)
#define drm_warn(...) forbidden()
#define ioremap_wc(...) NULL
#define amdgpu_ttm_set_buffer_funcs_status(...) ((void)0)
#define amdgpu_ttm_training_data_block_init(...) ((void)0)
#define amdgpu_ttm_alloc_mmio_remap_bo(...) 0

int si_meminfo(struct sysinfo *info) { memset(info, 0, sizeof(*info)); info->totalram = 1ul << 20; info->mem_unit = PAGE_SIZE; return 0; }
int ttm_pool_mgr_init(unsigned long count) { assert(count); pool_managers++; return 0; }
void ttm_pool_mgr_fini(void) { assert(pool_managers == 1); pool_managers--; }
void ttm_tt_mgr_init(unsigned long pages, unsigned long dma32) { assert(pages && dma32); }
unsigned long ttm_tt_pages_limit(void) { return (256ul << 20) / PAGE_SIZE; }
int amdgpu_gtt_size = -1;
void ttm_pool_init(struct ttm_pool *pool, struct device *dev, int nid, unsigned flags)
{ assert(!(flags & TTM_ALLOCATION_POOL_USE_DMA_ALLOC)); pool->dev = dev; pool->nid = nid; pool->alloc_flags = flags; pools++; }
void ttm_pool_fini(struct ttm_pool *pool) { assert(pool->dev && pools == 1); pools--; }
struct workqueue_struct *alloc_workqueue(const char *name, unsigned flags, int max_active)
{ (void)flags; assert(!strcmp(name, "ttm") && max_active == 16 && !test_workqueue); return test_workqueue = kmalloc(64, GFP_KERNEL); }
void drain_workqueue(struct workqueue_struct *queue) { assert(queue == test_workqueue); }
void destroy_workqueue(struct workqueue_struct *queue) { assert(queue == test_workqueue); kfree(queue); test_workqueue = NULL; }
int partial_gpu_buddy_init(struct gpu_buddy *mm, u64 size, u64 chunk)
{ assert(size == (32ull << 30) && chunk == PAGE_SIZE); return fail_vram ? -ENOSYS : gpu_buddy_init(mm, size, chunk); }
struct dmem_cgroup_region *drmm_cgroup_register_region(struct drm_device *dev, const char *name, u64 size)
{ assert(dev && !strcmp(name, "vram") && size == (32ull << 30)); return NULL; }
int device_create_file(struct device *dev, const struct device_attribute *attr)
{ assert(dev && attr == &dev_attr_mem_info_preempt_used); return -ENOSYS; }
void device_remove_file(struct device *dev, const struct device_attribute *attr)
{ (void)dev; (void)attr; forbidden(); }
void ttm_bo_put(struct ttm_buffer_object *bo);
void ttm_bo_tt_destroy(struct ttm_buffer_object *bo) { assert(!bo->ttm); }
static void amdgpu_bo_delete_mem_notify(struct ttm_buffer_object *bo);
static const struct ttm_device_funcs amdgpu_bo_driver = {
	.delete_mem_notify = amdgpu_bo_delete_mem_notify,
	.release_notify = amdgpu_bo_release_notify,
};
#define amdgpu_vm_bo_move(bo, mem, evict) assert(!(bo)->vm_bo && !(mem) && !(evict))
#define amdgpu_amdkfd_remove_all_eviction_fences(bo) assert(!(bo)->kfd_bo)
#define amdgpu_amdkfd_release_notify(bo) forbidden()
#define amdgpu_fill_buffer(...) (forbidden(), -EINVAL)
#define trace_amdgpu_bo_move(...) ((void)0)
#define dma_buf_invalidate_mappings(...) forbidden()
void drm_gem_object_release(struct drm_gem_object *obj)
{ assert(!obj->filp && !obj->import_attach && !obj->dma_buf); dma_resv_fini(&obj->_resv); bo_freed++; }
void drm_prime_gem_destroy(struct drm_gem_object *obj, struct sg_table *sg)
{ (void)obj; (void)sg; forbidden(); }
void drm_vma_offset_remove(struct drm_vma_offset_manager *mgr, struct drm_vma_offset_node *node)
{ (void)mgr; (void)node; }
void vunmap(const void *ptr) { (void)ptr; forbidden(); }
void dmem_cgroup_uncharge(struct dmem_cgroup_pool_state *pool, u64 size)
{ (void)pool; (void)size; forbidden(); }
int dmem_cgroup_try_charge(struct dmem_cgroup_region *region, u64 size,
	struct dmem_cgroup_pool_state **pool, struct dmem_cgroup_pool_state **limit)
{ (void)region; (void)size; (void)pool; (void)limit; forbidden(); return -EINVAL; }
static int ttm_bo_evict(struct ttm_buffer_object *bo, struct ttm_operation_ctx *ctx)
{ (void)bo; (void)ctx; forbidden(); return -EINVAL; }
int ttm_bo_wait_ctx(struct ttm_buffer_object *bo, struct ttm_operation_ctx *ctx)
{ (void)bo; (void)ctx; forbidden(); return -EINVAL; }
bool drm_dev_enter(struct drm_device *dev, int *idx) { (void)dev; (void)idx; forbidden(); return false; }
void drm_dev_exit(int idx) { (void)idx; forbidden(); }
void dma_buf_unpin(struct dma_buf_attachment *attach) { (void)attach; forbidden(); }
void __drm_err(const char *fmt, ...) { assert(strstr(fmt, "Failed to create device file")); }
void drm_printf(struct drm_printer *p, const char *fmt, ...) { (void)p; (void)fmt; }
void drm_buddy_print(struct gpu_buddy *mm, struct drm_printer *p) { (void)mm; (void)p; }
bool queue_work_node(int node, struct workqueue_struct *wq, struct work_struct *work)
{ (void)node; (void)wq; (void)work; forbidden(); return false; }
static void amdgpu_bo_destroy(struct ttm_buffer_object *bo);
static void amdgpu_gem_object_free(struct drm_gem_object *gem);
void mmu_interval_notifier_remove(struct mmu_interval_notifier *notifier) { (void)notifier; forbidden(); }
static const struct drm_gem_object_funcs gem_funcs = {.free = amdgpu_gem_object_free};
static int make_bo(struct amdgpu_device *adev, unsigned long size, unsigned type,
	unsigned long offset, struct amdgpu_bo **out, void **cpu)
{
	struct amdgpu_bo *bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	assert(bo && !*out);
	bo->tbo.base.dev = &adev->ddev; bo->tbo.base.size = PAGE_ALIGN(size);
	bo->tbo.base.funcs = &gem_funcs;
	kref_init(&bo->tbo.base.refcount); kref_init(&bo->tbo.kref);
	dma_resv_init(&bo->tbo.base._resv); bo->tbo.base.resv = &bo->tbo.base._resv;
	bo->tbo.bdev = &adev->mman.bdev; bo->tbo.type = ttm_bo_type_kernel;
	bo->tbo.destroy = amdgpu_bo_destroy; bo->tbo.priority = 2;
	struct ttm_place place = {.mem_type = type, .fpfn = offset / PAGE_SIZE,
		.lpfn = type == TTM_PL_VRAM ? (offset + PAGE_ALIGN(size)) / PAGE_SIZE : 0};
	struct ttm_resource_manager *man = bo->tbo.bdev->man_drv[type];
	assert(man && dma_resv_lock(bo->tbo.base.resv, NULL) == 0);
	int alloc_ret = ttm_resource_alloc(&bo->tbo, &place, &bo->tbo.resource, NULL);
	if (alloc_ret) fprintf(stderr, "fixture BO allocation type=%u size=%lu offset=%lu ret=%d\n", type, size, offset, alloc_ret);
	assert(alloc_ret == 0);
	ttm_bo_pin(&bo->tbo);
	dma_resv_unlock(bo->tbo.base.resv);
	if (cpu) {
		bo->kmap.virtual = (void *)(uintptr_t)(0x10000000 + offset);
		bo->kmap.bo = &bo->tbo; bo->kmap.bo_kmap_type = ttm_bo_map_premapped;
		*cpu = bo->kmap.virtual;
	}
	if (type == TTM_PL_VRAM) {
		atomic64_add(bo->tbo.base.size, &adev->vram_pin_size);
		atomic64_add(amdgpu_vram_mgr_bo_visible_size(bo), &adev->visible_pin_size);
	}
	atomic_inc(&ttm_glob.bo_count); *out = bo; return 0;
}
int amdgpu_bo_create_kernel_at(struct amdgpu_device *adev, uint64_t offset,
	uint64_t size, struct amdgpu_bo **out, void **cpu)
{ return make_bo(adev, size, TTM_PL_VRAM, offset, out, cpu); }
int amdgpu_bo_create_kernel(struct amdgpu_device *adev, unsigned long size, int align,
	u32 domain, struct amdgpu_bo **out, u64 *gpu, void **cpu)
{ assert(domain == AMDGPU_GEM_DOMAIN_DOORBELL && align == PAGE_SIZE && !gpu); return make_bo(adev, size, AMDGPU_PL_DOORBELL, 0, out, cpu); }
static void amdgpu_ttm_init_vram_resv_regions(struct amdgpu_device *adev)
{
	adev->mman.resv_region[AMDGPU_RESV_STOLEN_VGA] = (struct amdgpu_vram_resv){.offset = 0, .size = 4 * PAGE_SIZE, .needs_cpu_map = true};
	adev->mman.resv_region[AMDGPU_RESV_FW] = (struct amdgpu_vram_resv){.offset = 31ull << 30, .size = 8 * PAGE_SIZE};
}

#include "upstream_partial_ttm.inc"

static struct amdgpu_device *new_failed_device(struct pci_dev *pdev)
{
	static const struct amdgpu_ip_block_version gmc = {.type = AMD_IP_BLOCK_TYPE_GMC};
	struct amdgpu_device *adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	assert(adev);
	memset(pdev, 0, sizeof(*pdev)); pdev->vendor = 0x1002; pdev->device = 0x744c;
	adev->dev = &pdev->dev; adev->pdev = pdev;
	adev->ddev.dev = &pdev->dev; adev->ddev.driver = &test_kms_driver;
	adev->ddev.anon_inode = kzalloc(sizeof(*adev->ddev.anon_inode), GFP_KERNEL);
	adev->ddev.vma_offset_manager = kzalloc(sizeof(*adev->ddev.vma_offset_manager), GFP_KERNEL);
	assert(adev->ddev.anon_inode && adev->ddev.vma_offset_manager);
	adev->num_ip_blocks = 1; adev->ip_blocks[0].adev = adev; adev->ip_blocks[0].version = &gmc;
	adev->gmc.real_vram_size = adev->gmc.mc_vram_size = 32ull << 30;
	adev->gmc.num_mem_partitions = 1; adev->gmc.gart_size = 512ul << 20;
	adev->gmc.visible_vram_size = 256ul << 20;
	adev->doorbell.size = 2ul << 20; adev->doorbell.num_kernel_doorbells = 4096;
	pci_set_drvdata(pdev, &adev->ddev);
	partial_ttm_bridge_start();
	assert(amdgpu_ttm_init(adev) == -ENOSYS);
	assert(adev->mman.initialized && !adev->ip_blocks[0].status.sw);
	assert(ttm_glob_use_count == 1 && ttm_glob.dummy_read_page && pools == 1 && pool_managers == 1);
	partial_ttm_bridge_check(1, 0);
	return adev;
}

static void release_device(struct amdgpu_device *adev)
{
	assert(!ttm_glob_use_count && !ttm_glob.dummy_read_page && !pools && !pool_managers);
	assert(!test_workqueue && !linuxu_dart_used());
	partial_ttm_bridge_check(0, 1);
	partial_ttm_bridge_finish();
	kfree(adev->ddev.vma_offset_manager); kfree(adev->ddev.anon_inode); kfree(adev);
}

static void later_refusals(void)
{
	static const struct amdgpu_ip_block_version other_ip = {.type = AMD_IP_BLOCK_TYPE_GFX};
	static const struct ttm_resource_manager_func foreign_ops;
	for (unsigned condition = 0; condition < 28; condition++) {
		struct pci_dev pdev;
		struct amdgpu_device *adev = new_failed_device(&pdev);
		struct ttm_device *bdev = &adev->mman.bdev;
		struct amdgpu_bo *bo = adev->mman.resv_region[0].bo;
		struct amdgpu_bo *other = adev->mman.resv_region[AMDGPU_RESV_FW].bo;
		struct ttm_resource_manager *door = bdev->man_drv[AMDGPU_PL_DOORBELL];
		const struct ttm_resource_manager_func *door_ops = door->func;
		struct list_head foreign_node;
		struct drm_mm_node gart_node = {0};
		unsigned before = bo_freed;
		void *cpu = adev->mman.resv_region[0].cpu_ptr;
		switch (condition) {
		case 0: bo->tbo.pin_count++; break;
		case 1: kref_get(&bo->tbo.base.refcount); break;
		case 2: kref_get(&bo->tbo.kref); break;
		case 3: bo->tbo.base.resv = &other->tbo.base._resv; break;
		case 4: bo->flags |= AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE; break;
		case 5: bo->vm_bo = (void *)1; break;
		case 6: bo->tbo.ttm = (void *)1; break;
		case 7: bo->tbo.base.import_attach = (void *)1; break;
		case 8: bo->tbo.base.handle_count = 1; break;
		case 9: adev->mman.resv_region[AMDGPU_RESV_FW].bo = bo; break;
		case 10: adev->mman.vram_mgr.manager.usage += PAGE_SIZE; break;
		case 11: list_add(&foreign_node, &bdev->unevictable); break;
		case 12: adev->mman.vram_mgr.mm.avail -= PAGE_SIZE; break;
		case 13: door->func = &foreign_ops; break;
		case 14: adev->num_ip_blocks = 2; adev->ip_blocks[1].version = &other_ip; adev->ip_blocks[1].status.hw = true; break;
		case 15: adev->mman.resv_region[0].cpu_ptr = (void *)1; break;
		case 16: bdev->pool.caching[0].orders[0].pages.nr_items = 1; break;
		case 17: assert(!drm_mm_insert_node(&adev->mman.gtt_mgr.mm, &gart_node, 1)); break;
		case 18: assert(dma_resv_trylock(bo->tbo.base.resv)); break;
		case 19: bo->tbo.base._resv.allocation_failed = true; break;
		case 20: fail_alloc = 0; break; /* private canonical bdev allocation */
		case 21: fail_alloc = 1; break; /* private canonical range allocation */
		case 22: door->size++; break;
		case 23: adev->mman.sdma_access_bo = bo; break;
		case 24: adev->rmmio_remap.bo = bo; break;
		case 25: adev->mman.buffer_funcs_enabled = true; break;
		case 26: atomic_inc(&ttm_glob.bo_count); break;
		case 27: bdev->man_drv[TTM_PL_VRAM] = &bdev->sysman; break;
		}
		assert(rt_amdgpu_cleanup_failed_probe(&pdev) == -EBUSY);
		assert(bo_freed == before && adev->mman.initialized);
		assert(adev->mman.resv_region[0].bo == bo && adev->doorbell.kernel_doorbells);
		partial_ttm_bridge_check(1, 0);
		switch (condition) {
		case 0: bo->tbo.pin_count--; break;
		case 1: assert(!refcount_dec_and_test(&bo->tbo.base.refcount.refcount)); break;
		case 2: assert(!refcount_dec_and_test(&bo->tbo.kref.refcount)); break;
		case 3: bo->tbo.base.resv = &bo->tbo.base._resv; break;
		case 4: bo->flags &= ~AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE; break;
		case 5: bo->vm_bo = NULL; break;
		case 6: bo->tbo.ttm = NULL; break;
		case 7: bo->tbo.base.import_attach = NULL; break;
		case 8: bo->tbo.base.handle_count = 0; break;
		case 9: adev->mman.resv_region[AMDGPU_RESV_FW].bo = other; break;
		case 10: adev->mman.vram_mgr.manager.usage -= PAGE_SIZE; break;
		case 11: list_del(&foreign_node); break;
		case 12: adev->mman.vram_mgr.mm.avail += PAGE_SIZE; break;
		case 13: door->func = door_ops; break;
		case 14: adev->num_ip_blocks = 1; adev->ip_blocks[1].status.hw = false; break;
		case 15: adev->mman.resv_region[0].cpu_ptr = cpu; break;
		case 16: bdev->pool.caching[0].orders[0].pages.nr_items = 0; break;
		case 17: drm_mm_remove_node(&gart_node); break;
		case 18: dma_resv_unlock(bo->tbo.base.resv); break;
		case 19: bo->tbo.base._resv.allocation_failed = false; break;
		case 20: case 21: fail_alloc = -1; break;
		case 22: door->size--; break;
		case 23: adev->mman.sdma_access_bo = NULL; break;
		case 24: adev->rmmio_remap.bo = NULL; break;
		case 25: adev->mman.buffer_funcs_enabled = false; break;
		case 26: atomic_dec(&ttm_glob.bo_count); break;
		case 27: bdev->man_drv[TTM_PL_VRAM] = &adev->mman.vram_mgr.manager; break;
		}
		assert(rt_amdgpu_cleanup_failed_probe(&pdev) == 1);
		assert(bo_freed == before + 3 && !atomic_read(&ttm_glob.bo_count));
		assert(!atomic64_read(&adev->vram_pin_size) && !atomic64_read(&adev->visible_pin_size));
		release_device(adev);
	}
}

int main(void)
{
	struct pci_dev pdev;
	/* A foreign DRM object has no surrounding amdgpu_device. These checks
	 * must decline before reading any fields beyond that actual allocation. */
	static const struct drm_driver foreign_drivers[] = {
		{.name = "different", .release = amdgpu_driver_release_kms},
		{.name = "amdgpu", .release = foreign_release},
		{.name = "amdgpu", .release = NULL},
	};
	memset(&pdev, 0, sizeof(pdev)); pdev.vendor = 0x1002; pdev.device = 0x73bf;
	struct drm_device *foreign = kzalloc(sizeof(*foreign), GFP_KERNEL);
	assert(foreign); foreign->dev = &pdev.dev;
	pci_set_drvdata(&pdev, foreign);
	for (unsigned i = 0; i < ARRAY_SIZE(foreign_drivers); i++) {
		foreign->driver = &foreign_drivers[i];
		assert(rt_amdgpu_cleanup_failed_probe(&pdev) == 0);
		assert(!ttm_glob_use_count && !pools && !pool_managers);
	}
	kfree(foreign);
	struct amdgpu_device *adev = new_failed_device(&pdev);
	assert(rt_amdgpu_cleanup_failed_probe(&pdev) == 1);
	assert(!adev->mman.initialized && rt_amdgpu_cleanup_failed_probe(&pdev) == 0);
	release_device(adev);
	for (unsigned condition = 0; condition < 14; condition++) {
		adev = new_failed_device(&pdev);
		struct ttm_device *bdev = &adev->mman.bdev;
		switch (condition) {
		case 0: adev->gmc.is_app_apu = true; break;
		case 1: adev->mman.ttm_pools = (void *)1; break;
		case 2: adev->ip_blocks[0].status.sw = true; break;
		case 3: adev->ip_blocks[0].status.hw = true; break;
		case 4: adev->ip_blocks[0].status.late_initialized = true; break;
		case 5: bdev->man_drv[TTM_PL_VRAM] = &adev->mman.vram_mgr.manager; break;
		case 6: atomic_set(&ttm_glob.bo_count, 1); break;
		case 7: bdev->sysman.usage = 1; break;
		case 8: bdev->pool.caching[0].orders[0].pages.nr_items = 1; break;
		case 9: get_page(ttm_glob.dummy_read_page); break;
		case 10: bdev->unevictable.prev = NULL; break;
		case 11: bdev->sysman.lru[0].next = NULL; break;
		case 12: ttm_glob.device_list.next = &ttm_glob.device_list; break;
		case 13: adev->mman.vram_mgr.reservations_pending.next = NULL; break;
		}
		assert(rt_amdgpu_cleanup_failed_probe(&pdev) == -EBUSY);
		assert(adev->mman.initialized && ttm_glob_use_count == 1 && pools == 1);
		partial_ttm_bridge_check(1, 0);
		adev->gmc.is_app_apu = false; adev->mman.ttm_pools = NULL;
		adev->ip_blocks[0].status.sw = false; adev->ip_blocks[0].status.hw = false;
		adev->ip_blocks[0].status.late_initialized = false;
		bdev->man_drv[TTM_PL_VRAM] = NULL; atomic_set(&ttm_glob.bo_count, 0);
		bdev->sysman.usage = 0; bdev->pool.caching[0].orders[0].pages.nr_items = 0;
		if (condition == 9) put_page(ttm_glob.dummy_read_page);
		INIT_LIST_HEAD(&bdev->unevictable);
		INIT_LIST_HEAD(&bdev->sysman.lru[0]);
		ttm_glob.device_list.next = &bdev->device_list;
		INIT_LIST_HEAD(&adev->mman.vram_mgr.reservations_pending);
		if (condition == 2) {
			/* Normal explicit finalization remains the owner when GMC sw_init
			 * succeeded; the shim guard must not finalize it first. */
			ttm_device_fini(bdev); adev->mman.initialized = false;
		} else assert(rt_amdgpu_cleanup_failed_probe(&pdev) == 1);
		release_device(adev);
	}
	assert(linuxu_module_init_gpu_buddy_module_init() == 0);
	fail_vram = false;
	adev = new_failed_device(&pdev);
	assert(atomic_read(&ttm_glob.bo_count) == 3);
	assert(adev->mman.bdev.man_drv[TTM_PL_VRAM]);
	assert(adev->mman.bdev.man_drv[TTM_PL_TT]);
	assert(adev->mman.bdev.man_drv[AMDGPU_PL_DOORBELL]);
	assert(adev->mman.bdev.man_drv[AMDGPU_PL_MMIO_REMAP]);
	assert(!adev->mman.bdev.man_drv[AMDGPU_PL_PREEMPT]);
	assert(rt_amdgpu_cleanup_failed_probe(&pdev) == 1);
	assert(bo_freed == 3 && !atomic_read(&ttm_glob.bo_count));
	release_device(adev);
	later_refusals();
	linuxu_module_exit_gpu_buddy_module_exit();
	puts("Partial upstream TTM failures: VRAM/PREEMPT cleanup and 42 ownership refusals passed under DMA hold");
}
