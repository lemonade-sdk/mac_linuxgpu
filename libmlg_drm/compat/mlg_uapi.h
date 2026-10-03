/* The Linux DRM, amdgpu and KFD uapi (third_party/linux/include/uapi) as a
 * macOS client of the Linux-file RPC uses it: the headers' Linux branch,
 * with this directory's linux/types.h and asm/ioctl.h, so structures and
 * ioctl numbers are exactly the Linux ones. Include after system headers;
 * the include path puts libmlg_drm/compat before include/uapi. */
#ifndef MLG_UAPI_H
#define MLG_UAPI_H

#ifndef __linux__
#define __linux__ 1
#define MLG_UAPI_DEFINED_LINUX
#endif
#include <drm/drm.h>
#include <drm/amdgpu_drm.h>
#include <linux/kfd_ioctl.h>
#ifdef MLG_UAPI_DEFINED_LINUX
#undef __linux__
#undef MLG_UAPI_DEFINED_LINUX
#endif

#endif
