/* linuxu: SHIM (third_party/linux/include/linux/dma-fence-chain.h)
 *
 * Aligned to the pinned 2026 vendor header: struct dma_fence_chain embeds
 * `base` (the chain node's own fence) plus `prev` (RCU link to the
 * previous node) / `prev_seqno` / `fence` (the wrapped payload fence).
 * The unmodified driver .c files (amdgpu_gem.c, amdgpu_sync.c,
 * drm_syncobj.c) are the ABI test.
 *
 * Previous nodes and payloads own transferred references. Walking a timeline
 * reclaims completed history using RCU-protected references and link updates.
 * Parent notifications are deferred past child callback locks.
 */
#ifndef _DMA_FENCE_CHAIN_H
#define _DMA_FENCE_CHAIN_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/dma-fence.h>

struct dma_fence;

/* struct dma_fence_chain - fence to represent a node of a fence chain
 * (vendor 2026 layout). The irq_work union member is trimmed: the host
 * shim never signals chain nodes from irq context, only through the
 * callback half. */
struct dma_fence_chain {
	/* @base: fence to represent the chain node itself */
	struct dma_fence base;
	/* @prev: previous chain node */
	struct dma_fence *prev;
	/* @prev_seqno: seqno of the previous chain node */
	u64 prev_seqno;
	/* @fence: the fence this chain node wraps */
	struct dma_fence *fence;
	/* @cb: callback parked on the payload fence that signals the
	 * node's base fence when the payload completes (upstream this
	 * is a union with irq_work; the shim uses only the cb half). */
	struct dma_fence_cb cb;
	struct rcu_head signal_rcu;
};

/* to_dma_fence_chain - NULL if @fence is not a chain node (vendor 2026) */
static inline struct dma_fence_chain *
to_dma_fence_chain(struct dma_fence *fence)
{
	if (!fence || !dma_fence_is_chain(fence))
		return NULL;
	return container_of(fence, struct dma_fence_chain, base);
}

/*
 * dma_fence_chain_contained - return the fence contained in @fence if it is
 * a chain node, otherwise @fence itself (upstream form).
 */
static inline struct dma_fence *
dma_fence_chain_contained(struct dma_fence *fence)
{
	struct dma_fence_chain *chain = to_dma_fence_chain(fence);

	return chain ? chain->fence : fence;
}

/*
 * dma_fence_chain_alloc
 *
 * Returns a new struct dma_fence_chain object or NULL on failure.
 * Upstream 2026 uses kmalloc_obj(); the shim kmalloc is type-first.
 */
#define dma_fence_chain_alloc() \
	kmalloc(sizeof(struct dma_fence_chain), GFP_KERNEL)

/*
 * dma_fence_chain_free
 * @chain: chain node to free
 *
 * Frees an allocated but not used struct dma_fence_chain object. After
 * dma_fence_chain_init() has been called the fence must be released with
 * dma_fence_put(), not through this function.
 */
static inline void dma_fence_chain_free(struct dma_fence_chain *chain)
{
	kfree(chain);
}

/*
 * dma_fence_chain_for_each - iterate over all fences in chain
 * @iter: current fence
 * @head: starting point
 *
 * We keep a reference to the current fence while inside the loop which
 * must be dropped when breaking out.
 */
#define dma_fence_chain_for_each(iter, head)	\
	for (iter = dma_fence_get(head); iter;	\
	     iter = dma_fence_chain_walk(iter))

struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence);
int dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno);
void dma_fence_chain_init(struct dma_fence_chain *chain,
			  struct dma_fence *prev,
			  struct dma_fence *fence,
			  uint64_t seqno);

#endif /* _DMA_FENCE_CHAIN_H */
