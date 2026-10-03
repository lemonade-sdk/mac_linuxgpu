#include <pthread.h>
#include <limits.h>
/* Linux reservation ownership and usage semantics over the platform ww lock. */
#include <string.h>
#include <linux/dma-resv.h>
#include <linux/dma-fence-array.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

struct ww_class reservation_ww_class = {
	.stamp = LONGATOMIC_INIT(0), .acquire_name = "reservation_acquire",
	.mutex_name = "reservation_mutex",
};

void dma_resv_init(struct dma_resv *obj)
{
	memset(obj, 0, sizeof(*obj));
	ww_mutex_init(&obj->lock, &reservation_ww_class);
	pthread_mutex_init(&obj->fences_lock, NULL);
	atomic_set(&obj->count, 1);
}
struct dma_resv *dma_resv_alloc(void)
{
	struct dma_resv *obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (obj) { dma_resv_init(obj); obj->heap_allocated = true; }
	return obj;
}
static void put_fences(unsigned count, struct dma_fence **fences)
{
	for (unsigned i = 0; i < count; i++) dma_fence_put(fences[i]);
	kfree(fences);
}
void dma_resv_fini(struct dma_resv *obj)
{
	dma_fence_put(obj->excl);
	put_fences(obj->num_fences, obj->fences);
	kfree(obj->usages);
	obj->excl = NULL; obj->fences = NULL; obj->usages = NULL;
	obj->num_fences = obj->capacity = 0;
	pthread_mutex_destroy(&obj->fences_lock);
	ww_mutex_destroy(&obj->lock);
}
void dma_resv_put(struct dma_resv *obj)
{
	if (obj && atomic_dec_and_test(&obj->count)) {
		bool allocated = obj->heap_allocated;
		dma_resv_fini(obj);
		if (allocated) kfree(obj);
	}
}
/* Caller holds fences_lock. Reserving N means N additional fence slots. */
static int reserve_locked(struct dma_resv *obj, unsigned additional)
{
	if (additional > UINT_MAX - obj->num_fences) return -ENOMEM;
	unsigned count = obj->num_fences + additional;
	if (count <= obj->capacity) return 0;
	struct dma_fence **fences = kcalloc(count, sizeof(*fences), GFP_KERNEL);
	enum dma_resv_usage *usages = kcalloc(count, sizeof(*usages), GFP_KERNEL);
	if (!fences || !usages) { kfree(fences); kfree(usages); return -ENOMEM; }
	if (obj->num_fences) {
		memcpy(fences, obj->fences, obj->num_fences * sizeof(*fences));
		memcpy(usages, obj->usages, obj->num_fences * sizeof(*usages));
	}
	kfree(obj->fences); kfree(obj->usages);
	obj->fences = fences; obj->usages = usages; obj->capacity = count;
	return 0;
}
int dma_resv_reserve_fences(struct dma_resv *obj, unsigned additional)
{
	pthread_mutex_lock(&obj->fences_lock);
	int ret = reserve_locked(obj, additional);
	pthread_mutex_unlock(&obj->fences_lock);
	return ret;
}
void dma_resv_add_fence_unsafe(struct dma_resv *obj, struct dma_fence *fence,
			     enum dma_resv_usage usage)
{
	if (!fence) return;
	struct dma_fence *old = NULL;
	pthread_mutex_lock(&obj->fences_lock);
	for (unsigned i = 0; i < obj->num_fences; i++) {
		if (obj->fences[i]->context != fence->context) continue;
		if (dma_fence_is_later_or_same(fence, obj->fences[i])) {
			old = obj->fences[i]; obj->fences[i] = dma_fence_get(fence);
		}
		if (usage < obj->usages[i]) obj->usages[i] = usage;
		obj->generation++;
		goto out;
	}
	/* Upstream reserves before add. Keep compatibility with older callers,
	 * but never report success from waits if an unreserved add runs out. */
	if (reserve_locked(obj, 1)) { obj->allocation_failed = true; goto out; }
	obj->fences[obj->num_fences] = dma_fence_get(fence);
	obj->usages[obj->num_fences++] = usage;
	obj->generation++;
out:
	pthread_mutex_unlock(&obj->fences_lock);
	dma_fence_put(old);
}
void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
		      enum dma_resv_usage usage)
{
	/* The upstream caller already holds the reservation ww mutex. */
	dma_resv_add_fence_unsafe(obj, fence, usage);
}
void dma_resv_add_fence_noflush(struct dma_resv *obj, struct dma_fence *fence,
			      enum dma_resv_usage usage)
{ dma_resv_add_fence(obj, fence, usage); }
void dma_resv_add_exclusive_fence(struct dma_resv *obj, struct dma_fence *fence)
{
	pthread_mutex_lock(&obj->fences_lock);
	struct dma_fence *old = obj->excl;
	obj->excl = dma_fence_get(fence); obj->generation++;
	pthread_mutex_unlock(&obj->fences_lock);
	dma_fence_put(old);
}
struct dma_fence *dma_resv_take_exclusive(struct dma_resv *obj)
{
	pthread_mutex_lock(&obj->fences_lock);
	struct dma_fence *fence = obj->excl;
	obj->excl = NULL; obj->generation++;
	pthread_mutex_unlock(&obj->fences_lock);
	return fence;
}
void dma_resv_replace_fences(struct dma_resv *obj, uint64_t context,
			   struct dma_fence *fence, enum dma_resv_usage usage)
{
	if (!fence) return;
	/* The writer holds obj->lock. Drop old references outside the metadata
	 * lock because driver release callbacks may inspect reservations. */
	for (unsigned i = 0;; i++) {
		struct dma_fence *old = NULL;
		pthread_mutex_lock(&obj->fences_lock);
		if (i >= obj->num_fences) { pthread_mutex_unlock(&obj->fences_lock); break; }
		if (obj->fences[i]->context == context) {
			old = obj->fences[i]; obj->fences[i] = dma_fence_get(fence);
			obj->usages[i] = usage; obj->generation++;
		}
		pthread_mutex_unlock(&obj->fences_lock);
		dma_fence_put(old);
	}
	pthread_mutex_lock(&obj->fences_lock);
	struct dma_fence *old = NULL;
	if (obj->excl && obj->excl->context == context) {
		old = obj->excl; obj->excl = dma_fence_get(fence); obj->generation++;
	}
	pthread_mutex_unlock(&obj->fences_lock);
	dma_fence_put(old);
}
static int snapshot(struct dma_resv *obj, enum dma_resv_usage usage,
		    unsigned *count, struct dma_fence ***result,
		    enum dma_resv_usage **result_usages)
{
	*count = 0; *result = NULL;
	if (result_usages) *result_usages = NULL;
	pthread_mutex_lock(&obj->fences_lock);
	if (obj->allocation_failed) { pthread_mutex_unlock(&obj->fences_lock); return -ENOMEM; }
	unsigned n = obj->excl && usage >= DMA_RESV_USAGE_WRITE;
	for (unsigned i = 0; i < obj->num_fences; i++) n += obj->usages[i] <= usage;
	if (!n) { pthread_mutex_unlock(&obj->fences_lock); return 0; }
	struct dma_fence **fences = kcalloc(n, sizeof(*fences), GFP_KERNEL);
	enum dma_resv_usage *usages = result_usages ? kcalloc(n, sizeof(*usages), GFP_KERNEL) : NULL;
	if (!fences || (result_usages && !usages)) {
		kfree(fences); kfree(usages); pthread_mutex_unlock(&obj->fences_lock); return -ENOMEM;
	}
	unsigned j = 0;
	if (obj->excl && usage >= DMA_RESV_USAGE_WRITE) {
		if (usages) usages[j] = DMA_RESV_USAGE_WRITE;
		fences[j++] = dma_fence_get(obj->excl);
	}
	for (unsigned i = 0; i < obj->num_fences; i++) {
		if (obj->usages[i] > usage) continue;
		if (usages) usages[j] = obj->usages[i];
		fences[j++] = dma_fence_get(obj->fences[i]);
	}
	pthread_mutex_unlock(&obj->fences_lock);
	*count = n; *result = fences;
	if (result_usages) *result_usages = usages;
	return 0;
}
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src)
{
	if (dst == src) return 0;
	unsigned n; struct dma_fence **fences; enum dma_resv_usage *usages;
	int ret = snapshot(src, DMA_RESV_USAGE_BOOKKEEP, &n, &fences, &usages);
	if (ret) return ret;
	pthread_mutex_lock(&dst->fences_lock);
	struct dma_fence **old = dst->fences, *excl = dst->excl;
	unsigned old_count = dst->num_fences;
	enum dma_resv_usage *old_usages = dst->usages;
	dst->fences = fences; dst->usages = usages; dst->num_fences = dst->capacity = n;
	dst->excl = NULL; dst->allocation_failed = false; dst->generation++;
	pthread_mutex_unlock(&dst->fences_lock);
	put_fences(old_count, old); dma_fence_put(excl); kfree(old_usages);
	return 0;
}
int dma_resv_get_fences(struct dma_resv *obj, enum dma_resv_usage usage,
		       unsigned *count, struct dma_fence ***fences)
{ return snapshot(obj, usage, count, fences, NULL); }
int dma_resv_get_singleton(struct dma_resv *obj, enum dma_resv_usage usage,
			   struct dma_fence **fence)
{
	unsigned count; struct dma_fence **fences;
	*fence = NULL;
	int ret = dma_resv_get_fences(obj, usage, &count, &fences);
	if (ret || !count) return ret;
	if (count == 1) { *fence = fences[0]; kfree(fences); return 0; }
	struct dma_fence_array *array = dma_fence_array_create(count, fences,
		dma_fence_context_alloc(1), 1, false);
	if (!array) { put_fences(count, fences); return -ENOMEM; }
	*fence = &array->base;
	return 0;
}
struct dma_fence *dma_resv_get_exclusive_rcu(const struct dma_resv *obj)
{
	if (!obj) return NULL;
	pthread_mutex_lock((pthread_mutex_t *)&obj->fences_lock);
	struct dma_fence *fence = dma_fence_get(obj->excl);
	pthread_mutex_unlock((pthread_mutex_t *)&obj->fences_lock);
	return fence;
}
struct dma_fence *dma_resv_get_exclusive(struct dma_resv *obj)
{ return dma_resv_get_exclusive_rcu(obj); }
struct dma_fence *dma_resv_get_rcu(struct dma_resv *obj, struct ww_acquire_ctx *ctx,
				 enum dma_resv_usage usage)
{
	(void)ctx; struct dma_fence *fence = NULL;
	if (obj) dma_resv_get_singleton(obj, usage, &fence);
	return fence;
}
static struct dma_fence *iter_next(struct dma_resv_iter *cursor, bool unlocked)
{
	struct dma_resv *obj = cursor->obj;
	struct dma_fence *old = cursor->unlocked ? cursor->fence : NULL;
	cursor->fence = NULL; cursor->unlocked = unlocked;
	if (!obj) { dma_fence_put(old); return NULL; }
	pthread_mutex_lock(&obj->fences_lock);
	cursor->is_restarted = cursor->generation != obj->generation;
	if (cursor->is_restarted) cursor->index = 0;
	cursor->generation = obj->generation;
	while (cursor->index <= obj->num_fences) {
		unsigned index = cursor->index++;
		struct dma_fence *fence = index ? obj->fences[index - 1] : obj->excl;
		enum dma_resv_usage usage = index ? obj->usages[index - 1] : DMA_RESV_USAGE_WRITE;
		if (!fence || usage > cursor->usage) continue;
		cursor->fence = unlocked ? dma_fence_get(fence) : fence;
		cursor->fence_usage = usage;
		break;
	}
	pthread_mutex_unlock(&obj->fences_lock);
	dma_fence_put(old);
	return cursor->fence;
}
struct dma_fence *dma_resv_iter_first_unlocked(struct dma_resv_iter *cursor)
{
	dma_resv_iter_end(cursor); cursor->index = 0;
	struct dma_fence *fence = iter_next(cursor, true);
	cursor->is_restarted = true;
	return fence;
}
struct dma_fence *dma_resv_iter_next_unlocked(struct dma_resv_iter *cursor)
{ return iter_next(cursor, true); }
struct dma_fence *dma_resv_iter_first(struct dma_resv_iter *cursor)
{ cursor->index = 0; return iter_next(cursor, false); }
struct dma_fence *dma_resv_iter_next(struct dma_resv_iter *cursor)
{ return iter_next(cursor, false); }
bool dma_resv_test_signaled(struct dma_resv *obj, enum dma_resv_usage usage)
{
	pthread_mutex_lock(&obj->fences_lock);
	bool failed = obj->allocation_failed;
	pthread_mutex_unlock(&obj->fences_lock);
	if (failed) return false;
	struct dma_resv_iter cursor; struct dma_fence *fence; bool done = true;
	dma_resv_iter_begin(&cursor, obj, usage);
	dma_resv_for_each_fence_unlocked(&cursor, fence) {
		if (!dma_fence_is_signaled(fence)) { done = false; break; }
	}
	dma_resv_iter_end(&cursor);
	return done;
}
bool dma_resv_test_signalled(struct dma_resv *obj, enum dma_resv_usage usage)
{ return dma_resv_test_signaled(obj, usage); }
bool dma_resv_test_signaled_unsafe(struct dma_resv *obj, enum dma_resv_usage usage)
{ return dma_resv_test_signaled(obj, usage); }
static long wait_fences(unsigned count, struct dma_fence **fences, bool intr, unsigned long timeout)
{
	if (!timeout) {
		for (unsigned i = 0; i < count; i++)
			if (!dma_fence_is_signaled(fences[i])) return 0;
		return 1;
	}
	long remaining = timeout > LONG_MAX ? LONG_MAX : (long)timeout;
	for (unsigned i = 0; i < count; i++) {
		remaining = dma_fence_wait_timeout(fences[i], intr, remaining);
		if (remaining <= 0) return remaining;
	}
	return remaining ? remaining : 1;
}
long dma_resv_wait_timeout(struct dma_resv *obj, enum dma_resv_usage usage,
			   bool intr, unsigned long timeout)
{
	unsigned count; struct dma_fence **fences;
	int ret = dma_resv_get_fences(obj, usage, &count, &fences);
	if (ret) return ret;
	long remaining = wait_fences(count, fences, intr, timeout);
	put_fences(count, fences);
	return remaining;
}
int dma_resv_wait(struct dma_resv *obj, enum dma_resv_usage usage, bool intr)
{
	long ret = dma_resv_wait_timeout(obj, usage, intr, MAX_SCHEDULE_TIMEOUT);
	return ret < 0 ? (int)ret : 0;
}
struct resv_wait_work {
	struct work_struct work;
	unsigned count;
	struct dma_fence **fences;
	bool intr;
	unsigned long timeout;
};
static void wait_work_fn(struct work_struct *work)
{
	struct resv_wait_work *w = container_of(work, struct resv_wait_work, work);
	wait_fences(w->count, w->fences, w->intr, w->timeout);
	put_fences(w->count, w->fences); kfree(w);
}
void dma_resv_wait_timeout_work(struct dma_resv *obj, enum dma_resv_usage usage,
	bool intr, unsigned long timeout, struct work_struct *work)
{
	(void)work;
	struct resv_wait_work *w = kmalloc(sizeof(*w), GFP_KERNEL);
	if (!w) return;
	if (dma_resv_get_fences(obj, usage, &w->count, &w->fences)) { kfree(w); return; }
	w->intr = intr; w->timeout = timeout;
	INIT_WORK(&w->work, wait_work_fn);
	if (!queue_work(system_wq, &w->work)) { put_fences(w->count, w->fences); kfree(w); }
}
void dma_resv_update_fences(struct dma_resv *obj, struct dma_fence *excl,
			   struct dma_fence **shared, int count)
{
	if (count < 0 || (count && !shared) || dma_resv_reserve_fences(obj, count)) return;
	dma_resv_add_exclusive_fence(obj, excl);
	for (int i = 0; i < count; i++) dma_resv_add_fence(obj, shared[i], DMA_RESV_USAGE_READ);
}
void dma_resv_describe(struct dma_resv *obj, struct seq_file *seq) { (void)obj; (void)seq; }
int dma_resv_lock(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{ return ww_mutex_lock(&obj->lock, ctx); }
int dma_resv_lock_interruptible(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{ return ww_mutex_lock_interruptible(&obj->lock, ctx); }
int dma_resv_lock_slow(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{ return ww_mutex_lock(&obj->lock, ctx); }
int dma_resv_lock_slow_interruptible(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{ return ww_mutex_lock_slow_interruptible(&obj->lock, ctx); }
bool dma_resv_trylock(struct dma_resv *obj) { return ww_mutex_trylock(&obj->lock, NULL); }
bool dma_resv_is_locked(const struct dma_resv *obj) { return mutex_is_locked(&obj->lock.base); }
void dma_resv_assert_held(const struct dma_resv *obj) { (void)obj; }
void dma_resv_unlock(struct dma_resv *obj) { ww_mutex_unlock(&obj->lock); }
