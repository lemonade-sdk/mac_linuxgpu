/* linuxu: SHIM (redirect) — vendor/linux has no drm/dma_resv.h; the
 * reservation object lives in <linux/dma-resv.h> (see linuxu/headers/linux/dma-resv.h).
 * Kept so historical includes resolve. */
#ifndef _DRM_DMA_RESV_H
#define _DRM_DMA_RESV_H
#include <linux/dma-resv.h>
#endif
