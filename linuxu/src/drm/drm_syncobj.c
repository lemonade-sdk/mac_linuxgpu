/* linuxu shim: drm_syncobj — the driver's own drm-core/drm_syncobj.c
 * (in the 463-file driver build set) provides all drm_syncobj_*
 * symbols, so this shim keeps no definitions; the file remains a
 * marker for the syncobj namespace in the linuxu/src build list. */
#include <drm/drm_syncobj.h>
