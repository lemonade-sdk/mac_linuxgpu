#include <pthread.h>
#include <limits.h>
/* Fence containers retain every dependency until its callback is finished. */
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/slab.h>
#include <linux/sort.h>

static void chain_free_rcu(struct rcu_head *head)
{
	struct dma_fence_chain *chain = container_of(head, struct dma_fence_chain, base.rcu);
	dma_fence_put(chain->prev);
	dma_fence_put(chain->fence);
	kfree(chain);
}
static void chain_release(struct dma_fence *fence)
{
	call_rcu(&fence->rcu, chain_free_rcu);
}
static bool chain_signaled(struct dma_fence *base)
{
	/* Timeline depth must not become native call-stack depth. */
	struct dma_fence *iter = dma_fence_get(base);
	while (iter) {
		struct dma_fence *payload = dma_fence_chain_contained(iter);
		if (!dma_fence_is_signaled(payload)) { dma_fence_put(iter); return false; }
		iter = dma_fence_chain_walk(iter);
	}
	return true;
}
static void chain_scan_deferred(struct rcu_head *head);
static void chain_payload_cb(struct dma_fence *f, struct dma_fence_cb *cb)
{
	(void)f;
	struct dma_fence_chain *chain = container_of(cb, struct dma_fence_chain, cb);
	call_rcu(&chain->signal_rcu, chain_scan_deferred);
	dma_fence_put(f);
}
static void chain_scan_deferred(struct rcu_head *head)
{
	struct dma_fence_chain *owner = container_of(head, struct dma_fence_chain, signal_rcu);
	struct dma_fence *iter = dma_fence_get(&owner->base);
	while (iter) {
		struct dma_fence *payload = dma_fence_get(dma_fence_chain_contained(iter));
		if (!dma_fence_is_signaled(payload) &&
		    !dma_fence_add_callback(payload, &owner->cb, chain_payload_cb)) {
			dma_fence_put(iter);
			return;
		}
		dma_fence_put(payload);
		iter = dma_fence_chain_walk(iter);
	}
	dma_fence_signal(&owner->base);
	dma_fence_put(&owner->base);
}
static bool chain_enable(struct dma_fence *base)
{
	struct dma_fence_chain *chain = to_dma_fence_chain(base);
	/* Keep the node alive while the callback is on any payload. Scanning
	 * after the parent's lock is released avoids nested timeline locks. */
	dma_fence_get(base);
	call_rcu(&chain->signal_rcu, chain_scan_deferred);
	return true;
}
const struct dma_fence_ops dma_fence_chain_ops = {
	.release = chain_release, .signaled = chain_signaled,
	.enable_signaling = chain_enable,
};
void dma_fence_chain_init(struct dma_fence_chain *chain, struct dma_fence *prev,
			  struct dma_fence *fence, uint64_t seqno)
{
	/* Both input references are transferred to the node, as in Linux. */
	rcu_assign_pointer(chain->prev, prev);
	chain->fence = fence;
	chain->prev_seqno = 0;
	u64 context = dma_fence_context_alloc(1);
	if (prev && dma_fence_is_chain(prev)) {
		if (prev->seqno < seqno) {
			context = prev->context;
			chain->prev_seqno = prev->seqno;
		} else seqno = prev->seqno;
	}
	dma_fence_init64(&chain->base, &dma_fence_chain_ops, NULL, context, seqno);
}
static struct dma_fence *chain_previous(struct dma_fence_chain *chain)
{
	rcu_read_lock();
	struct dma_fence *previous = dma_fence_get_rcu_safe(&chain->prev);
	rcu_read_unlock();
	return previous;
}
struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence)
{
	struct dma_fence_chain *chain = to_dma_fence_chain(fence);
	struct dma_fence *prev = NULL;
	while (chain && (prev = chain_previous(chain))) {
		struct dma_fence_chain *node = to_dma_fence_chain(prev);
		if (!dma_fence_is_signaled(node ? node->fence : prev)) break;
		struct dma_fence *next = node ? chain_previous(node) : NULL;
		struct dma_fence *expected = prev;
		if (__atomic_compare_exchange_n(&chain->prev, &expected, next, false,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			dma_fence_put(prev); /* Drop the replaced link's reference. */
		else
			dma_fence_put(next);
		dma_fence_put(prev); /* Drop our inspection reference. */
	}
	dma_fence_put(fence);
	return prev;
}
int dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno)
{
	if (!pfence || !*pfence) return -EINVAL;
	if (!seqno) return 0;
	struct dma_fence *head = *pfence;
	if (!to_dma_fence_chain(head) || head->seqno < seqno) return -EINVAL;
	struct dma_fence *iter = dma_fence_get(head);
	for (;;) {
		struct dma_fence_chain *chain = to_dma_fence_chain(iter);
		if (!chain || iter->context != head->context || chain->prev_seqno < seqno)
			break;
		iter = dma_fence_chain_walk(iter);
		if (!iter) break;
	}
	dma_fence_put(head);
	*pfence = iter;
	return 0;
}

static void array_release(struct dma_fence *base)
{
	struct dma_fence_array *array = to_dma_fence_array(base);
	for (unsigned i = 0; i < array->num_fences; i++)
		dma_fence_put(array->fences[i]);
	kfree(array->fences);
	dma_fence_free(base);
}
static bool array_signaled(struct dma_fence *base)
{
	struct dma_fence_array *array = to_dma_fence_array(base);
	for (unsigned i = 0; i < array->num_fences; i++) {
		bool signaled = dma_fence_is_signaled(array->fences[i]);
		if (array->signal_on_any && signaled) return true;
		if (!array->signal_on_any && !signaled) return false;
	}
	return !array->signal_on_any;
}
static void array_signal_deferred(struct rcu_head *head)
{
	struct dma_fence_array *array = container_of(head, struct dma_fence_array, signal_rcu);
	dma_fence_signal(&array->base);
	dma_fence_put(&array->base);
}
static void array_complete_one(struct dma_fence_array *array)
{
	if (atomic_dec_and_test(&array->num_pending))
		call_rcu(&array->signal_rcu, array_signal_deferred);
	else
		dma_fence_put(&array->base);
}
static void array_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	(void)fence;
	array_complete_one(container_of(cb, struct dma_fence_array_cb, cb)->array);
}
static bool array_enable(struct dma_fence *base)
{
	struct dma_fence_array *array = to_dma_fence_array(base);
	atomic_set(&array->num_pending, array->signal_on_any ? 1 : array->num_fences);
	for (unsigned i = 0; i < array->num_fences; i++) {
		array->callbacks[i].array = array;
		dma_fence_get(base);
		if (dma_fence_add_callback(array->fences[i], &array->callbacks[i].cb,
					   array_callback))
			array_complete_one(array);
	}
	return atomic_read(&array->num_pending) > 0;
}
const struct dma_fence_ops dma_fence_array_ops = {
	.release = array_release, .signaled = array_signaled, .enable_signaling = array_enable,
};
struct dma_fence_array *dma_fence_array_alloc(int count)
{
	if (count <= 0 || (size_t)count > (SIZE_MAX - sizeof(struct dma_fence_array)) /
					 sizeof(struct dma_fence_array_cb)) return NULL;
	return kzalloc(sizeof(struct dma_fence_array) + (size_t)count *
		       sizeof(struct dma_fence_array_cb), GFP_KERNEL);
}
void dma_fence_array_init(struct dma_fence_array *array, int count,
	struct dma_fence **fences, u64 context, unsigned seqno, bool any)
{
	array->num_fences = count;
	array->fences = fences;
	array->signal_on_any = any;
	atomic_set(&array->num_pending, any ? 1 : count);
	dma_fence_init64(&array->base, &dma_fence_array_ops, NULL, context, seqno);
}
struct dma_fence_array *dma_fence_array_create(int count, struct dma_fence **fences,
	u64 context, unsigned seqno, bool any)
{
	if (!fences) return NULL;
	for (int i = 0; i < count; i++) if (!fences[i]) return NULL;
	struct dma_fence_array *array = dma_fence_array_alloc(count);
	if (array) dma_fence_array_init(array, count, fences, context, seqno, any);
	return array;
}
struct dma_fence *dma_fence_array_first(struct dma_fence *head)
{
	struct dma_fence_array *array = to_dma_fence_array(head);
	return array ? (array->num_fences ? array->fences[0] : NULL) : head;
}
struct dma_fence *dma_fence_array_next(struct dma_fence *head, unsigned index)
{
	struct dma_fence_array *array = to_dma_fence_array(head);
	return array && index < array->num_fences ? array->fences[index] : NULL;
}
bool dma_fence_match_context(struct dma_fence *fence, u64 context)
{
	struct dma_fence *item;
	unsigned index;
	dma_fence_array_for_each(item, index, fence)
		if (item->context != context) return false;
	return true;
}

