/* Exercise unchanged upstream syncobj creation/replacement/timeline code. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/slab.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_print.h>
#include "dext_heap_backend.h"

/* File/eventfd/task notification is outside this object-lifetime test. */
struct eventfd_ctx;
void eventfd_ctx_put(struct eventfd_ctx *ctx) { (void)ctx; assert(0); }
void __drm_dev_dbg(struct _ddebug *desc, const struct device *dev,
	enum drm_debug_category category, const char *format, ...)
{ (void)desc; (void)dev; (void)category; (void)format; }

static const struct dma_fence_ops ops = {0};
static struct dma_fence *make_fence(void)
{
	struct dma_fence *f = kmalloc(sizeof(*f), GFP_KERNEL); assert(f);
	dma_fence_init64(f, &ops, NULL, dma_fence_context_alloc(1), 1); return f;
}
static void drain(void) { for (int i = 0; i < 16; i++) rcu_barrier(); }
struct shared { struct drm_syncobj *obj; int finished; unsigned reads; };
static void *reader(void *arg)
{
	struct shared *s = arg;
	while (!__atomic_load_n(&s->finished, __ATOMIC_ACQUIRE)) {
		struct dma_fence *f = drm_syncobj_fence_get(s->obj);
		if (f) { assert(dma_fence_was_initialized(f)); dma_fence_put(f); }
		s->reads++;
	}
	return NULL;
}
int main(void)
{
	struct drm_syncobj *obj;
	assert(!drm_syncobj_create(&obj, 0, NULL));
	struct shared shared = { .obj = obj }; pthread_t thread;
	assert(!pthread_create(&thread, NULL, reader, &shared));
	for (unsigned i = 0; i < 2048; i++) {
		struct dma_fence *f = make_fence();
		drm_syncobj_replace_fence(obj, f); dma_fence_put(f);
		if (!(i % 16)) drm_syncobj_replace_fence(obj, NULL);
	}
	__atomic_store_n(&shared.finished, 1, __ATOMIC_RELEASE);
	pthread_join(thread, NULL); assert(shared.reads);
	drm_syncobj_replace_fence(obj, NULL);
	struct dma_fence *one = make_fence(), *two = make_fence();
	struct dma_fence_chain *first = dma_fence_chain_alloc(), *last = dma_fence_chain_alloc();
	assert(first && last);
	drm_syncobj_add_point(obj, first, one, 10);
	drm_syncobj_add_point(obj, last, two, 20);
	struct dma_fence *found = drm_syncobj_fence_get(obj);
	assert(found == &last->base);
	assert(!dma_fence_chain_find_seqno(&found, 10)); assert(found == &first->base);
	dma_fence_put(found);
	found = drm_syncobj_fence_get(obj);
	assert(!dma_fence_chain_find_seqno(&found, 15)); assert(found == &last->base);
	dma_fence_put(found);
	found = drm_syncobj_fence_get(obj);
	assert(dma_fence_chain_find_seqno(&found, 21) == -EINVAL);
	assert(found == &last->base); dma_fence_put(found);
	dma_fence_signal(two); found = drm_syncobj_fence_get(obj);
	assert(!dma_fence_is_signaled(found));
	dma_fence_signal(one); assert(dma_fence_is_signaled(found));
	dma_fence_put(found); dma_fence_put(one); dma_fence_put(two); drm_syncobj_put(obj);
	drain(); assert(!dext_heap_test_live_allocations());
	puts("PASS unchanged upstream drm_syncobj: 2048 concurrent fence replacements, timeline lookup, out-of-order completion and release");
	return 0;
}
