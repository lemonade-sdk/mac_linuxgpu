/* Linux fence ownership and callback semantics over the shim's CPU locks. */
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <rt/task.h>
#include <rt/park.h>

#include <linux/jiffies.h>	/* CONFIG_HZ */
#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif

/* Interruptible waits stop for the waiting task's signals. A thread without
 * a task has none; do not create one just to look. */
static bool fence_signal_pending(void)
{
	return signal_pending(linuxu_current_task_peek());
}

static uint64_t fence_now_ns(void)
{
	return ktime_get_ns();
}

void dma_fence_init(struct dma_fence *fence, const struct dma_fence_ops *ops,
		    spinlock_t *lock, u64 context, u64 seqno)
{
	memset(fence, 0, sizeof(*fence));
	fence->flags = 1ul << DMA_FENCE_FLAG_INITIALIZED_BIT;
	if (lock)
		fence->extern_lock = lock;
	else {
		spin_lock_init(&fence->inline_lock);
		fence->flags |= 1ul << DMA_FENCE_FLAG_INLINE_LOCK_BIT;
	}
	rcu_assign_pointer(fence->ops, ops);
	fence->context = context;
	fence->seqno = seqno;
	INIT_LIST_HEAD(&fence->cb_list);
	kref_init(&fence->refcount);
	init_waitqueue_head(&fence->waitq);
	INIT_LIST_HEAD(&fence->waiter_entry.entry);
}

void dma_fence_init64(struct dma_fence *fence, const struct dma_fence_ops *ops,
		      spinlock_t *lock, u64 context, u64 seqno)
{
	dma_fence_init(fence, ops, lock, context, seqno);
	set_bit(DMA_FENCE_FLAG_SEQNO64_BIT, &fence->flags);
}

static void fence_free_rcu(struct rcu_head *head)
{
	struct dma_fence *fence = container_of(head, struct dma_fence, rcu);
	kfree(fence);
}

void dma_fence_free(struct dma_fence *fence)
{
	/* Readers such as drm_syncobj_fence_get may still hold an RCU pointer
	 * while attempting get_unless_zero after the last owning reference. */
	call_rcu(&fence->rcu, fence_free_rcu);
}

void dma_fence_release(struct kref *ref)
{
	struct dma_fence *fence = container_of(ref, struct dma_fence, refcount);
	if (fence->ops && fence->ops->release)
		fence->ops->release(fence);
	else
		dma_fence_free(fence);
}

void dma_fence_signal_timestamp_locked(struct dma_fence *fence, ktime_t timestamp)
{
	struct list_head pending;
	if (!fence || test_and_set_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return;
	INIT_LIST_HEAD(&pending);
	list_splice_init(&fence->cb_list, &pending);
	fence->timestamp = timestamp;
	set_bit(DMA_FENCE_FLAG_TIMESTAMP_BIT, &fence->flags);
	/* A callback may free its own container. Unlink before invoking and
	 * never read that callback's node again. Caller holds the fence lock. */
	while (!list_empty(&pending)) {
		struct dma_fence_cb *cb = list_first_entry(&pending, struct dma_fence_cb, node);
		list_del_init(&cb->node);
		cb->func(fence, cb);
	}
}

void dma_fence_signal_locked(struct dma_fence *fence)
{
	dma_fence_signal_timestamp_locked(fence, (ktime_t)fence_now_ns());
}

void dma_fence_signal_timestamp(struct dma_fence *fence, ktime_t timestamp)
{
	if (!fence) return;
	dma_fence_get(fence);
	rcu_read_lock();
	spinlock_t *lock = dma_fence_spinlock(fence);
	spin_lock(lock);
	dma_fence_signal_timestamp_locked(fence, timestamp);
	spin_unlock(lock);
	rcu_read_unlock();
	dma_fence_put(fence);
}

void dma_fence_signal(struct dma_fence *fence)
{
	dma_fence_signal_timestamp(fence, (ktime_t)fence_now_ns());
}

static bool fence_enable_locked(struct dma_fence *fence)
{
	if (dma_fence_is_signaled_locked(fence))
		return false;
	if (!test_and_set_bit(DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT, &fence->flags) &&
	    fence->ops && fence->ops->enable_signaling &&
	    !fence->ops->enable_signaling(fence)) {
		dma_fence_signal_locked(fence);
		return false;
	}
	return !dma_fence_test_signaled_flag(fence);
}

void dma_fence_enable_sw_signaling(struct dma_fence *fence)
{
	if (!fence) return;
	rcu_read_lock();
	spinlock_t *lock = dma_fence_spinlock(fence);
	spin_lock(lock);
	fence_enable_locked(fence);
	spin_unlock(lock);
	rcu_read_unlock();
}

int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
			   dma_fence_func_t func)
{
	if (!fence || !cb || !func) return -EINVAL;
	INIT_LIST_HEAD(&cb->node);
	cb->func = func;
	rcu_read_lock();
	spinlock_t *lock = dma_fence_spinlock(fence);
	spin_lock(lock);
	int result = -ENOENT;
	if (fence_enable_locked(fence)) {
		list_add_tail(&cb->node, &fence->cb_list);
		result = 0;
	}
	spin_unlock(lock);
	rcu_read_unlock();
	/* Linux callers handle -ENOENT themselves; invoking here causes double
	 * completion/free for callers which run their callback on failure. */
	return result;
}

