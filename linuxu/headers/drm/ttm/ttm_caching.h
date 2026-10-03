/* linuxu: AS-IS (third_party/linux/include/drm/ttm/ttm_caching.h)
 * 2026 enum: ttm_uncached / ttm_write_combined / ttm_cached.
 * ttm_prot_from_caching is defined in third_party/linux/drivers/gpu/drm/ttm/ttm_module.c.
 */
#ifndef _TTM_CACHING_H_
#define _TTM_CACHING_H_

#include <linux/pgtable.h>

#define TTM_NUM_CACHING_TYPES	3

enum ttm_caching {
	ttm_uncached,
	ttm_write_combined,
	ttm_cached
};

pgprot_t ttm_prot_from_caching(enum ttm_caching caching, pgprot_t tmp);

#endif
