/* AMDGPU_INFO through the render node (rt/drm_info.h), the way libdrm's
 * amdgpu_query_info() and amdgpu_top issue it on Linux: a process opens
 * the render node and calls DRM_IOCTL_AMDGPU_INFO with return_pointer in
 * its own memory. */
#include <pthread.h>
#include <stddef.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/mmu_notifier.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/amdgpu_drm.h>
#include <rt/drm_info.h>
#include <rt/process.h>
#include <rt/process_file.h>

/* The reader process's own address space holds only this block. */
#define RT_DRM_INFO_ARENA_VA	(1ULL << 46)

struct rt_drm_info {
	struct linuxu_process *proc;
	pthread_mutex_t lock;
	int fd;
};

struct rt_drm_info_arena {
	struct drm_amdgpu_info request;
	uint8_t result[RT_DRM_INFO_MAX_BYTES];
};

int rt_drm_info_query_allowed(uint32_t query)
{
	switch (query) {
	case AMDGPU_INFO_SENSOR:
	case AMDGPU_INFO_VRAM_USAGE:
	case AMDGPU_INFO_VIS_VRAM_USAGE:
	case AMDGPU_INFO_GTT_USAGE:
	case AMDGPU_INFO_VRAM_GTT:
	case AMDGPU_INFO_MEMORY:
	case AMDGPU_INFO_DEV_INFO:
	case AMDGPU_INFO_READ_MMR_REG:
		return 1;
	default:
		return 0;
	}
}

int rt_drm_info_open(struct pci_dev *pdev, struct rt_drm_info **out)
{
	struct linuxu_process_saved saved;
	struct drm_device *ddev = pdev ? pci_get_drvdata(pdev) : NULL;
	struct rt_drm_info *info;
	int r;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!ddev || !ddev->render)
		return -ENODEV;
	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	pthread_mutex_init(&info->lock, NULL);
	info->proc = linuxu_process_create(0, "drm-info");
	if (!info->proc) {
		kfree(info);
		return -ENOMEM;
	}
	r = linuxu_process_enter(info->proc, &saved);
	if (!r) {
		info->fd = rt_process_open_chrdev(MKDEV(DRM_MAJOR, ddev->render->index),
						  O_RDWR | O_CLOEXEC);
		linuxu_process_leave(&saved);
		r = info->fd < 0 ? info->fd : 0;
	}
	if (r) {
		rt_drm_info_close(info);
		return r;
	}
	*out = info;
	return 0;
}

int rt_drm_info_query(struct rt_drm_info *info, uint32_t query, const void *args,
		      size_t args_size, void *out, uint32_t size)
{
	struct linuxu_process_saved saved;
	struct rt_drm_info_arena *arena;
	const size_t union_bytes = sizeof(struct drm_amdgpu_info) -
				   offsetof(struct drm_amdgpu_info, mode_crtc);
	long r;

	if (!info || !out || !size || size > RT_DRM_INFO_MAX_BYTES ||
	    args_size > union_bytes || (args_size && !args))
		return -EINVAL;
	if (!rt_drm_info_query_allowed(query))
		return -EPERM;
	arena = kzalloc(sizeof(*arena), GFP_KERNEL);
	if (!arena)
		return -ENOMEM;
	arena->request.return_pointer = RT_DRM_INFO_ARENA_VA +
					offsetof(struct rt_drm_info_arena, result);
	arena->request.return_size = size;
	arena->request.query = query;
	if (args_size)
		memcpy(&arena->request.mode_crtc, args, args_size);
	pthread_mutex_lock(&info->lock);
	r = linuxu_process_enter(info->proc, &saved);
	if (!r) {
		r = rt_process_ioctl(info->fd, DRM_IOCTL_AMDGPU_INFO, RT_DRM_INFO_ARENA_VA,
				     arena, sizeof(*arena));
		linuxu_process_leave(&saved);
	}
	pthread_mutex_unlock(&info->lock);
	if (!r)
		memcpy(out, arena->result, size);
	kfree(arena);
	return (int)r;
}

void rt_drm_info_close(struct rt_drm_info *info)
{
	if (!info)
		return;
	/* exit_files closes the render file: drm_release, then upstream's
	 * postclose tears down the file's VM. */
	linuxu_process_exit(info->proc);
	mmu_notifier_synchronize();
	pthread_mutex_destroy(&info->lock);
	kfree(info);
}
