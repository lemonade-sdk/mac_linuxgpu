/* linuxu: SHIM (third_party/linux/include/linux/shrinker.h)
 *
 * Aligned to the 2026 pinned API used by third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c: struct
 * shrink_control, struct shrinker (function-pointer form), SHRINK_STOP /
 * SHRINK_EMPTY, and shrinker_register/shrinker_unregister. In the userspace
 * dext the shrinker is not wired to the real reclaim path; the pool's own
 * bookkeeping is what ttm_pool.c exercises.
 */
#ifndef _LINUX_SHRINKER_H
#define _LINUX_SHRINKER_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/slab.h>

struct mem_cgroup;
struct completion;
struct rcu_head;

struct shrink_control {
	gfp_t gfp_mask;
	int nid;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
	struct mem_cgroup *memcg;
};

#define SHRINK_STOP	(~0UL)
#define SHRINK_EMPTY	(~0UL - 1)

struct shrinker {
	unsigned long (*count_objects)(struct shrinker *shrink,
				       struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *shrink,
				      struct shrink_control *sc);
	long batch;
	int seeks;
	unsigned int flags;
	/* Shim-owned registry links; no reclaim callback runs without a host hook. */
	struct shrinker *linuxu_next;
	bool linuxu_registered;
};

/* Registry lifetime is real; DriverKit has no Linux memory-pressure hook yet. */
extern int shrinker_register(struct shrinker *shrink);
extern void shrinker_unregister(struct shrinker *shrink);

/* 2026 shrinker_alloc / flags (shim) */
#define SHRINKER_NUMA_AWARE	(1U << 0)
#define SHRINKER_MEMCG_AWARE	(1U << 1)
extern struct shrinker *shrinker_alloc(unsigned int flags, const char *name);
extern void shrinker_free(struct shrinker *shrink);

#endif /* _LINUX_SHRINKER_H */
