#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "dext_heap_backend.h"

extern int kmemcheck_verify_all(void);
extern size_t kmemcheck_live_bytes(void);

static void test_sizes(void)
{
	for (size_t delta = 0; delta < 64; delta++)
		assert(kmalloc(SIZE_MAX - delta, GFP_KERNEL) == NULL);
	assert(IS_ERR(memdup_user_nul("", SIZE_MAX)));
	assert(kmem_cache_create("overflow", SIZE_MAX, 0, 0, NULL) == NULL);
	assert(kmem_cache_create("truncate", (size_t)UINT_MAX + 1, 0, 0, NULL) == NULL);
	assert(kmem_cache_create("zero", 0, 0, 0, NULL) == NULL);
	for (size_t size = 1; size <= 31; size++) {
		unsigned char *p = kmalloc(size, GFP_KERNEL);
		assert(p && ksize(p) == size);
		assert((uintptr_t)p % ARCH_DMA_MINALIGN == 0);
		memset(p, 0xa5, size);
		assert(kmemcheck_verify_all() == 0);
		kfree(p);
	}
	dext_heap_test_fail_after(0);
	assert(kmalloc(32, GFP_KERNEL) == NULL);
	assert(kmem_cache_create("failed", 32, 0, 0, NULL) == NULL);
	dext_heap_test_fail_after(-1);
	for (size_t alignment = 8; alignment <= 4096; alignment *= 2) {
		struct kmem_cache *cache = kmem_cache_create("aligned", 13, alignment, 0, NULL);
		assert(cache);
		void *p = kmem_cache_alloc(cache, GFP_KERNEL);
		assert(p && (uintptr_t)p % alignment == 0);
		kmem_cache_free(cache, p);
		kmem_cache_destroy(cache);
	}
}

static void test_capacity_and_reuse(void)
{
	struct kmem_cache *caches[256];
	for (size_t i = 0; i < 256; i++) {
		caches[i] = kmem_cache_create("capacity", 13, 0, 0, NULL);
		assert(caches[i]);
		void *p = kmem_cache_alloc(caches[i], GFP_KERNEL);
		assert(p);
		kmem_cache_free(caches[i], p);
	}
	assert(kmem_cache_create("full", 13, 0, 0, NULL) == NULL);
	for (size_t i = 0; i < 256; i += 2) {
		kmem_cache_destroy(caches[i]);
		caches[i] = NULL;
	}
	/* An earlier empty side slot must not hide a later live cache's list. */
	for (size_t i = 1; i < 256; i += 2) {
		void *p = kmem_cache_alloc(caches[i], GFP_KERNEL);
		assert(p);
		kmem_cache_free(caches[i], p);
	}
	for (size_t i = 0; i < 256; i += 2) {
		caches[i] = kmem_cache_create("replacement", 1, 0, 0, NULL);
		assert(caches[i]);
		void *p = kmem_cache_alloc(caches[i], GFP_KERNEL);
		assert(p);
		kmem_cache_free(caches[i], p);
	}
	for (size_t i = 0; i < 256; i++)
		kmem_cache_destroy(caches[i]);
	/* More lifetimes than the side-table size must reclaim every slot. */
	for (size_t i = 0; i < 512; i++) {
		struct kmem_cache *cache = kmem_cache_create("retry", 32, 0, 0, NULL);
		assert(cache);
		void *p = kmem_cache_alloc(cache, GFP_KERNEL);
		assert(p);
		kmem_cache_free(cache, p);
		kmem_cache_destroy(cache);
	}
	kmem_cache_destroy(NULL);
}

struct worker {
	struct kmem_cache *cache;
	unsigned char pattern;
};

static void *use_cache(void *context)
{
	struct worker *w = context;
	for (int round = 0; round < 1000; round++) {
		unsigned char *p = kmem_cache_zalloc(w->cache, GFP_KERNEL);
		assert(p);
		for (size_t i = 0; i < 32; i++)
			assert(p[i] == 0);
		memset(p, w->pattern, 32);
		sched_yield();
		for (size_t i = 0; i < 32; i++)
			assert(p[i] == w->pattern);
		kmem_cache_free(w->cache, p);
		p = kmalloc(7, GFP_KERNEL);
		assert(p);
		kfree(p);
	}
	return NULL;
}

static void test_concurrent_cache_and_scan(void)
{
	struct kmem_cache *cache = kmem_cache_create("concurrent", 32, 0, 0, NULL);
	assert(cache);
	struct worker workers[4];
	pthread_t threads[4];
	for (size_t i = 0; i < 4; i++) {
		workers[i] = (struct worker){cache, (unsigned char)(i + 1)};
		assert(pthread_create(&threads[i], NULL, use_cache, &workers[i]) == 0);
	}
	for (int round = 0; round < 100; round++)
		assert(kmemcheck_verify_all() == 0);
	for (size_t i = 0; i < 4; i++)
		assert(pthread_join(threads[i], NULL) == 0);
	kmem_cache_destroy(cache);
}

int main(void)
{
	test_sizes();
	test_capacity_and_reuse();
	test_concurrent_cache_and_scan();
	assert(kmemcheck_verify_all() == 0);
	assert(kmemcheck_live_bytes() == 0);
	assert(dext_heap_test_live_allocations() == 0);
	puts("DriverKit heap: allocation overflow, cache exhaustion/reuse, concurrent ownership and odd-size canaries passed");
	return 0;
}
