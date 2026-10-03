/* linuxu shim: drm_gem — the driver's own drm-core/drm_gem.c (in the
 * 463-file driver build set) provides all drm_gem_* symbols, so this
 * shim keeps no definitions; the file remains a marker for the GEM
 * namespace in the linuxu/src build list. */
#include <drm/drm_gem.h>
