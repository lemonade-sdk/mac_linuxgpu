/* linuxu: SHIM (third_party/linux/include/linux/dma-resv.h)
 * Reservation fences use Linux usage ordering and own their references.
 * The ww mutex serializes writers; a separate metadata lock protects snapshots.
 */
#ifndef _LINUX_DMA_RESV_H
#define _LINUX_DMA_RESV_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/rwlock.h>
#include <linux/ww_mutex.h>
#include <linux/dma-fence.h>

typedef u64 dma_fence_context_lock;

/* ---- usage (upstream enum) ---- */
enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL = 0,
	DMA_RESV_USAGE_WRITE,
	DMA_RESV_USAGE_READ,
	DMA_RESV_USAGE_BOOKKEEP,
	DMA_RESV_USAGE_CURSOR = DMA_RESV_USAGE_BOOKKEEP,
	DMA_RESV_USAGE_ANY = DMA_RESV_USAGE_BOOKKEEP,
};
static inline enum dma_resv_usage dma_resv_usage_rw(bool write)
{
	return write ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE;
}

/* ---- core object ---- */
struct dma_resv {
	struct ww_mutex	lock;
	struct dma_fence	*excl;
	struct dma_fence	**fences;
	unsigned int		num_fences;
	bool			cachable;
	unsigned long		domain;
	atomic_t		count;
	pthread_mutex_t fences_lock;
	enum dma_resv_usage *usages;
	unsigned int capacity;
	u64 generation;
	bool allocation_failed;
	bool heap_allocated;
};

/* ---- iterator (upstream shape) ---- */
struct dma_resv_list {
	struct dma_resv	*resv;
	struct dma_fence	*fence;
	enum dma_resv_usage	usage;
};

struct dma_resv_iter {
	struct dma_resv	*obj;
	enum dma_resv_usage	usage;
	struct dma_fence	*fence;
	enum dma_resv_usage	fence_usage;
	unsigned int		index;
	struct dma_resv_list	*fences;
	unsigned int		num_fences;
	bool			is_restarted;
	bool unlocked;
	u64 generation;
};

/* ---- alloc/lock ---- */
struct dma_resv *dma_resv_alloc(void);
void dma_resv_fini(struct dma_resv *resv);
void dma_resv_init(struct dma_resv *resv);

int dma_resv_lock(struct dma_resv *obj,
		  struct ww_acquire_ctx *ctx);
int dma_resv_lock_interruptible(struct dma_resv *obj,
				struct ww_acquire_ctx *ctx);
int dma_resv_lock_slow(struct dma_resv *obj,
		       struct ww_acquire_ctx *ctx);
int dma_resv_lock_slow_interruptible(struct dma_resv *obj,
				     struct ww_acquire_ctx *ctx);
bool dma_resv_trylock(struct dma_resv *obj);
void dma_resv_unlock(struct dma_resv *obj);
bool dma_resv_is_locked(const struct dma_resv *obj);
void dma_resv_assert_held(const struct dma_resv *obj);
static inline struct ww_acquire_ctx *
dma_resv_locking_ctx(const struct dma_resv *obj)
{
	return obj->lock.ctx;
}

/* ---- fences ---- */
int dma_resv_reserve_fences(struct dma_resv *obj, unsigned int num_fences);
void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
			enum dma_resv_usage usage);
void dma_resv_add_fence_unsafe(struct dma_resv *obj, struct dma_fence *fence,
			       enum dma_resv_usage usage);
void dma_resv_add_exclusive_fence(struct dma_resv *obj, struct dma_fence *fence);
void dma_resv_add_fence_noflush(struct dma_resv *obj, struct dma_fence *fence,
				enum dma_resv_usage usage);
struct dma_fence *dma_resv_take_exclusive(struct dma_resv *obj);
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src);
void dma_resv_replace_fences(struct dma_resv *obj, uint64_t context,
			     struct dma_fence *fence,
			     enum dma_resv_usage usage);
