/* The pinned TTM eviction loop and both cursor layers must release their
 * list hitch, reservation lock and BO hold on every control-flow exit. */
#include <assert.h>
#include <stdio.h>
#include <linux/cleanup.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include "ttm_bo_internal.h"

/* The fixture owns its BOs; the real cursor obtains/releases their krefs. */
void ttm_bo_put(struct ttm_buffer_object *bo)
{
	assert(refcount_read(&bo->kref.refcount) > 1);
	assert(!refcount_dec_and_test(&bo->kref.refcount));
}
#include "upstream-ttm-lru.inc"

static struct ttm_device bdev;
static struct ttm_resource_manager manager;
static struct ttm_buffer_object objects[2];
static struct ttm_resource resources[2];
static int visited;
static s64 process_result;
static s64 process_bo(struct ttm_lru_walk *walk, struct ttm_buffer_object *bo)
{
	(void)walk;
	assert(bo == &objects[visited++]);
	assert(dma_resv_is_locked(bo->base.resv));
	assert(refcount_read(&bo->kref.refcount) == 2);
	return process_result;
}
static const struct ttm_lru_walk_ops ops = {.process_bo = process_bo};

static void verify_released(void)
{
	for (unsigned i = 0; i < 2; i++) {
		assert(refcount_read(&objects[i].kref.refcount) == 1);
		assert(dma_resv_trylock(objects[i].base.resv));
		dma_resv_unlock(objects[i].base.resv);
	}
	assert(manager.lru[0].next == &resources[0].lru.link);
	assert(resources[0].lru.link.next == &resources[1].lru.link);
	assert(resources[1].lru.link.next == &manager.lru[0]);
	assert(manager.lru[0].prev == &resources[1].lru.link);
	for (unsigned i = 1; i < TTM_MAX_BO_PRIORITY; i++) assert(list_empty(&manager.lru[i]));
}
static int return_early(struct ttm_lru_walk_arg *arg)
{
	struct ttm_bo_lru_cursor cursor;
	struct ttm_buffer_object *bo;
	ttm_bo_lru_for_each_reserved_guarded(&cursor, &manager, arg, bo) {
		assert(bo == objects && dma_resv_is_locked(bo->base.resv));
		return 42;
	}
	return 0;
}
int main(void)
{
	spin_lock_init(&bdev.lru_lock);
	ttm_resource_manager_init(&manager, &bdev, 8 * PAGE_SIZE);
	for (unsigned i = 0; i < 2; i++) {
		kref_init(&objects[i].kref);
		dma_resv_init(&objects[i].base._resv);
		objects[i].base.resv = &objects[i].base._resv;
		objects[i].bdev = &bdev;
		objects[i].resource = &resources[i];
		resources[i].bo = &objects[i];
		resources[i].mem_type = TTM_PL_SYSTEM;
		ttm_lru_item_init(&resources[i].lru, TTM_LRU_RESOURCE);
		list_add_tail(&resources[i].lru.link, &manager.lru[0]);
	}
	struct ttm_operation_ctx context = {0};
	struct ttm_lru_walk walk = {.ops = &ops, .arg = {.ctx = &context}};
	const s64 results[] = {1, -EIO, -EBUSY, -EALREADY, 2};
	for (unsigned i = 0; i < ARRAY_SIZE(results); i++) {
		visited = 0; process_result = results[i];
		s64 progress = ttm_lru_walk_for_evict(&walk, &bdev, &manager, 1);
		assert(progress == (process_result == -EBUSY || process_result == -EALREADY ? 0 : process_result));
		assert(visited == (process_result == -EBUSY || process_result == -EALREADY ? 2 : 1));
		verify_released();
	}
	assert(return_early(&walk.arg) == 42);
	verify_released();
	for (unsigned i = 0; i < 2; i++) list_del_init(&resources[i].lru.link);
	visited = 0;
	assert(ttm_lru_walk_for_evict(&walk, &bdev, &manager, 1) == 0 && !visited);
	for (unsigned i = 0; i < TTM_MAX_BO_PRIORITY; i++) assert(list_empty(&manager.lru[i]));
	for (unsigned i = 0; i < 2; i++) dma_resv_fini(objects[i].base.resv);
	puts("upstream TTM LRU: target break, error, skipped objects, exhausted/empty list and early return release cursor/BO/lock ownership");
}
