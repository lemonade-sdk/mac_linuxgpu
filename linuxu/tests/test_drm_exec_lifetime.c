/* Canonical GEM object layouts, actual ww locks, and injected allocation /
 * reservation failures. All objects and reservations are CPU-only fixtures. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/dma-resv.h>
#include <drm/drm_gem.h>
#include <drm/drm_exec.h>
extern int usleep(unsigned int);
void msleep(unsigned int ms) { usleep(ms * 1000); }

DEFINE_WW_CLASS(reservation_ww_class);
static int fail_alloc, fail_reserve, contend_once;
static struct dma_resv *contended;
static unsigned reserved;
void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{ (void)flags; return fail_alloc || (size && n > SIZE_MAX / size) ? NULL : malloc(n * size); }
void *krealloc_array(const void *p, size_t n, size_t size, gfp_t flags)
{ (void)flags; return fail_alloc || (size && n > SIZE_MAX / size) ? NULL : realloc((void *)p, n * size); }
void kfree(const void *p) { free((void *)p); }
void drm_gem_object_free(struct kref *ref) { (void)ref; assert(!"fixture reference lost"); }
int dma_resv_lock(struct dma_resv *r, struct ww_acquire_ctx *ctx)
{
	if (r == contended && contend_once) { contend_once = 0; return -EDEADLK; }
	return ww_mutex_lock(&r->lock, ctx);
}
int dma_resv_lock_interruptible(struct dma_resv *r, struct ww_acquire_ctx *ctx)
{ return dma_resv_lock(r, ctx); }
int dma_resv_lock_slow(struct dma_resv *r, struct ww_acquire_ctx *ctx)
{ return ww_mutex_lock(&r->lock, ctx); }
int dma_resv_lock_slow_interruptible(struct dma_resv *r, struct ww_acquire_ctx *ctx)
{ return dma_resv_lock_slow(r, ctx); }
void dma_resv_unlock(struct dma_resv *r) { ww_mutex_unlock(&r->lock); }
int dma_resv_reserve_fences(struct dma_resv *r, unsigned count)
{
	assert(ww_mutex_is_locked(&r->lock));
	if (fail_reserve) return -ENOMEM;
	reserved += count; return 0;
}
static void clean_objects(struct drm_gem_object *objects, unsigned count)
{
	for (unsigned i = 0; i < count; ++i) {
		assert(refcount_read(&objects[i].refcount.refcount) == 1);
		assert(!ww_mutex_is_locked(&objects[i].resv->lock));
		assert(!ww_mutex_is_locked(&objects[i]._resv.lock));
	}
}
int main(void)
{
	struct drm_gem_object objects[3] = {0};
	struct dma_resv external[3] = {0};
	struct drm_gem_object *array[] = {&objects[0], &objects[1], &objects[2]};
	struct drm_exec exec;
	unsigned loops = 0;
	for (unsigned i = 0; i < 3; ++i) {
		kref_init(&objects[i].refcount);
		objects[i].resv = &external[i];
		ww_mutex_init(&external[i].lock, &reservation_ww_class);
		ww_mutex_init(&objects[i]._resv.lock, &reservation_ww_class);
	}
	drm_exec_init(&exec, 0, 1);
	drm_exec_until_all_locked(&exec) {
		loops++;
		assert(drm_exec_prepare_array(&exec, array, 3, 2) == 0);
	}
	assert(loops == 1 && reserved == 6 && exec.num_objects == 3);
	assert(exec.ticket.acquired == 3);
	assert(drm_exec_lock_obj(&exec, &objects[0]) == -EALREADY);
	drm_exec_fini(&exec); clean_objects(objects, 3);

	drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES, 0);
	assert(drm_exec_cleanup(&exec));
	assert(drm_exec_lock_obj(&exec, &objects[0]) == 0);
	assert(drm_exec_lock_obj(&exec, &objects[0]) == 0 && exec.num_objects == 1);
	assert(ww_mutex_lock(&external[1].lock, NULL) == 0);
	drm_exec_unlock_obj(&exec, &objects[1]);
	assert(ww_mutex_is_locked(&external[1].lock)); /* not owned by exec */
	ww_mutex_unlock(&external[1].lock);
	drm_exec_fini(&exec); clean_objects(objects, 3);

	fail_alloc = 1;
	drm_exec_init(&exec, 0, 2);
	assert(exec.max_objects == 0 && drm_exec_cleanup(&exec));
	assert(drm_exec_lock_obj(&exec, &objects[0]) == -ENOMEM);
	clean_objects(objects, 3);
	fail_alloc = 0;
	assert(drm_exec_lock_obj(&exec, &objects[0]) == 0);
	fail_reserve = 1;
	assert(drm_exec_prepare_obj(&exec, &objects[1], 4) == -ENOMEM);
	assert(exec.num_objects == 1 && !ww_mutex_is_locked(&external[1].lock));
	fail_reserve = 0;
	drm_exec_fini(&exec); clean_objects(objects, 3);

	contended = &external[1]; contend_once = 1; loops = 0;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec) {
		assert(++loops <= 2);
		int r = drm_exec_prepare_array(&exec, array, 3, 1);
		drm_exec_retry_on_contention(&exec);
		assert(r == 0);
	}
	assert(loops == 2 && exec.ticket.acquired == 3 && !exec.prelocked);
	drm_exec_fini(&exec); clean_objects(objects, 3);

	/* Contention reference must also survive an empty-array retry. */
	contend_once = 1;
	drm_exec_init(&exec, 0, 0);
	assert(drm_exec_cleanup(&exec));
	assert(drm_exec_lock_obj(&exec, &objects[0]) == 0);
	assert(drm_exec_lock_obj(&exec, &objects[1]) == -EDEADLK);
	assert(drm_exec_cleanup(&exec));
	assert(drm_exec_prepare_array(&exec, NULL, 0, 0) == 0);
	assert(!drm_exec_cleanup(&exec) && exec.num_objects == 1);
	drm_exec_fini(&exec); clean_objects(objects, 3);

	/* Explicitly removing the prelocked reservation must clear its shortcut. */
	contend_once = 1;
	drm_exec_init(&exec, 0, 0);
	assert(drm_exec_cleanup(&exec));
	assert(drm_exec_lock_obj(&exec, &objects[1]) == -EDEADLK);
	assert(drm_exec_cleanup(&exec));
	assert(drm_exec_lock_obj(&exec, &objects[0]) == 0);
	assert(exec.prelocked == &objects[1]);
	drm_exec_unlock_obj(&exec, &objects[1]);
	assert(!exec.prelocked && !ww_mutex_is_locked(&external[1].lock));
	assert(drm_exec_lock_obj(&exec, &objects[1]) == 0);
	assert(ww_mutex_is_locked(&external[1].lock));
	drm_exec_fini(&exec); clean_objects(objects, 3);

	/* Finalize before the first loop; no uninitialized ticket/reference. */
	drm_exec_init(&exec, 0, 0); drm_exec_fini(&exec);
	clean_objects(objects, 3);
	puts("GEM execution layout, reservation retries, OOM and reference cleanup passed");
	return 0;
}