int dma_resv_get_fences(struct dma_resv *obj, enum dma_resv_usage usage,
			unsigned int *num_fences, struct dma_fence ***fences);
int dma_resv_get_singleton(struct dma_resv *obj, enum dma_resv_usage usage,
			   struct dma_fence **fence);
bool dma_resv_test_signaled(struct dma_resv *obj, enum dma_resv_usage usage);
bool dma_resv_test_signalled(struct dma_resv *obj, enum dma_resv_usage usage);
bool dma_resv_test_signaled_unsafe(struct dma_resv *obj, enum dma_resv_usage usage);
struct dma_fence *dma_resv_get_exclusive_rcu(const struct dma_resv *obj);
struct dma_fence *dma_resv_get_exclusive(struct dma_resv *resv);
struct dma_fence *dma_resv_get_rcu(struct dma_resv *resv,
				   struct ww_acquire_ctx *ctx,
				   enum dma_resv_usage usage);

/* ---- iterator ---- */
struct dma_fence *dma_resv_iter_first_unlocked(struct dma_resv_iter *cursor);
struct dma_fence *dma_resv_iter_next_unlocked(struct dma_resv_iter *cursor);
struct dma_fence *dma_resv_iter_first(struct dma_resv_iter *cursor);
struct dma_fence *dma_resv_iter_next(struct dma_resv_iter *cursor);
static inline void dma_resv_iter_begin(struct dma_resv_iter *cursor,
				       struct dma_resv *obj,
				       enum dma_resv_usage usage)
{
	cursor->obj = obj;
	cursor->usage = usage;
	cursor->fence = NULL;
	cursor->fence_usage = DMA_RESV_USAGE_ANY;
	cursor->index = 0;
	cursor->num_fences = 0;
	cursor->is_restarted = true;
	cursor->unlocked = false;
	cursor->generation = 0;
}
static inline void dma_resv_iter_end(struct dma_resv_iter *cursor)
{
	if (cursor->unlocked) dma_fence_put(cursor->fence);
	cursor->fence = NULL;
}
static inline bool dma_resv_iter_is_restarted(struct dma_resv_iter *cursor)
{
	return cursor->is_restarted;
}
static inline enum dma_resv_usage dma_resv_iter_usage(struct dma_resv_iter *cursor)
{
	return cursor->fence_usage;
}
#define dma_resv_for_each_fence(cursor, obj, usage, fence)		\
	for (dma_resv_iter_begin(cursor, obj, usage),			\
	     (fence) = dma_resv_iter_first(cursor);				\
	     (fence) != NULL; (fence) = dma_resv_iter_next(cursor))
#define dma_resv_for_each_fence_unlocked(cursor, fence)			\
	for ((fence) = dma_resv_iter_first_unlocked(cursor);		\
	     (fence) != NULL; (fence) = dma_resv_iter_next_unlocked(cursor))

/* ---- wait ---- */
struct timespec64;
extern long dma_resv_wait_timeout(struct dma_resv *resv,
				  enum dma_resv_usage usage, bool intr,
				  unsigned long timeout);
extern int dma_resv_wait(struct dma_resv *resv, enum dma_resv_usage usage,
			 bool intr);
extern void dma_resv_wait_timeout_work(struct dma_resv *resv,
				       enum dma_resv_usage usage, bool intr,
				       unsigned long timeout,
				       struct work_struct *work);

/* ---- misc ---- */
extern void dma_resv_describe(struct dma_resv *resv, struct seq_file *seq);
static inline struct dma_resv *dma_resv_get(struct dma_resv *resv)
{
	atomic_inc(&resv->count);
	return resv;
}
void dma_resv_put(struct dma_resv *resv);
void dma_resv_update_fences(struct dma_resv *obj, struct dma_fence *excl,
			   struct dma_fence **shared, int num_shared);

static inline bool dma_resv_held(struct dma_resv *r)
{
	return dma_resv_is_locked(r);
}

#endif /* _LINUX_DMA_RESV_H */