bool dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	if (!fence || !cb) return false;
	bool removed = false;
	rcu_read_lock();
	spinlock_t *lock = dma_fence_spinlock(fence);
	spin_lock(lock);
	if (!dma_fence_test_signaled_flag(fence) && !list_empty(&cb->node)) {
		list_del_init(&cb->node);
		removed = true;
	}
	spin_unlock(lock);
	rcu_read_unlock();
	return removed;
}

bool dma_fence_check_and_signal(struct dma_fence *fence)
{
	return fence && dma_fence_is_signaled(fence);
}
bool dma_fence_check_and_signal_locked(struct dma_fence *fence)
{
	return fence && dma_fence_is_signaled_locked(fence);
}

static signed long fence_remaining(uint64_t start, signed long timeout)
{
	if (timeout == LONG_MAX) return timeout;
	uint64_t elapsed = (fence_now_ns() - start) / (1000000000ull / CONFIG_HZ);
	return elapsed >= (uint64_t)timeout ? 0 : timeout - (signed long)elapsed;
}

/* Fence waits park until a fence's callback fires (dma_fence_signal runs
 * it from the signalling path, the GPU's interrupt for hardware fences) or
 * the timeout, bounded by the backstop (rt/park.h). */
struct fence_park {
	unsigned int fired;
};
struct fence_park_cb {
	struct dma_fence_cb base;
	struct fence_park *park;
};

static void fence_park_fired(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct fence_park *park = container_of(cb, struct fence_park_cb, base)->park;

	(void)fence;
	__atomic_store_n(&park->fired, 1, __ATOMIC_SEQ_CST);
	linuxu_unpark(park);
}

static bool fence_park_still(const void *p)
{
	return !__atomic_load_n(&((const struct fence_park *)p)->fired, __ATOMIC_SEQ_CST);
}

static signed long fences_wait(struct dma_fence **fences, uint32_t count, bool intr,
			       signed long timeout, uint32_t *idx, const void *caller)
{
	struct fence_park park = { 0 };
	struct fence_park_cb one, *cbs = count == 1 ? &one : NULL;
	bool *armed, armed_one = false;
	uint64_t start = fence_now_ns(), backstop = linuxu_park_backstop_first();
	bool backstopped = false;
	signed long ret = 0;

	if (count > 1) {
		cbs = kcalloc(count, sizeof(*cbs) + sizeof(*armed), GFP_KERNEL);
		if (!cbs)
			return -ENOMEM;
		armed = (bool *)(cbs + count);
	} else {
		armed = &armed_one;
	}
	for (uint32_t i = 0; i < count; i++) {
		cbs[i].park = &park;
		armed[i] = !dma_fence_add_callback(fences[i], &cbs[i].base, fence_park_fired);
	}
	for (;;) {
		const unsigned int fired = __atomic_load_n(&park.fired, __ATOMIC_SEQ_CST);
		long remaining = fence_remaining(start, timeout);
		bool done = false, interrupted = false;
		uint64_t now, until;

		for (uint32_t i = 0; i < count && !done; i++) {
			if (dma_fence_is_signaled(fences[i])) {
				if (idx) *idx = i;
				done = true;
			} else if (intr && (__atomic_load_n(&fences[i]->wait_interrupted, __ATOMIC_ACQUIRE) ||
					    fence_signal_pending())) {
				interrupted = true;
			}
		}
		if (done) {
			if (backstopped && !fired)
				linuxu_wait_missed(NULL, caller);
			ret = remaining ? remaining : 1;
			break;
		}
		if (interrupted) {
			ret = -ERESTARTSYS;
			break;
		}
		if (!remaining) {
			ret = 0;
			break;
		}
		now = fence_now_ns();
		until = now + backstop;
		if (timeout != LONG_MAX) {
			const uint64_t deadline = start + (uint64_t)timeout * (1000000000ull / CONFIG_HZ);

			if (deadline < until)
				until = deadline;
		}
		if (linuxu_park(&park, fence_park_still, &park, until) == LINUXU_PARK_WOKEN) {
			backstopped = false;
			backstop = linuxu_park_backstop_first();
		} else {
			backstopped = true;
			backstop = linuxu_park_backstop_next(backstop);
		}
	}
	/* After removal no callback of ours runs: park may go. */
	for (uint32_t i = 0; i < count; i++)
		if (armed[i])
			dma_fence_remove_callback(fences[i], &cbs[i].base);
	if (count > 1)
		kfree(cbs);
	return ret;
}

