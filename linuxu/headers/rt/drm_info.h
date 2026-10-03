/* An AMDGPU_INFO reader (linuxu/src/amdgpu-rt/drm_info.c): one linuxu
 * process holding the device's render node open, as amdgpu_top holds
 * /dev/dri/renderD128, and issuing DRM_IOCTL_AMDGPU_INFO through the
 * file's ioctl (drm_ioctl, then upstream amdgpu_info_ioctl). The open
 * creates the drm_file upstream creates for any render client, VM
 * included; it never creates a context, BO or queue.
 *
 * Shared with DriverKit and C++ callers: no kernel types. */
#ifndef LINUXU_RT_DRM_INFO_H
#define LINUXU_RT_DRM_INFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;
struct rt_drm_info;

/* The largest result one query returns. */
#define RT_DRM_INFO_MAX_BYTES 4096u

/* Open the render node of the DRM device bound to @pdev (its PCI drvdata). */
int rt_drm_info_open(struct pci_dev *pdev, struct rt_drm_info **out);

/* The queries a monitor issues: sensors, memory usage and sizes, device
 * info and allowed registers. Anything else is -EPERM before the ioctl. */
int rt_drm_info_query_allowed(uint32_t query);

/* One DRM_IOCTL_AMDGPU_INFO: @query and its 16 argument bytes (the union of
 * struct drm_amdgpu_info, e.g. sensor_info or read_mmr_reg), @size result
 * bytes into @out. Returns 0 or the ioctl's negative errno. Serialized per
 * reader; callers may sleep in upstream locks. */
int rt_drm_info_query(struct rt_drm_info *info, uint32_t query, const void *args,
		      size_t args_size, void *out, uint32_t size);

/* Close the render file and exit the process (amdgpu_driver_postclose_kms
 * runs). Must not race a query on the same reader. */
void rt_drm_info_close(struct rt_drm_info *info);

#ifdef __cplusplus
}
#endif
#endif
