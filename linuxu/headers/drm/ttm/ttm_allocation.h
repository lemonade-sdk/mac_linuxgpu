/* linuxu: SHIM (ttm allocation flags — upstream ttm_allocation.h carries
 * TTM_ALLOCATION_* used by struct ttm_device.alloc_flags) */
#ifndef _TTM_ALLOCATION_H_
#define _TTM_ALLOCATION_H_

#include <linux/types.h>
#include <linux/bits.h>

/* vendor 2026 ttm_allocation.h */
#define TTM_ALLOCATION_POOL_BENEFICIAL_ORDER(n)	((n) & 0xff)

/**
 * enum ttm_alloc_flags
 */
enum ttm_alloc_flags {
	/**
	 * TTM_ALLOCATION_CAN_SPLIT:
	 *
	 * Allow the allocator to split the allocation into multiple
	 * contiguous blocks.
	 */
	TTM_ALLOCATION_CAN_SPLIT = BIT(0),

	/**
	 * TTM_ALLOCATION_CAN_FAIL:
	 *
	 * Allow the allocation to fail rather than invoking the OOM
	 * killer.
	 */
	TTM_ALLOCATION_CAN_FAIL = BIT(1),

	/**
	 * TTM_ALLOCATION_PROPAGATE_ENOSPC:
	 *
	 * Do not convert ENOSPC from resource managers to ENOMEM.
	 */
	TTM_ALLOCATION_PROPAGATE_ENOSPC = BIT(10),

	TTM_ALLOCATION_POOL_USE_DMA_ALLOC = BIT(8),
	TTM_ALLOCATION_POOL_USE_DMA32 = BIT(9),
};

#endif
