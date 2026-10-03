/* linuxu shim: gpuvm — amdgpu GPUVM glue hooks the drm glue needs;
 * strategy (MAPPING: in-process pointers,
 * VA registry for userptr).  P0 skeleton: the real GPUVM page-table
 * walk lands with P2 (WP5); the entry points exist so amdgpu_gem/
 * ttm link. */
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

/*
 * amdgpu_vm_* entry points are in third_party/linux/drivers/gpu/drm/amd/amdgpu (upstream); this file
 * hosts only the *drm-side* glue that the shim owns:
 * drm_gem_lru maintenance + vma fault helper registration.
 */

/* drm_gem_lru (upstream in drm_gem_lru.c; provided here) */
int drm_gem_lru_reserve(struct drm_gem_lru *lru, size_t size,
			struct drm_gem_lru_cookie *cookie)
{
	(void)lru;
	(void)size;
	(void)cookie;
	return -EOPNOTSUPP;
}

void drm_gem_lru_unreserve(struct drm_gem_lru *lru,
			   struct drm_gem_lru_cookie *cookie)
{
	(void)lru;
	(void)cookie;
}

/* userptr validate/register helpers (WP1/WP13 territory) */
int gpuvm_userptr_validate(unsigned long uaddr, unsigned long size)
{
	/* ANONONLY check via the VA registry (TODO: WP1) */
	(void)uaddr;
	(void)size;
	return -EOPNOTSUPP;
}

int gpuvm_userptr_register(unsigned long uaddr, unsigned long size)
{
	/* mmu_interval_notifier_register = no-op (WP13) */
	(void)uaddr;
	(void)size;
	return -EOPNOTSUPP;
}

void gpuvm_userptr_unregister(unsigned long uaddr, unsigned long size)
{
	(void)uaddr;
	(void)size;
}