signed long dma_fence_default_wait(struct dma_fence *fence, bool intr, signed long timeout)
{
	if (timeout < 0) return -EINVAL;
	if (!fence || dma_fence_is_signaled(fence)) return timeout ? timeout : 1;
	if (!timeout) return 0;
	dma_fence_enable_sw_signaling(fence);
	return fences_wait(&fence, 1, intr, timeout, NULL, __builtin_return_address(0));
}

signed long dma_fence_wait_timeout(struct dma_fence *fence, bool intr, signed long timeout)
{
	if (timeout < 0) return -EINVAL;
	if (!fence) return timeout ? timeout : 1;
	if (fence->ops && fence->ops->wait && fence->ops->wait != dma_fence_default_wait)
		return fence->ops->wait(fence, intr, timeout);
	if (dma_fence_is_signaled(fence)) return timeout ? timeout : 1;
	if (!timeout) return 0;
	dma_fence_enable_sw_signaling(fence);
	return fences_wait(&fence, 1, intr, timeout, NULL, __builtin_return_address(0));
}

signed long dma_fence_wait(struct dma_fence *fence, bool intr)
{
	long ret = dma_fence_wait_timeout(fence, intr, LONG_MAX);
	return ret < 0 ? ret : 0;
}

signed long dma_fence_wait_any_timeout(struct dma_fence **fences, uint32_t count,
				       bool intr, signed long timeout, uint32_t *idx)
{
	if (!fences || !count || timeout < 0) return -EINVAL;
	for (uint32_t i = 0; i < count; i++) {
		if (!fences[i]) return -EINVAL;
		if (timeout) dma_fence_enable_sw_signaling(fences[i]);
	}
	if (!timeout) {
		for (uint32_t i = 0; i < count; i++)
			if (dma_fence_is_signaled(fences[i])) {
				if (idx) *idx = i;
				return 1;
			}
		return 0;
	}
	return fences_wait(fences, count, intr, timeout, idx, __builtin_return_address(0));
}

int dma_fence_get_status(struct dma_fence *fence)
{
	return fence && dma_fence_is_signaled(fence) ? (fence->error ?: 1) : 0;
}
void dma_fence_set_deadline(struct dma_fence *fence, ktime_t deadline)
{
	if (fence && fence->ops && fence->ops->set_deadline)
		fence->ops->set_deadline(fence, deadline);
}
int dma_fence_begin_signalling(void) { return 0; }
void dma_fence_end_signalling(int cookie) { (void)cookie; }
void dma_fence_describe(struct dma_fence *fence, struct seq_file *seq)
{ (void)fence; (void)seq; }

struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp)
{
	static const struct dma_fence_ops ops = {0};
	struct dma_fence *fence = kmalloc(sizeof(*fence), GFP_KERNEL);
	if (fence) {
		dma_fence_init64(fence, &ops, NULL, 0, 0);
		dma_fence_signal_timestamp(fence, timestamp);
	}
	return fence;
}
static pthread_once_t stub_once = PTHREAD_ONCE_INIT;
static struct dma_fence stub;
static const struct dma_fence_ops stub_ops = {0};
static void stub_init(void)
{
	dma_fence_init64(&stub, &stub_ops, NULL, 0, 0);
	dma_fence_signal_timestamp_locked(&stub, (ktime_t)fence_now_ns());
}
struct dma_fence *dma_fence_get_stub(void)
{
	/* Linux callers rely on an infallible, permanently referenced stub. */
	pthread_once(&stub_once, stub_init);
	return dma_fence_get(&stub);
}

static atomic64_t contexts = ATOMIC64_INIT(1);
u64 dma_fence_context_alloc(unsigned num)
{
	return (u64)atomic64_fetch_add(num, &contexts);
}
const char __rcu *dma_fence_driver_name(struct dma_fence *fence)
{
	return fence && fence->ops && fence->ops->get_driver_name ?
		fence->ops->get_driver_name(fence) : "unknown";
}
const char __rcu *dma_fence_timeline_name(struct dma_fence *fence)
{
	return fence && fence->ops && fence->ops->get_timeline_name ?
		fence->ops->get_timeline_name(fence) : "unknown";
}
