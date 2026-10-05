/* Surprise removal of the GPU (rt/removal.h). */
#include <pthread.h>

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <rt/removal.h>
#include <rt/device_string.h>

#include "amdgpu.h"

static pthread_mutex_t removal_lock = PTHREAD_MUTEX_INITIALIZER;
static struct amdgpu_device *removal_adev;
static struct task_struct *removal_thread;
static unsigned long removal_completions;

/* What amdgpu_fence_driver_hw_fini does for an unplugged device: every
 * fence emitted on the ring is completed with -ECANCELED. */
static void complete_rings(struct amdgpu_device *adev)
{
	for (unsigned int i = 0; i < AMDGPU_MAX_RINGS; ++i) {
		struct amdgpu_ring *ring = adev->rings[i];

		if (!ring || !ring->fence_drv.initialized)
			continue;
		if ((uint32_t)atomic_read(&ring->fence_drv.last_seq) ==
		    READ_ONCE(ring->fence_drv.sync_seq))
			continue;
		amdgpu_fence_driver_force_completion(ring);
		__atomic_add_fetch(&removal_completions, 1, __ATOMIC_RELAXED);
	}
}

static int removal_main(void *arg)
{
	struct amdgpu_device *adev = arg;

	while (!kthread_should_stop()) {
		complete_rings(adev);
		msleep(RT_REMOVAL_TICK_MS);
	}
	return 0;
}

int rt_removal_begin(struct pci_dev *pdev)
{
	struct drm_device *ddev;
	struct amdgpu_device *adev;
	int r = 0;

	if (!pdev)
		return -EINVAL;
	/* First, before anything else can store through it: the VRAM aperture
	 * the CPU maps (rt/device_string.h), as amdgpu_device_unmap_mmio
	 * takes it away on Linux. */
	linuxu_aperture_gone("the device was removed from the bus");
	linuxu_pci_mark_removed(pdev);
	ddev = pci_get_drvdata(pdev);
	if (!ddev)
		return 0;
	adev = drm_to_adev(ddev);
	pthread_mutex_lock(&removal_lock);
	if (!removal_adev) {
		/* amdgpu_device_halt's order: the DRM device first, then no
		 * hardware access, then the fences. */
		WRITE_ONCE(ddev->unplugged, true);
		adev->no_hw_access = true;
		complete_rings(adev);
		removal_adev = adev;
		removal_thread = kthread_run(removal_main, adev, "amdgpu-removal");
		if (IS_ERR_OR_NULL(removal_thread)) {
			removal_thread = NULL;
			r = -ENOMEM;
		}
		dev_warn(adev->dev, "device removed from the bus: no hardware access, "
			 "GPU work completes with -ECANCELED\n");
	}
	pthread_mutex_unlock(&removal_lock);
	return r;
}

int rt_device_lost(struct pci_dev *pdev, const char *why)
{
	struct drm_device *ddev;
	struct amdgpu_device *adev;
	int r = 0;

	if (!pdev)
		return -EINVAL;
	linuxu_aperture_gone(why ? why : "the device no longer answers");
	ddev = pci_get_drvdata(pdev);
	if (!ddev)
		return 0;
	adev = drm_to_adev(ddev);
	pthread_mutex_lock(&removal_lock);
	if (!removal_adev) {
		WRITE_ONCE(ddev->unplugged, true);
		adev->no_hw_access = true;
		removal_adev = adev;
		/* The rings are completed by the thread, never here: the caller
		 * may hold any lock. */
		removal_thread = kthread_run(removal_main, adev, "amdgpu-lost");
		if (IS_ERR_OR_NULL(removal_thread)) {
			removal_thread = NULL;
			r = -ENOMEM;
		}
		dev_err(adev->dev, "%s: the device no longer answers; no hardware access, "
			"GPU work completes with -ECANCELED\n", why ? why : "device lost");
	}
	pthread_mutex_unlock(&removal_lock);
	return r;
}

void rt_removal_end(void)
{
	struct task_struct *thread;
	struct amdgpu_device *adev;

	pthread_mutex_lock(&removal_lock);
	thread = removal_thread;
	adev = removal_adev;
	removal_thread = NULL;
	removal_adev = NULL;
	pthread_mutex_unlock(&removal_lock);
	if (thread)
		kthread_stop(thread);
	if (adev)
		complete_rings(adev);
}

bool rt_removal_active(struct amdgpu_device *adev)
{
	struct pci_dev *pdev = adev ? adev->pdev : NULL;

	return pdev && __atomic_load_n(&pdev->error_state, __ATOMIC_ACQUIRE) ==
		       pci_channel_io_perm_failure;
}

unsigned long rt_removal_completions(void)
{
	return __atomic_load_n(&removal_completions, __ATOMIC_RELAXED);
}
