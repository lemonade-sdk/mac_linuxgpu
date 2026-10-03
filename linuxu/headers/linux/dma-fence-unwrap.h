/* linuxu: SHIM (third_party/linux/include/linux/dma-fence-unwrap.h)
 * Aligned to the pinned 2026 vendor surface: dma_fence_unwrap cursor
 * struct + dma_fence_unwrap_for_each() + the variadic
 * dma_fence_unwrap_merge() macro (the driver calls it with 1 or 2
 * fences). */
#ifndef __LINUX_DMA_FENCE_UNWRAP_H
#define __LINUX_DMA_FENCE_UNWRAP_H
#include <linux/dma-fence.h>

/**
 * struct dma_fence_unwrap - cursor into the container structure
 *
 * Should be used with dma_fence_unwrap_for_each() iterator macro.
 */
struct dma_fence_unwrap {
	/**
	 * @chain: potential dma_fence_chain, but can be other fence as well
	 */
	struct dma_fence *chain;
	/**
	 * @array: potential dma_fence_array, but can be other fence as well
	 */
	struct dma_fence *array;
	/**
	 * @index: last returned index if @array is really a dma_fence_array
	 */
	unsigned int index;
};

struct dma_fence *dma_fence_unwrap_first(struct dma_fence *head,
					 struct dma_fence_unwrap *cursor);
struct dma_fence *dma_fence_unwrap_next(struct dma_fence_unwrap *cursor);

/**
 * dma_fence_unwrap_for_each - iterate over all fences in containers
 * @fence: current fence
 * @cursor: current position inside the containers
 * @head: starting point for the iterator
 */
#define dma_fence_unwrap_for_each(fence, cursor, head)			\
	for (fence = dma_fence_unwrap_first(head, cursor); fence;	\
	     fence = dma_fence_unwrap_next(cursor))

struct dma_fence *__dma_fence_unwrap_merge(unsigned int num_fences,
					   struct dma_fence **fences,
					   struct dma_fence_unwrap *cursors);

int dma_fence_dedup_array(struct dma_fence **array, int num_fences);

/**
 * dma_fence_unwrap_merge - unwrap and merge fences (vendor 2026 variadic
 * form; every input dependency is retained by the returned owned fence).
 */
#define dma_fence_unwrap_merge(...)					\
	({								\
		struct dma_fence *__f[] = { __VA_ARGS__ };		\
		struct dma_fence_unwrap __c[ARRAY_SIZE(__f)];		\
									\
		__dma_fence_unwrap_merge(ARRAY_SIZE(__f), __f, __c);	\
	})

#endif
