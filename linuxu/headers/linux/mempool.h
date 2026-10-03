/* linuxu: SHIM (third_party/linux/include/linux/mempool.h)
 * The only pool the amdgpu tree uses is a kmalloc-backed pool
 * (mempool_create_kmalloc_pool in ras_log_ring.c). Backed directly by
 * kmalloc; no pre-allocation bookkeeping in userspace. */
#ifndef _LINUX_MEMPOOL_H
#define _LINUX_MEMPOOL_H

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/gfp.h>

typedef void (mempool_free_t)(void *element, void *pool_data);
typedef void *(*mempool_alloc_t)(gfp_t gfp_mask, void *pool_data);

struct mempool {
	int min_nr;
	int curr_nr;
	size_t size;
	mempool_alloc_t *alloc;
	mempool_free_t *free;
	void *pool_data;
};

void *mempool_alloc_preallocated(struct mempool *pool);
void *mempool_alloc_noprof(struct mempool *pool, gfp_t gfp_mask);
#define mempool_alloc(...) mempool_alloc_noprof(__VA_ARGS__)
void mempool_free(void *element, struct mempool *pool);
void mempool_destroy(struct mempool *pool);

static inline void *
mempool_create_kmalloc_pool(int min_nr, size_t size)
{
	struct mempool *pool = kzalloc_obj(struct mempool);
	if (pool) {
		pool->min_nr = min_nr;
		pool->size = size;
	}
	return pool;
}

#endif /* _LINUX_MEMPOOL_H */
