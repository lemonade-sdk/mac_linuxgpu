/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* GEM execution reservations use the canonical object layout and retain every
 * tracked object through contention retries and final unlock. */
#include <limits.h>
#include <string.h>
#include <drm/drm_exec.h>
#include <drm/drm_gem.h>
#include <linux/dma-resv.h>
#include <linux/errno.h>
#include <linux/slab.h>

#define EXEC_FIRST_PASS ((struct drm_gem_object *)(uintptr_t)-1)

static void unlock_all(struct drm_exec *exec)
{
	while (exec->num_objects) {
		struct drm_gem_object *obj = exec->objects[--exec->num_objects];
		dma_resv_unlock(obj->resv);
		drm_gem_object_put(obj);
	}
	drm_gem_object_put(exec->prelocked);
	exec->prelocked = NULL;
}

void drm_exec_init(struct drm_exec *exec, u32 flags, unsigned nr)
{
	memset(exec, 0, sizeof(*exec));
	exec->flags = flags;
	if (nr)
		exec->objects = kmalloc_array(nr, sizeof(*exec->objects), GFP_KERNEL);
	exec->max_objects = exec->objects ? nr : 0;
	exec->contended = EXEC_FIRST_PASS;
}

void drm_exec_fini(struct drm_exec *exec)
{
	unlock_all(exec);
	if (exec->contended != EXEC_FIRST_PASS) {
		drm_gem_object_put(exec->contended);
		ww_acquire_fini(&exec->ticket);
	}
	kfree(exec->objects);
	exec->objects = NULL;
	exec->max_objects = 0;
	exec->contended = EXEC_FIRST_PASS;
}

bool drm_exec_cleanup(struct drm_exec *exec)
{
	if (!exec->contended) {
		ww_acquire_done(&exec->ticket);
		return false;
	}
	if (exec->contended == EXEC_FIRST_PASS) {
		ww_acquire_init(&exec->ticket, &reservation_ww_class);
		exec->contended = NULL;
	} else {
		unlock_all(exec);
	}
	return true;
}

static int track_locked(struct drm_exec *exec, struct drm_gem_object *obj)
{
	if (exec->num_objects == exec->max_objects) {
		unsigned capacity;
		void *objects;
		if (exec->max_objects > UINT_MAX - 64)
			return -ENOMEM;
		capacity = exec->max_objects + 64;
		objects = krealloc_array(exec->objects, capacity,
					 sizeof(*exec->objects), GFP_KERNEL);
		if (!objects)
			return -ENOMEM;
		exec->objects = objects;
		exec->max_objects = capacity;
	}
	drm_gem_object_get(obj);
	exec->objects[exec->num_objects++] = obj;
	return 0;
}

static int lock_contended(struct drm_exec *exec)
{
	struct drm_gem_object *obj = exec->contended;
	int r;
	if (!obj)
		return 0;
	if (obj == EXEC_FIRST_PASS)
		return -EINVAL; /* acquisition requires the cleanup/retry loop */
	exec->contended = NULL;
	if (exec->flags & DRM_EXEC_INTERRUPTIBLE_WAIT)
		r = dma_resv_lock_slow_interruptible(obj->resv, &exec->ticket);
	else
		r = dma_resv_lock_slow(obj->resv, &exec->ticket);
	if (!r) {
		r = track_locked(exec, obj);
		if (!r) {
			exec->prelocked = obj; /* retain the contention reference */
			return 0;
		}
		dma_resv_unlock(obj->resv);
	}
	drm_gem_object_put(obj);
	return r;
}

int drm_exec_lock_obj(struct drm_exec *exec, struct drm_gem_object *obj)
{
	int r = lock_contended(exec);
	if (r)
		return r;
	if (exec->prelocked == obj) {
		drm_gem_object_put(exec->prelocked);
		exec->prelocked = NULL;
		return 0;
	}
	r = exec->flags & DRM_EXEC_INTERRUPTIBLE_WAIT ?
		dma_resv_lock_interruptible(obj->resv, &exec->ticket) :
		dma_resv_lock(obj->resv, &exec->ticket);
	if (r == -EDEADLK) {
		drm_gem_object_get(obj);
		exec->contended = obj;
		return r;
	}
	if (r == -EALREADY && (exec->flags & DRM_EXEC_IGNORE_DUPLICATES))
		return 0;
	if (r)
		return r;
	r = track_locked(exec, obj);
	if (r)
		dma_resv_unlock(obj->resv);
	return r;
}

void drm_exec_unlock_obj(struct drm_exec *exec, struct drm_gem_object *obj)
{
	for (unsigned i = exec->num_objects; i--;) {
		if (exec->objects[i] != obj)
			continue;
		dma_resv_unlock(obj->resv);
		memmove(&exec->objects[i], &exec->objects[i + 1],
			(exec->num_objects - i - 1) * sizeof(*exec->objects));
		exec->num_objects--;
		if (exec->prelocked == obj) {
			exec->prelocked = NULL;
			drm_gem_object_put(obj);
		}
		drm_gem_object_put(obj);
		return;
	}
}

int drm_exec_prepare_obj(struct drm_exec *exec, struct drm_gem_object *obj,
                        unsigned int num_fences)
{
	int r = drm_exec_lock_obj(exec, obj);
	if (r)
		return r;
	r = dma_resv_reserve_fences(obj->resv, num_fences);
	if (r)
		drm_exec_unlock_obj(exec, obj);
	return r;
}

int drm_exec_prepare_array(struct drm_exec *exec, struct drm_gem_object **objects,
                          unsigned int num_objects, unsigned int num_fences)
{
	if (!num_objects)
		return lock_contended(exec);
	for (unsigned i = 0; i < num_objects; ++i) {
		int r = drm_exec_prepare_obj(exec, objects[i], num_fences);
		if (r)
			return r;
	}
	return 0;
}
