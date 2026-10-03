/* linuxu: AS-IS (third_party/linux/include/drm/ttm/ttm_kmap_iter.h)
 * 2026 layout: ops carry map_local/unmap_local/maps_tt; struct ttm_kmap_iter
 * is embedded in a resource-specific specialization. The .c files call the
 * ops directly (no init/next inlines in the pinned tree).
 */
#ifndef __TTM_KMAP_ITER_H__
#define __TTM_KMAP_ITER_H__

#include <linux/types.h>

struct ttm_kmap_iter;
struct iosys_map;

struct ttm_kmap_iter_ops {
	void (*map_local)(struct ttm_kmap_iter *res_iter,
			  struct iosys_map *dmap, pgoff_t i);
	void (*unmap_local)(struct ttm_kmap_iter *res_iter,
			    struct iosys_map *dmap);
	bool maps_tt;
};

struct ttm_kmap_iter {
	const struct ttm_kmap_iter_ops *ops;
};

#endif /* __TTM_KMAP_ITER_H__ */