static struct dma_fence *unwrap_current(struct dma_fence_unwrap *cursor)
{
	while (cursor->chain) {
		cursor->array = dma_fence_chain_contained(cursor->chain);
		cursor->index = 0;
		struct dma_fence *fence = dma_fence_array_first(cursor->array);
		if (fence) return fence;
		cursor->chain = dma_fence_chain_walk(cursor->chain);
	}
	return NULL;
}
struct dma_fence *dma_fence_unwrap_first(struct dma_fence *head,
					 struct dma_fence_unwrap *cursor)
{
	cursor->chain = dma_fence_get(head);
	return unwrap_current(cursor);
}
struct dma_fence *dma_fence_unwrap_next(struct dma_fence_unwrap *cursor)
{
	struct dma_fence *fence = dma_fence_array_next(cursor->array, ++cursor->index);
	if (fence) return fence;
	cursor->chain = dma_fence_chain_walk(cursor->chain);
	return unwrap_current(cursor);
}
struct dma_fence *__dma_fence_unwrap_merge(unsigned count, struct dma_fence **fences,
					 struct dma_fence_unwrap *cursors)
{
	unsigned capacity = 0;
	struct dma_fence *item;
	/* Flatten first: nested containers otherwise turn repeated merges into
	 * unbounded recursion through signaled/enable/release callbacks. */
	for (unsigned i = 0; i < count; i++) {
		dma_fence_unwrap_for_each(item, &cursors[i], fences[i]) {
			if (dma_fence_is_signaled(item)) continue;
			if (capacity == INT_MAX) { dma_fence_put(cursors[i].chain); return NULL; }
			capacity++;
		}
	}
	if (!capacity) return dma_fence_get_stub();
	struct dma_fence **owned = kcalloc(capacity, sizeof(*owned), GFP_KERNEL);
	if (!owned) return NULL;
	unsigned n = 0;
	for (unsigned i = 0; i < count; i++) {
		dma_fence_unwrap_for_each(item, &cursors[i], fences[i]) {
			if (!dma_fence_is_signaled(item)) owned[n++] = dma_fence_get(item);
		}
	}
	if (!n) { kfree(owned); return dma_fence_get_stub(); }
	if (n == 1) { item = owned[0]; kfree(owned); return item; }
	struct dma_fence_array *array = dma_fence_array_create(n, owned,
						 dma_fence_context_alloc(1), 1, false);
	if (array) return &array->base;
	for (unsigned i = 0; i < n; i++) dma_fence_put(owned[i]);
	kfree(owned);
	return NULL;
}

/* drivers/dma-buf/dma-fence-unwrap.c: order by context, newest first within a
 * context, so the dedup pass keeps the most recent fence of each context. */
static int fence_cmp(const void *_a, const void *_b)
{
	struct dma_fence *a = *(struct dma_fence **)_a;
	struct dma_fence *b = *(struct dma_fence **)_b;

	if (a->context < b->context)
		return -1;
	else if (a->context > b->context)
		return 1;

	if (dma_fence_is_later(b, a))
		return 1;
	else if (dma_fence_is_later(a, b))
		return -1;

	return 0;
}

/* Sort and deduplicate in place, dropping the references held by the
 * discarded entries. Returns the number of unique fences left. An empty
 * array stays empty (Linux would report one entry for num_fences == 0). */
int dma_fence_dedup_array(struct dma_fence **fences, int num_fences)
{
	int i, j;

	if (num_fences <= 0)
		return 0;

	sort(fences, num_fences, sizeof(*fences), fence_cmp, NULL);

	j = 0;
	for (i = 1; i < num_fences; i++) {
		if (fences[i]->context == fences[j]->context)
			dma_fence_put(fences[i]);
		else
			fences[++j] = fences[i];
	}

	return ++j;
}
