/* linuxu: AS-IS (third_party/linux/include/drm/drm_fbdev_shmem) — vendored verbatim from
 * the pinned 2026 tree; the unmodified third_party/linux/drivers/gpu/drm/*.c is the ABI
 * test. Kernel-only includes resolve to linuxu shadows. */
/* SPDX-License-Identifier: MIT */

#ifndef DRM_FBDEV_SHMEM_H
#define DRM_FBDEV_SHMEM_H

struct drm_fb_helper;
struct drm_fb_helper_surface_size;

#ifdef CONFIG_DRM_FBDEV_EMULATION
int drm_fbdev_shmem_driver_fbdev_probe(struct drm_fb_helper *fb_helper,
				       struct drm_fb_helper_surface_size *sizes);

#define DRM_FBDEV_SHMEM_DRIVER_OPS \
	.fbdev_probe = drm_fbdev_shmem_driver_fbdev_probe
#else
#define DRM_FBDEV_SHMEM_DRIVER_OPS \
	.fbdev_probe = NULL
#endif

#endif
