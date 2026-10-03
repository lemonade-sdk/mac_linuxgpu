/* Aggregate fence API matching the pinned Linux callers. */
#ifndef __LINUX_DMA_FENCE_ARRAY_H
#define __LINUX_DMA_FENCE_ARRAY_H
#include <linux/dma-fence.h>
struct dma_fence_array;
struct dma_fence_array_cb {
	struct dma_fence_cb cb;
	struct dma_fence_array *array;
};
struct dma_fence_array {
	struct dma_fence base;
	unsigned num_fences;
	atomic_t num_pending;
	struct dma_fence **fences;
	struct rcu_head signal_rcu;
	bool signal_on_any;
	struct dma_fence_array_cb callbacks[];
};
static inline struct dma_fence_array *to_dma_fence_array(struct dma_fence *fence)
{
	return fence && dma_fence_is_array(fence) ?
		container_of(fence, struct dma_fence_array, base) : NULL;
}
struct dma_fence_array *dma_fence_array_alloc(int num_fences);
void dma_fence_array_init(struct dma_fence_array *array, int num_fences,
			 struct dma_fence **fences, u64 context, unsigned seqno,
			 bool signal_on_any);
struct dma_fence_array *dma_fence_array_create(int num_fences,
			struct dma_fence **fences, u64 context, unsigned seqno,
			bool signal_on_any);
struct dma_fence *dma_fence_array_first(struct dma_fence *head);
struct dma_fence *dma_fence_array_next(struct dma_fence *head, unsigned index);
bool dma_fence_match_context(struct dma_fence *fence, u64 context);
#define dma_fence_array_for_each(fence, index, head) \
	for (index = 0, fence = dma_fence_array_first(head); fence; \
	     ++index, fence = dma_fence_array_next(head, index))
#endif
