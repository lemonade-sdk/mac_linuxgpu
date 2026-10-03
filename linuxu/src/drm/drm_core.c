/* linuxu shim: drm_core — the driver's own drm-core/drm_drv.c
 * (in the 463-file driver build set) provides all drm_device
 * lifecycle symbols (drm_dev_alloc, __drm_dev_alloc,
 * __devm_drm_dev_alloc, register/unregister, put/get, ...), so this
 * shim keeps no definitions; the file remains a marker for the
 * drm-core namespace in the linuxu/src build list. */
#include <drm/drm_drv.h>
