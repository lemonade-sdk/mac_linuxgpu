/* linuxu: SHIM — <drm/drm_mode.h> resolves to the uapi mode surface
 * (the 2026 kernel header has no drm/drm_mode.h; .c files include this
 * for DRM_MODE_* flags). */
#ifndef _DRM_MODE_H_LINUXU_
#define _DRM_MODE_H_LINUXU_
#include <uapi/drm/drm_mode.h>
#endif
