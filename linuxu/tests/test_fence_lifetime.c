#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/fs.h>
#include <linux/file.h>
#include "dext_heap_backend.h"

static const struct dma_fence_ops ops = {0};
static struct dma_fence *make_fence(void)
{
	struct dma_fence *fence = kmalloc(sizeof(*fence), GFP_KERNEL);
	assert(fence);
	dma_fence_init64(fence, &ops, NULL, dma_fence_context_alloc(1), 1);
	return fence;
}
static void drain(void) { for (int i = 0; i < 16; i++) rcu_barrier(); }
static int callback_count;
static void freeing_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{ (void)fence; callback_count++; kfree(cb); }
static void count_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{ (void)fence; (void)cb; callback_count++; }
static void callbacks(void)
{
	struct dma_fence *fence = make_fence();
	struct dma_fence_cb cb;
	for (int i = 0; i < 3; i++) {
		struct dma_fence_cb *owned = kmalloc(sizeof(*owned), GFP_KERNEL);
		assert(dma_fence_add_callback(fence, owned, freeing_callback) == 0);
	}
	assert(dma_fence_add_callback(fence, &cb, count_callback) == 0);
	assert(dma_fence_remove_callback(fence, &cb));
	assert(!dma_fence_remove_callback(fence, &cb));
	dma_fence_signal_timestamp(fence, 123456);
	assert(callback_count == 3);
	assert(dma_fence_timestamp(fence) == 123456);
	assert(dma_fence_add_callback(fence, &cb, count_callback) == -ENOENT);
	assert(callback_count == 3);
	dma_fence_put(fence);
}
static int enable_count;
static bool enable_false(struct dma_fence *fence) { (void)fence; enable_count++; return false; }
static void enable(void)
{
	static const struct dma_fence_ops enabled_ops = { .enable_signaling = enable_false };
	struct dma_fence *f = make_fence(); f->ops = &enabled_ops;
	dma_fence_enable_sw_signaling(f); dma_fence_enable_sw_signaling(f);
	assert(enable_count == 1 && dma_fence_is_signaled(f)); dma_fence_put(f);
}
static void *signal_later(void *arg)
{ usleep(15000); dma_fence_signal(arg); return NULL; }
static void waits(void)
{
	struct dma_fence *fences[2] = {make_fence(), make_fence()};
	assert(dma_fence_wait_timeout(fences[0], false, 0) == 0);
	assert(dma_fence_wait_timeout(fences[0], false, -1) == -EINVAL);
	pthread_t thread; assert(!pthread_create(&thread, NULL, signal_later, fences[1]));
	uint32_t index = 99;
	long result = dma_fence_wait_any_timeout(fences, 2, false, msecs_to_jiffies(2000), &index);
	assert(result > 0 && index == 1);
	pthread_join(thread, NULL);
	assert(!dma_fence_is_signaled(fences[0]));
	assert(!pthread_create(&thread, NULL, signal_later, fences[0]));
	assert(!dma_fence_wait(fences[0], false)); pthread_join(thread, NULL);
	dma_fence_put(fences[0]); dma_fence_put(fences[1]);
	u64 first = dma_fence_context_alloc(1024), second = dma_fence_context_alloc(1);
	assert(second == first + 1024);
}
static void lifetime(void)
{
	struct dma_fence *fence = make_fence();
	rcu_read_lock(); dma_fence_put(fence);
	assert(!dma_fence_get_rcu(fence));
	rcu_read_unlock(); drain();
}
static void containers(void)
{
	struct dma_fence *one = make_fence(), *two = make_fence();
	struct dma_fence_chain *first = dma_fence_chain_alloc(), *last = dma_fence_chain_alloc();
	assert(first && last);
	dma_fence_chain_init(first, NULL, dma_fence_get(one), 1);
	dma_fence_chain_init(last, &first->base, dma_fence_get(two), 2);
	struct dma_fence_cb cb;
	int before = callback_count;
	assert(dma_fence_add_callback(&last->base, &cb, count_callback) == 0);
	dma_fence_signal(two);
	assert(!dma_fence_is_signaled(&last->base));
	assert(callback_count == before);
	/* Owner may drop the timeline before pending child notifications finish. */
	dma_fence_put(&last->base);
	dma_fence_signal(one); drain();
	assert(callback_count == before + 1);
	dma_fence_put(one); dma_fence_put(two); drain();
	one = make_fence(); two = make_fence();
	struct dma_fence *merged = dma_fence_unwrap_merge(one, two);
	assert(merged && kref_read(&one->refcount) == 2);
	dma_fence_signal(one); assert(!dma_fence_is_signaled(merged));
	dma_fence_signal(two); assert(dma_fence_is_signaled(merged));
	dma_fence_put(merged); dma_fence_put(one); dma_fence_put(two);
}
static void *scan_long_chain(void *arg)
{
	struct dma_fence *head = arg;
	assert(!dma_fence_is_signaled(head));
	dma_fence_enable_sw_signaling(head);
	return NULL;
}
static void long_chain(void)
{
	struct dma_fence *pending = make_fence(), *head = NULL;
	for (unsigned i = 0; i < 4096; i++) {
		struct dma_fence_chain *node = dma_fence_chain_alloc(); assert(node);
		dma_fence_chain_init(node, head, i ? dma_fence_get_stub() : dma_fence_get(pending), i + 1);
		head = &node->base;
	}
	pthread_attr_t attr; pthread_attr_init(&attr);
	assert(!pthread_attr_setstacksize(&attr, 64 * 1024));
	pthread_t thread; assert(!pthread_create(&thread, &attr, scan_long_chain, head));
	pthread_join(thread, NULL); pthread_attr_destroy(&attr);
	dma_fence_signal(pending);
	assert(dma_fence_wait_timeout(head, false, 100) > 0);
	dma_fence_put(head); dma_fence_put(pending);
	for (unsigned i = 0; i < 8192 && dext_heap_test_live_allocations(); i++) rcu_barrier();
	assert(!dext_heap_test_live_allocations());
}
static void stub_oom(void)
{
	dext_heap_test_fail_after(0);
	struct dma_fence *stub = dma_fence_get_stub();
	assert(stub && dma_fence_is_signaled(stub));
	dma_fence_put(stub);
	assert(!dma_fence_allocate_private_stub(1));
	dext_heap_test_fail_after(-1);
}
static void *collect_history(void *opaque)
{
	struct dma_fence *head = opaque;
	for (unsigned i = 0; i < 100; i++)
		assert(!dma_fence_chain_walk(dma_fence_get(head)));
	return NULL;
}
static void history_reclamation(void)
{
	struct dma_fence *head = NULL;
	for (unsigned i = 0; i < 2048; i++) {
		struct dma_fence_chain *node = dma_fence_chain_alloc(); assert(node);
		dma_fence_chain_init(node, head, dma_fence_get_stub(), i + 1);
		head = &node->base;
	}
	pthread_t readers[2];
	for (unsigned i = 0; i < 2; i++) assert(!pthread_create(&readers[i], NULL, collect_history, head));
	for (unsigned i = 0; i < 2; i++) assert(!pthread_join(readers[i], NULL));
	/* Detached predecessors still own their former links until each RCU
	 * callback runs. Reader interleaving can defer one release per epoch;
	 * a barrier only covers callbacks queued before that barrier started. */
	for (unsigned i = 0; i < 4096 && dext_heap_test_live_allocations() != 1; i++)
		rcu_barrier();
	assert(dext_heap_test_live_allocations() == 1); /* The live head survives. */
	struct dma_fence *point = dma_fence_get(head);
	assert(!dma_fence_chain_find_seqno(&point, 1) && !point);
	struct dma_fence_chain *reset = dma_fence_chain_alloc(); assert(reset);
	dma_fence_chain_init(reset, head, dma_fence_get_stub(), 2);
	assert(reset->base.context != head->context && reset->prev_seqno == 0);
	assert(reset->base.seqno == head->seqno);
	dma_fence_put(&reset->base); drain();
	assert(!dext_heap_test_live_allocations());
}
static void reservations(void)
{
	struct dma_resv resv, copy; dma_resv_init(&resv); dma_resv_init(&copy);
	struct dma_fence *fences[10];
	assert(!dma_resv_lock(&resv, NULL));
	assert(!dma_resv_reserve_fences(&resv, 10));
	for (unsigned i = 0; i < 10; i++) fences[i] = make_fence();
	dext_heap_test_fail_after(0);
	for (unsigned i = 0; i < 10; i++)
		dma_resv_add_fence(&resv, fences[i], i < 3 ? i : DMA_RESV_USAGE_BOOKKEEP);
	dext_heap_test_fail_after(-1);
	dma_resv_unlock(&resv);
	assert(dma_resv_usage_rw(false) == DMA_RESV_USAGE_WRITE);
	assert(dma_resv_usage_rw(true) == DMA_RESV_USAGE_READ);
	for (unsigned usage = 0; usage < 4; usage++) {
		unsigned count; struct dma_fence **owned;
		assert(!dma_resv_get_fences(&resv, usage, &count, &owned));
		assert(count == (usage < 3 ? usage + 1 : 10));
		for (unsigned i = 0; i < count; i++) dma_fence_put(owned[i]);
		kfree(owned);
	}
	assert(!dma_resv_copy_fences(&copy, &resv));
	struct dma_fence *singleton = NULL;
	assert(!dma_resv_get_singleton(&copy, DMA_RESV_USAGE_BOOKKEEP, &singleton));
	assert(singleton && dma_fence_is_array(singleton));
	for (unsigned i = 0; i < 9; i++) dma_fence_signal(fences[i]);
	assert(!dma_fence_is_signaled(singleton));
	assert(!dma_resv_test_signaled(&resv, DMA_RESV_USAGE_BOOKKEEP));
	assert(dma_resv_wait_timeout(&resv, DMA_RESV_USAGE_BOOKKEEP, false, 0) == 0);
	assert(dma_resv_wait_timeout(&resv, DMA_RESV_USAGE_READ, false, 0) > 0);
	struct dma_resv_iter cursor;
	dma_resv_iter_begin(&cursor, &resv, DMA_RESV_USAGE_BOOKKEEP);
	assert(dma_resv_iter_first_unlocked(&cursor) == fences[0]);
	unsigned refs = kref_read(&fences[0]->refcount);
	dma_resv_iter_end(&cursor);
	assert(kref_read(&fences[0]->refcount) == refs - 1);
	dma_fence_signal(fences[9]);
	assert(dma_resv_wait_timeout(&resv, DMA_RESV_USAGE_BOOKKEEP, false, 10) > 0);
	dma_resv_fini(&resv); dma_resv_fini(&copy); dma_fence_put(singleton);
	for (unsigned i = 0; i < 10; i++) dma_fence_put(fences[i]);
}
static void sync_files(void)
{
	struct dma_fence *fence = make_fence();
	assert(!sync_file_create(NULL));
	for (long budget = 0; budget < 2; budget++) {
		dext_heap_test_fail_after(budget);
		struct sync_file *sf = sync_file_create(fence);
		dext_heap_test_fail_after(-1);
		assert(!sf);
		assert(kref_read(&fence->refcount) == 1);
	}
	struct sync_file *sf = sync_file_create(fence);
	assert(sf && sf->file && sf->file->private_data == sf);
	assert(kref_read(&fence->refcount) == 2);
	struct file *file = get_file(sf->file);
	assert(file && file->f_count == 2);
	sync_file_put(sf);
	assert(file->f_count == 1 && kref_read(&fence->refcount) == 2);
	fput(file);
	assert(kref_read(&fence->refcount) == 1);
	sf = sync_file_create(fence);
	assert(sf);
	int fd = get_unused_fd_flags(O_CLOEXEC);
	assert(fd >= 0);
	fd_install(fd, sf->file); /* The descriptor owns the initial file reference. */
	struct dma_fence *imported = sync_file_get_fence(fd);
	assert(imported == fence && kref_read(&fence->refcount) == 3);
	assert(!close_fd(fd));
	assert(kref_read(&fence->refcount) == 2);
	dma_fence_put(imported);
	assert(kref_read(&fence->refcount) == 1);
	assert(!sync_file_get_fence(fd));
	static const struct file_operations other_operations = {0};
	file = anon_inode_getfile("other", &other_operations, NULL, O_RDWR);
	assert(!IS_ERR(file));
	fd = get_unused_fd_flags(0);
	assert(fd >= 0);
	fd_install(fd, file);
	assert(!sync_file_get_fence(fd));
	assert(!close_fd(fd));
	assert(!sync_file_get_fence(-1));
	dma_fence_put(fence);
}
int main(void)
{
	dext_heap_test_enable_abort_trace();
	stub_oom(); callbacks(); enable(); waits(); lifetime(); containers(); reservations(); sync_files(); drain();
	long_chain();
	history_reclamation();
	assert(!dext_heap_test_live_allocations());
	puts("PASS fence lifetime: freeing callbacks, completion contract, deadlines, RCU, 4096-node timeline on 64 KiB stack, chain/array dependencies, stub OOM, reservation snapshots/usage/reserved OOM/>8 fences");
	return 0;
}
