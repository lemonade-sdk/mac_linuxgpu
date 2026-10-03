/* linuxu: SHIM (third_party/linux/include/linux/cgroup_dmem.h)
 *
 * CONFIG_CGROUP_DMEM=n in the linuxu build (no cgroup v2 dmem controller in
 * userspace), so this is the vendor #else branch verbatim: all the pool/region
 * APIs collapse to static no-ops. ttm_bo.c calls dmem_cgroup_state_evict_
 * valuable() / dmem_cgroup_pool_state_put() which then reduce to true/no-op.
 */
#ifndef _CGROUP_DMEM_H
#define _CGROUP_DMEM_H

#include <linux/types.h>
#include <linux/llist.h>

struct dmem_cgroup_pool_state;

/* Opaque definition of a cgroup region, used internally */
struct dmem_cgroup_region;

static inline struct dmem_cgroup_region *
dmem_cgroup_register_region(u64 size, const char *name_fmt, ...)
{
	return NULL;
}

static inline void dmem_cgroup_unregister_region(struct dmem_cgroup_region *region)
{ }

static inline int dmem_cgroup_try_charge(struct dmem_cgroup_region *region, u64 size,
					 struct dmem_cgroup_pool_state **ret_pool,
					 struct dmem_cgroup_pool_state **ret_limit_pool)
{
	*ret_pool = NULL;

	if (ret_limit_pool)
		*ret_limit_pool = NULL;

	return 0;
}

static inline void dmem_cgroup_uncharge(struct dmem_cgroup_pool_state *pool, u64 size)
{ }

static inline
bool dmem_cgroup_state_evict_valuable(struct dmem_cgroup_pool_state *limit_pool,
				      struct dmem_cgroup_pool_state *test_pool,
				      bool ignore_low, bool *ret_hit_low)
{
	return true;
}

static inline void dmem_cgroup_pool_state_put(struct dmem_cgroup_pool_state *pool)
{ }

#endif	/* _CGROUP_DMEM_H */
