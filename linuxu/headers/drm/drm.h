/* linuxu: SHIM (drm/drm.h — kernel-side wrapper over the uapi copy) */
#ifndef _LINUXU_DRM_KERNEL_H
#define _LINUXU_DRM_KERNEL_H

/*
 * Upstream <drm/drm.h> is a thin kernel-side wrapper that pulls in the
 * uapi definitions plus a handful of kernel-only helpers.  In the linuxu
 * shadow tree the uapi copy lives at uapi/drm/drm.h (verbatim), and the
 * kernel-only helpers (DRM_IOCTL_DEF, drm_ioctl_call, ...) are all
 * declared here so that <drm/drm_ioctl.h> and the KMD's ioctl tables
 * resolve.
 */
#include <uapi/drm/drm.h>
#include <uapi/drm/drm_mode.h>

/*
 * The ioctl command band is the uapi's: DRM_COMMAND_BASE 0x40 to
 * DRM_COMMAND_END 0xA0. Driver ioctl numbers (DRM_IOCTL_AMDGPU_*) are
 * DRM_COMMAND_BASE + their index, as on Linux, and drm_ioctl() routes
 * numbers below the band to the core table (DRM_IOCTL_VERSION is 0x00).
 */

#endif
