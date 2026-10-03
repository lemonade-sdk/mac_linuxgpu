/* Place the upstream GART aperture at a host-chosen VA before TTM allocates
 * its first pinned GTT BO. The upstream location routine still determines
 * the default and applies its MC aperture constraints. */
#include <pthread.h>
#include <stdint.h>

#include <linux/errno.h>
#include <rt/gart.h>

#include "amdgpu.h"
#include "amdgpu_gmc.h"

#define RT_GART_MIN_BASE (1ULL << 32)
#define RT_GART_VA_LIMIT (1ULL << 47)
#define RT_GART_DEFAULT_SIZE (512ULL << 20)

static pthread_mutex_t gart_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t requested_base;
static uint64_t selected_base;
static uint64_t selected_size;
static int placement_result = -EAGAIN;
static int location_called;

static uint64_t requested_size(void)
{
	if (amdgpu_gart_size == -1)
		return RT_GART_DEFAULT_SIZE;
	if (amdgpu_gart_size <= 0)
		return 0;
	return (uint64_t)amdgpu_gart_size << 20;
}

static int valid_static_window(uint64_t base, uint64_t size)
{
	if (!size || (size & (size - 1)) || base < RT_GART_MIN_BASE ||
	    (base & (size - 1)) || base >= RT_GART_VA_LIMIT ||
	    size > RT_GART_VA_LIMIT - base)
		return -EINVAL;
	return 0;
}

int rt_gart_set_window(uint64_t base)
{
	uint64_t size = requested_size();
	int r = valid_static_window(base, size);
	if (r)
		return r;
	pthread_mutex_lock(&gart_lock);
	if (location_called || (requested_base && requested_base != base))
		r = -EBUSY;
	else {
		requested_base = base;
		selected_size = size;
		placement_result = -EAGAIN;
	}
	pthread_mutex_unlock(&gart_lock);
	return r;
}

int rt_gart_get_window(uint64_t *base, uint64_t *size)
{
	int r;
	if (!base || !size)
		return -EINVAL;
	pthread_mutex_lock(&gart_lock);
	*size = selected_size ? selected_size : requested_size();
	*base = location_called ? selected_base : requested_base;
	r = location_called && placement_result ? placement_result : 0;
	pthread_mutex_unlock(&gart_lock);
	return r;
}

int rt_gart_status(void)
{
	int r;
	pthread_mutex_lock(&gart_lock);
	r = placement_result;
	pthread_mutex_unlock(&gart_lock);
	return r;
}

void rt_gart_reset(void)
{
	pthread_mutex_lock(&gart_lock);
	requested_base = 0;
	selected_base = 0;
	selected_size = 0;
	placement_result = -EAGAIN;
	location_called = 0;
	pthread_mutex_unlock(&gart_lock);
}

void rt_amdgpu_gmc_gart_location(struct amdgpu_device *adev,
				struct amdgpu_gmc *mc,
				enum amdgpu_gart_placement placement)
{
	uint64_t base, size;
	int r = -EINVAL;
	amdgpu_gmc_gart_location(adev, mc, placement);
	pthread_mutex_lock(&gart_lock);
	location_called = 1;
	selected_base = 0;
	selected_size = mc->gart_size;
	base = requested_base;
	size = requested_size();
	if (!base)
		r = -EAGAIN;
	else if (!adev || !mc || mc->gart_size != size ||
		 valid_static_window(base, size) ||
		 mc->fb_start > mc->fb_end ||
		 !(base > mc->fb_end || base + size <= mc->fb_start) ||
		 base > adev->gmc.mc_mask ||
		 size - 1 > adev->gmc.mc_mask - base)
		r = -ERANGE;
	else {
		mc->gart_start = base;
		mc->gart_end = base + size - 1;
		selected_base = base;
		r = 0;
	}
	placement_result = r;
	pthread_mutex_unlock(&gart_lock);
}
